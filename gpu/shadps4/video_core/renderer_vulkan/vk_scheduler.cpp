// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <thread>
#include <chrono>
#include <algorithm>
#include <cstdio>
#include "bbport_tsc.h"
#ifdef _WIN32
#include <windows.h>
#endif
#include <cstdlib>
#include <unordered_map>
#ifndef _WIN32
#include <dlfcn.h>
#endif
#include <functional>

#include "bbport_copy.h"
#include "bbport_owner_wait.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/landing_gate.h"
#include "bbport_toggles.h"
#include "common/assert.h"
#include "common/debug.h"
#include "common/thread.h"
#include "imgui/renderer/texture_manager.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "bbport_threads.h"

namespace Vulkan {

std::mutex Scheduler::submit_mutex;

extern void (*g_gpu_stall_report)();
bool (*g_draw_pipe_busy)() = nullptr;
// g2-arm64: the latest GPU checkpoint recorded (BB_GPU_CHECKPOINTS, vk_rasterizer.cpp).
u64 (*g_checkpoint_seq)() = nullptr;

namespace {
// bbport: the last submissions to the graphics queue (all schedulers share it), printed when a
// GPU wait stalls: which timeline value each one signals and which semaphores it waits for.
struct SubmitRecord {
    const Scheduler* scheduler;
    VkSemaphore timeline;
    u64 signal;
    u32 waits;
    std::array<VkSemaphore, 4> wait_semaphore;
    std::array<u64, 4> wait_value;
    u64 checkpoint; // the latest checkpoint at submission (its draws are the ones up to it)
    std::chrono::steady_clock::time_point time;
};
std::mutex g_recent_mutex;
std::array<SubmitRecord, 32> g_recent;
u64 g_recent_count;
std::vector<std::pair<const Scheduler*, const Instance*>> g_schedulers;
} // namespace

void Scheduler::ReportStall() {
    std::scoped_lock lk{g_recent_mutex};
    for (const auto& [scheduler, inst] : g_schedulers) {
        const VkSemaphore timeline = scheduler->work_semaphore.Handle();
        const auto [result, value] = inst->GetDevice().getSemaphoreCounterValue(timeline);
        std::printf("GPU stall: scheduler %p timeline %p: GPU reached %llu, next tick %llu\n",
                    static_cast<const void*>(scheduler), static_cast<void*>(timeline),
                    static_cast<unsigned long long>(value),
                    static_cast<unsigned long long>(scheduler->work_semaphore.CurrentTick()));
    }
    const u64 count = std::min<u64>(g_recent_count, g_recent.size());
    for (u64 i = g_recent_count - count; i < g_recent_count; ++i) {
        const auto& r = g_recent[i % g_recent.size()];
        std::printf("GPU stall: submit #%llu scheduler %p signals %p=%llu, waits",
                    static_cast<unsigned long long>(i), static_cast<const void*>(r.scheduler),
                    static_cast<void*>(r.timeline), static_cast<unsigned long long>(r.signal));
        for (u32 w = 0; w < r.waits; ++w) {
            std::printf(" %p=%llu", static_cast<void*>(r.wait_semaphore[w]),
                        static_cast<unsigned long long>(r.wait_value[w]));
        }
        if (g_checkpoint_seq) {
            std::printf(", checkpoints up to #%llu", static_cast<unsigned long long>(r.checkpoint));
        }
        std::printf(", %lld ms ago\n",
                    static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                               std::chrono::steady_clock::now() - r.time)
                                               .count()));
    }
    std::fflush(stdout);
}

Scheduler::Scheduler(const Instance& instance, bool threaded_recording)
    : instance{instance}, work_semaphore{instance}, command_pool{instance, &work_semaphore} {
    {
        std::scoped_lock lk{g_recent_mutex};
        g_schedulers.emplace_back(this, &instance);
        g_gpu_stall_report = &Scheduler::ReportStall;
    }
    // bbport: BB_VK_RECORD_THREAD=0 records on the calling thread.
    const char* env = std::getenv("BB_VK_RECORD_THREAD");
    if (threaded_recording && !(env && env[0] == '0')) {
        record_chunk = AcquireChunk();
        recorder_thread = std::jthread(std::bind_front(&Scheduler::RecorderThread, this));
        recording_threaded = true;
    }
#if TRACY_GPU_ENABLED
    profiler_scope = reinterpret_cast<tracy::VkCtxScope*>(std::malloc(sizeof(tracy::VkCtxScope)));
#endif
    AllocateWorkerCommandBuffers();
    priority_pending_ops_thread =
        std::jthread(std::bind_front(&Scheduler::PriorityPendingOpsThread, this));
}

Scheduler::~Scheduler() {
    {
        std::scoped_lock lk{g_recent_mutex};
        std::erase_if(g_schedulers, [this](const auto& entry) { return entry.first == this; });
    }
    if (recorder_thread.joinable()) {
        SyncRecording();
        recorder_thread.request_stop();
        recorder_cv.notify_all();
        recorder_thread.join();
    }
#if TRACY_GPU_ENABLED
    std::free(profiler_scope);
#endif
}

void Scheduler::BeginRendering(const RenderState& new_state) {
    FrontCheck check{this, "BeginRendering"};
    if (is_rendering && render_state == new_state) {
        return;
    }
    EndRendering();
    is_rendering = true;
    render_state = new_state;

    std::array<vk::RenderingAttachmentInfo, 8> color_attachments;
    for (u32 i = 0; i < render_state.num_color_attachments; ++i) {
        const auto& cb = render_state.color_attachments[i];
        color_attachments[i] = vk::RenderingAttachmentInfo{
            .imageView = cb.image_view,
            .imageLayout = cb.image_layout,
            .loadOp = cb.is_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .clearValue = vk::ClearValue{.color = vk::ClearColorValue{.uint32 = cb.clear_value}},
        };
    }

    const auto& db = render_state.depth_stencil_attachment;
    const vk::RenderingAttachmentInfo depth_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.depth_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue =
            vk::ClearValue{.depthStencil = vk::ClearDepthStencilValue{.depth = std::bit_cast<float>(
                                                                          db.clear_value[0])}},
    };
    const vk::RenderingAttachmentInfo stencil_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.stencil_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = vk::ClearValue{.depthStencil =
                                         vk::ClearDepthStencilValue{.stencil = db.clear_value[1]}},
    };

    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .offset = {0, 0},
                .extent = {render_state.width, render_state.height},
            },
        .layerCount = render_state.num_layers,
        .colorAttachmentCount = render_state.num_color_attachments,
        .pColorAttachments = color_attachments.data(),
        .pDepthAttachment = db.has_depth ? &depth_attachment : nullptr,
        .pStencilAttachment = db.has_stencil ? &stencil_attachment : nullptr,
    };

    if (!recording_threaded) {
        current_cmdbuf.beginRendering(rendering_info);
        return;
    }
    // The attachment infos live on this stack frame: the recorded closure keeps copies.
    Record([info = rendering_info, color_attachments, depth_attachment,
            stencil_attachment](vk::CommandBuffer cmdbuf) mutable {
        info.pColorAttachments = color_attachments.data();
        if (info.pDepthAttachment) {
            info.pDepthAttachment = &depth_attachment;
        }
        if (info.pStencilAttachment) {
            info.pStencilAttachment = &stencil_attachment;
        }
        cmdbuf.beginRendering(info);
    });
}

void Scheduler::EndRendering() {
    FrontCheck check{this, "EndRendering"};
    if (!is_rendering) {
        return;
    }
    is_rendering = false;
    Record([](vk::CommandBuffer cmdbuf) { cmdbuf.endRendering(); });
}

void Scheduler::TraceDirectRecording(void* caller) {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_RECORDER_TRACE");
        return env && env[0] == '1';
    }();
    if (!enabled) {
        return;
    }
    static std::mutex mutex;
    static std::unordered_map<void*, u64> callers;
    static u64 calls;
    std::scoped_lock lk{mutex};
    ++callers[caller];
    if (++calls % 2000) {
        return;
    }
    std::vector<std::pair<u64, void*>> top;
    for (const auto& [address, count] : callers) {
        top.emplace_back(count, address);
    }
    std::ranges::sort(top, std::greater{});
    for (size_t i = 0; i < std::min<size_t>(top.size(), 8); ++i) {
#ifdef _WIN32
        struct { const char* dli_fname; void* dli_fbase; } info{nullptr, nullptr};
#else
        Dl_info info{};
        dladdr(top[i].second, &info);
#endif
        std::printf("Recorder sync caller: %llu x %s+0x%lx\n",
                    static_cast<unsigned long long>(top[i].first),
                    info.dli_fname ? info.dli_fname : "?",
                    static_cast<unsigned long>(reinterpret_cast<uintptr_t>(top[i].second) -
                                               reinterpret_cast<uintptr_t>(info.dli_fbase)));
    }
    callers.clear();
}

#ifndef _WIN32
void PrintHostBacktrace(const char* prefix); // vk_semaphore.cpp
#endif

void Scheduler::ReportFront(const char* what, const char* other) {
    static std::mutex report_mutex;
    static u32 reports = 0;
    std::scoped_lock lk{report_mutex};
    if (reports >= 24) {
        return;
    }
    ++reports;
    char name[32] = "?";
#ifndef _WIN32
    pthread_getname_np(pthread_self(), name, sizeof(name));
#endif
    std::printf("Record check: scheduler %p: %s on thread '%s' while another thread is in %s\n",
                static_cast<const void*>(this), what, name, other ? other : "?");
#ifndef _WIN32
    PrintHostBacktrace("Record check:");
#endif
    std::fflush(stdout);
}

void Scheduler::CheckDirectOwner(const char* what) {
    const uintptr_t me = bb_thread_pointer();
    uintptr_t owner = 0;
    if (!direct_owner.compare_exchange_strong(owner, me, std::memory_order_acq_rel) && owner != me) {
        ReportFront(what, "direct mode taken by another thread (raw command buffer held there)");
    }
}

void Scheduler::CheckDirect(const char* what) {
    bool busy;
    {
        std::scoped_lock lk{recorder_mutex};
        busy = recorder_busy || !recorder_queue.empty();
    }
    if (busy) {
        ReportFront(what, "the recording thread (commands queued or running)");
    }
}

std::unique_ptr<RecordChunk> Scheduler::AcquireChunk() {
    std::scoped_lock lk{recorder_mutex};
    if (free_chunks.empty()) {
        return std::make_unique<RecordChunk>();
    }
    auto chunk = std::move(free_chunks.back());
    free_chunks.pop_back();
    return chunk;
}

void Scheduler::SignalAfterHostCopies(std::function<void()> signal, u64 landing_seq) {
    if (!IsRecordingDeferred()) {
        WaitHostCopies();
        if (VideoCore::LandingGate::Active()) {
            WaitDeferredSignals();
        }
        VideoCore::LandingGate::Wait(landing_seq);
        signal();
        return;
    }
    BbCopy::FlushBatch();
    deferred_signals_issued.fetch_add(1, std::memory_order_relaxed);
    Record([signal = std::move(signal), done = deferred_signals_done,
            landing_seq](vk::CommandBuffer) mutable {
        BbCopy::AfterCopies([signal = std::move(signal), done = std::move(done), landing_seq] {
            auto fire = [signal = std::move(signal), done = std::move(done)] {
                signal();
                done->fetch_add(1, std::memory_order_release);
            };
            // A signal behind deferred raycaster copies waits for them on the landing thread:
            // this thread must not block (submissions wait for the copy threads).
            VideoCore::LandingGate::Run(landing_seq, std::move(fire));
        });
    });
    KickRecording(true);
}

void Scheduler::DescribeStall() const {
    std::fprintf(stderr,
                 "STALL: recording thread: %zu chunks queued, %s, %s; host copies %llu of %llu "
                 "done, deferred submissions %llu of %llu, deferred signals %llu of %llu\n",
                 queued_chunks.load(std::memory_order_relaxed), recorder_busy ? "busy" : "idle",
                 recorder_sleeping ? "sleeping" : "awake",
                 static_cast<unsigned long long>(host_copies_done.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(host_copies_issued),
                 static_cast<unsigned long long>(deferred_submits_done.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(deferred_submits_issued),
                 static_cast<unsigned long long>(deferred_signals_done->load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(deferred_signals_issued.load(std::memory_order_relaxed)));
}

void Scheduler::WaitDeferredSignals() {
    const u64 issued = deferred_signals_issued.load(std::memory_order_relaxed);
    if (deferred_signals_done->load(std::memory_order_acquire) >= issued ||
        BbToggle::Disabled(BbToggle::OrderedGuestWrites)) {
        return;
    }
    BbStats::WaitTimer timer{BbStats::host_copies_wait_ns};
    const BbOwnerWait::Scope owner_waits; // g2-arm64: the signals may need the owner
    KickRecording(true);
    const auto begin = std::chrono::steady_clock::now();
    u32 spins = 0;
    bool reported = false;
    while (deferred_signals_done->load(std::memory_order_acquire) < issued) {
        if (VideoCore::LandingGate::Stopped()) {
            break; // shutdown: the labels waiting on the landing thread were dropped
        }
        // Helps the copy threads the signals wait for.
        BbCopy::WaitAsync();
        std::this_thread::yield();
        // g2-arm64: a wait that never ends leaves a line (the stall report comes later).
        if (!reported && (++spins & 1023) == 0 &&
            std::chrono::steady_clock::now() - begin > std::chrono::seconds(5)) {
            reported = true;
            std::printf("GPU stall: waiting 5 s for the deferred signals (%llu of %llu done)\n",
                        static_cast<unsigned long long>(
                            deferred_signals_done->load(std::memory_order_relaxed)),
                        static_cast<unsigned long long>(issued));
        }
    }
}

void Scheduler::WaitHostCopies() {
    // g2-arm64: a fence label on the recording or a copy thread may need the owner meanwhile.
    const BbOwnerWait::Scope owner_waits;
    if (host_copies_done.load(std::memory_order_acquire) < host_copies_issued) {
        BbStats::WaitTimer timer{BbStats::host_copies_wait_ns};
        BbStats::host_copy_waits.fetch_add(1, std::memory_order_relaxed);
        KickRecording(true);
        while (host_copies_done.load(std::memory_order_acquire) < host_copies_issued) {
            std::this_thread::yield();
        }
    }
    BbStats::WaitTimer timer{BbStats::copy_threads_wait_ns};
    BbCopy::WaitAsync();
}

void Scheduler::KickRecording(bool force) {
    FrontCheck check{this, "KickRecording"};
    if (!recording_threaded) {
        return;
    }
    if (check.on) {
        direct_owner.store(0, std::memory_order_release);
    }
    // Callers kick where nobody holds the raw command buffer: deferral resumes.
    direct_mode = false;
    // Batches of tens of KiB keep the queue handoff cheap relative to the work it carries.
    if (!force && full_chunks.empty() && record_chunk->Size() < 32 * 1024) {
        return;
    }
    if (full_chunks.empty() && record_chunk->Empty()) {
        return;
    }
    // g2-arm64 (from uncharted_Linux 53fba30): the replacement is taken before the current chunk
    // is handed over, so record_chunk is never null (see RetireChunk). AcquireChunk() takes
    // recorder_mutex itself.
    std::unique_ptr<RecordChunk> next = record_chunk->Empty() ? nullptr : AcquireChunk();
    bool wake;
    {
        std::scoped_lock lk{recorder_mutex};
        for (auto& chunk : full_chunks) {
            recorder_queue.push_back(std::move(chunk));
        }
        if (next) {
            recorder_queue.push_back(std::exchange(record_chunk, std::move(next)));
        }
        wake = recorder_sleeping;
        queued_chunks.store(recorder_queue.size(), std::memory_order_release);
    }
    full_chunks.clear();
    // A busy recorder picks the new chunks up by itself: waking it is a syscall per draw.
    if (wake) {
        recorder_cv.notify_one();
    }
}

void Scheduler::SyncRecording() {
    FrontCheck check{this, "SyncRecording"};
    if (!recording_threaded) {
        return;
    }
    // g2-arm64 diagnosis: BB_SYNC_STATS=1 prints every 5 s how often and how long recording
    // threads waited here, with what was queued for the Vulkan recording thread on entry.
    static const bool stats = std::getenv("BB_SYNC_STATS") != nullptr;
    size_t entry_chunks = 0, entry_bytes = 0;
    bool entry_sleeping = false;
    if (stats) {
        entry_bytes = record_chunk->Size();
        for (const auto& chunk : full_chunks) {
            entry_bytes += chunk->Size();
        }
        entry_chunks = full_chunks.size() + !record_chunk->Empty();
        std::scoped_lock lk{recorder_mutex};
        for (const auto& chunk : recorder_queue) {
            entry_bytes += chunk->Size();
        }
        entry_chunks += recorder_queue.size();
        entry_sleeping = recorder_sleeping;
    }
    const auto start = stats ? std::chrono::steady_clock::now()
                             : std::chrono::steady_clock::time_point{};
    KickRecording(true);
    {
        BbStats::WaitTimer timer{BbStats::sync_recording_ns};
        // g2-arm64: a fault on the recording thread may need the owner meanwhile.
        const BbOwnerWait::Scope owner_waits;
        std::unique_lock lk{recorder_mutex};
        recorder_idle_cv.wait(lk, [this] { return recorder_queue.empty() && !recorder_busy; });
    }
    if (stats) {
        static std::mutex stats_mutex;
        static u64 syncs, ns, max_ns, chunks, bytes, asleep;
        static std::chrono::steady_clock::time_point window_start;
        std::scoped_lock lk{stats_mutex};
        const auto now = std::chrono::steady_clock::now();
        const u64 waited = std::chrono::duration_cast<std::chrono::nanoseconds>(now - start).count();
        ++syncs;
        ns += waited;
        max_ns = std::max(max_ns, waited);
        chunks += entry_chunks;
        bytes += entry_bytes;
        asleep += entry_sleeping;
        if (window_start.time_since_epoch().count() == 0) {
            window_start = now;
        } else if (now - window_start >= std::chrono::seconds(5)) {
            const double secs = std::chrono::duration<double>(now - window_start).count();
            std::printf("Recording syncs over %.1f s: %.0f/s, %.0f us each (max %.0f), %.1f ms/s; "
                        "on entry %.1f chunks, %.1f KiB queued, recorder asleep %.0f%%\n",
                        secs, syncs / secs, ns / 1e3 / syncs, max_ns / 1e3, ns / 1e6 / secs,
                        double(chunks) / syncs, double(bytes) / 1024 / syncs,
                        100.0 * asleep / syncs);
            syncs = ns = max_ns = chunks = bytes = asleep = 0;
            window_start = now;
        }
    }
}

void Scheduler::RecorderThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("bb:VkRecorder");
    BbOwnerWait::helper_thread = true; // g2-arm64: writes fence labels (RecorderFences)
    while (true) {
        // Spin briefly before sleeping: the next chunk usually follows within microseconds,
        // and a sleeping recorder costs the GPU thread a wake-up syscall per kick. With few
        // hardware threads (Steam Deck: 8) the spin would take time from guest threads.
        // g2-arm64: with wfe (bb_wait_word) the core idles until the queue's line is written,
        // so the spin can cover the gaps between kicks within a frame: a sleeping recorder
        // cost the draw recording thread a futex wake per fence it hands over (U1: ~4% of its
        // profile). BB_RECORDER_SPIN_US overrides.
        static const u64 spin_us = [] {
            if (const char* env = std::getenv("BB_RECORDER_SPIN_US")) {
                return static_cast<u64>(std::max(0, std::atoi(env)));
            }
            return u64{BbThreads::Available() >= 12 || bb_wfe_enabled() ? 200u : 20u};
        }();
        const BbSpinDeadline spin_until{spin_us};
        for (u32 spins = 1; queued_chunks.load(std::memory_order_acquire) == 0; ++spins) {
            bb_wait_word(&queued_chunks, size_t{0});
            // Without wfe the clock is read every 256 pauses, not per iteration.
            if ((bb_wfe_enabled() || !(spins & 255)) &&
                (stoken.stop_requested() || spin_until.Passed())) {
                break;
            }
        }
        std::unique_ptr<RecordChunk> chunk;
        {
            std::unique_lock lk{recorder_mutex};
            recorder_sleeping = true;
            recorder_cv.wait(lk, stoken, [this] { return !recorder_queue.empty(); });
            recorder_sleeping = false;
            if (recorder_queue.empty()) {
                return; // stop requested
            }
            chunk = std::move(recorder_queue.front());
            recorder_queue.pop_front();
            queued_chunks.store(recorder_queue.size(), std::memory_order_release);
            recorder_busy = true;
        }
        // current_cmdbuf only changes after SyncRecording(), which waits for this thread, or
        // on this thread (a deferred submission): Execute reads it for every command.
        if (FrontCheck::Wanted() && direct_mode) {
            ReportFront("recording thread runs commands", "direct mode (raw command buffer held)");
        }
        chunk->Execute(current_cmdbuf);
        {
            std::scoped_lock lk{recorder_mutex};
            free_chunks.push_back(std::move(chunk));
            recorder_busy = false;
            if (recorder_queue.empty()) {
                recorder_idle_cv.notify_all();
            }
        }
    }
}

void Scheduler::Flush(SubmitInfo& info) {
    // When flushing, we only send data to the driver; no waiting is necessary.
    SubmitExecution(info);
}

void Scheduler::Flush() {
    SubmitInfo info{};
    Flush(info);
}

void Scheduler::Finish() {
    // When finishing, we need to wait for the submission to have executed on the device.
    const u64 presubmit_tick = CurrentTick();
    SubmitInfo info{};
    SubmitExecution(info);
    Wait(presubmit_tick);
}

void Scheduler::WaitSubmitted() {
    const u64 tick = CurrentTick();
    if (tick > 1) {
        Wait(tick - 1);
    }
}

void Scheduler::Wait(u64 tick) {
    if (tick >= work_semaphore.CurrentTick()) {
        // Make sure we are not waiting for the current tick without signalling
        SubmitInfo info{};
        Flush(info);
    }
    BbStats::WaitTimer timer{BbStats::tick_wait_ns};
    // g2-arm64: the submission may still be on the recording thread (BB_DEFERRED_SUBMIT).
    const BbOwnerWait::Scope owner_waits;
    work_semaphore.Wait(tick);
}

void Scheduler::PopPendingOperations() {
    if (num_pending_ops.load(std::memory_order_acquire) == 0) {
        return; // every draw comes here
    }
    std::unique_lock lk(pending_ops_mutex);
    // bbport: this runs on every draw and dispatch. Querying the timeline semaphore is an
    // ioctl, so it is skipped when nothing waits and done once per 32 calls (~0.3 ms; reading
    // the clock per draw instead was itself a hot spot).
    if (pending_ops.empty()) {
        return;
    }
    if (!work_semaphore.IsFree(pending_ops.front().gpu_tick)) {
        if ((++pending_polls & 31) != 0 && !BbToggle::Disabled(BbToggle::PendingPollLimit)) {
            return;
        }
        work_semaphore.Refresh();
    }
    while (!pending_ops.empty() && work_semaphore.IsFree(pending_ops.front().gpu_tick)) {
        pending_ops.front().callback();
        pending_ops.pop();
        num_pending_ops.fetch_sub(1, std::memory_order_release);
    }
}

void Scheduler::AllocateWorkerCommandBuffers() {
    const vk::CommandBufferBeginInfo begin_info = {
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    };

    current_cmdbuf = command_pool.Commit();
    Check(current_cmdbuf.begin(begin_info));

    // Invalidate dynamic state so it gets applied to the new command buffer.
    dynamic_state.Invalidate();

#if TRACY_GPU_ENABLED
    auto* profiler_ctx = instance.GetProfilerContext();
    if (profiler_ctx) {
        static const auto scope_loc =
            GPU_SCOPE_LOCATION("Guest Frame", MarkersPalette::GpuMarkerColor);
        new (profiler_scope) tracy::VkCtxScope{profiler_ctx, &scope_loc, current_cmdbuf, true};
    }
#endif
}

namespace {
// g2-arm64: BB_DEFERRED_SUBMIT=1: a submission from a thread that records through the Vulkan
// recording thread is itself recorded: that thread ends the command buffer, submits it and
// begins the next once it has recorded everything before. The thread submitting went on at
// once; before, it waited for the recording thread to catch up (SyncRecording). U1 720p:
// ~250 submissions a second (mid-frame flushes), ~600 us each in SyncRecording, ~15% of the
// time - mostly on the draw recording thread. Experimental, off by default: no frame rate
// gained where measured (the flip waits for the GPU, BB_FRAMES_AHEAD), and the device was lost
// in one run of six (2026-10-20; a wait on a tick still deferred?).
bool DeferredSubmitWanted() {
    static const bool wanted = [] {
        const char* env = std::getenv("BB_DEFERRED_SUBMIT");
        return env && env[0] == '1';
    }();
    return wanted;
}
} // namespace

namespace {
/// g2-arm64: ends a command buffer that is about to be submitted; with read backs from mapped
/// GPU memory (g_host_read_barrier) its writes are made visible to the host first.
void EndForSubmission(vk::CommandBuffer cmdbuf) {
    if (g_host_read_barrier) {
        const vk::MemoryBarrier2 to_host = {
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask = vk::AccessFlagBits2::eHostRead,
        };
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &to_host});
    }
    Check(cmdbuf.end());
}
} // namespace

void Scheduler::SubmitExecution(SubmitInfo& info) {
    // g2-arm64: a deferred submission takes submit_mutex on the recording thread: none may be
    // pending while this thread holds it and waits for that thread. Direct recording (or
    // deferral off) means the recording thread has nothing queued.
    FrontCheck check{this, "SubmitExecution"};
    const bool defer = DeferredSubmitWanted() && IsRecordingDeferred();
    if (defer && DeferredSubmitsPending() && queue_work_pending && queue_work_pending()) {
        // The submit callback's sparse binds go to the queue now: earlier deferred
        // submissions reach it first, as before.
        SyncRecording();
    }
    std::scoped_lock lk{submit_mutex};
    // bbport: BB_SUBMIT_THREADS=1 names each thread that submits through this scheduler (once).
    static const bool submit_threads = std::getenv("BB_SUBMIT_THREADS") != nullptr;
#ifdef _WIN32
    if (submit_threads) {
        static std::mutex seen_mutex;
        static std::vector<std::pair<const Scheduler*, u32>> seen;
        const u32 tid = static_cast<u32>(GetCurrentThreadId());
        std::scoped_lock seen_lk{seen_mutex};
        if (std::ranges::find(seen, std::pair<const Scheduler*, u32>{this, tid}) == seen.end()) {
            seen.emplace_back(this, tid);
            PWSTR description = nullptr;
            char name[64] = "?";
            if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &description)) && description) {
                WideCharToMultiByte(CP_UTF8, 0, description, -1, name, sizeof(name), nullptr, nullptr);
                LocalFree(description);
            }
            std::printf("Scheduler %p: first submission from thread %u '%s'%s\n",
                        static_cast<const void*>(this), tid, name, on_submit ? " (draw scheduler)" : "");
        }
    }
    // bbport: the GPU command thread must not submit while the draw recording thread still has
    // work in this command buffer (its sparse binds would miss this submission). Reported once
    // per caller with a stack (module offsets for llvm-symbolizer).
    if (on_submit && g_draw_pipe_busy && g_draw_pipe_busy()) {
        static std::mutex report_mutex;
        static std::vector<void*> reported;
        void* frames[24]{};
        const u16 count = CaptureStackBackTrace(1, 24, frames, nullptr);
        std::scoped_lock report_lk{report_mutex};
        if (count > 2 && std::ranges::find(reported, frames[1]) == reported.end()) {
            reported.push_back(frames[1]);
            const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
            std::printf("Scheduler: GPU thread submits while the draw pipe is busy:");
            for (u16 i = 0; i < count; ++i) {
                std::printf(" %llx", static_cast<unsigned long long>(
                                         reinterpret_cast<uintptr_t>(frames[i]) - base));
            }
            std::printf("\n");
        }
    }
#endif
    const u64 signal_value = work_semaphore.NextTick();

#if TRACY_GPU_ENABLED
    auto* profiler_ctx = instance.GetProfilerContext();
    if (profiler_ctx) {
        profiler_scope->~VkCtxScope();
        TracyVkCollect(profiler_ctx, current_cmdbuf);
    }
#endif

    if (on_submit) {
        on_submit(info);
    }

    EndRendering();
    if (auto* profiler = GpuProfiler::Get(); profiler && profiler->Records(this)) {
        // Until the next submission's first timestamp: mostly the GPU waiting for it.
        profiler->Mark(GpuProfiler::IdleKey,
                       [] { return std::string{"(between submissions: GPU idle)"}; });
    }
    if (defer && IsRecordingDeferred()) {
        ++deferred_submits_issued;
        Record([this, info, signal_value](vk::CommandBuffer cmdbuf) mutable {
            SubmitRecorded(cmdbuf, info, signal_value);
        });
        // Commands recorded from here on go to the next command buffer: its dynamic state
        // starts unset.
        dynamic_state.Invalidate();
        KickRecording(true);
        PopPendingOperations();
        return;
    }
    SyncRecording();
    // Guest memory copies into staging read by this submission (copy threads).
    WaitHostCopies();
    EndForSubmission(current_cmdbuf);
    QueueSubmit(current_cmdbuf, info, signal_value);
    work_semaphore.Refresh();
    AllocateWorkerCommandBuffers();

    // Apply pending operations
    PopPendingOperations();
}

void Scheduler::SubmitRecorded(vk::CommandBuffer cmdbuf, SubmitInfo& info, u64 signal_value) {
    // Guest memory copies into staging read by this submission: this thread ran its own
    // (RecordHostCopy) before; the copy threads' may still run.
    BbCopy::WaitAsync();
    EndForSubmission(cmdbuf);
    {
        std::scoped_lock lk{submit_mutex};
        QueueSubmit(cmdbuf, info, signal_value);
    }
    work_semaphore.Refresh();
    current_cmdbuf = command_pool.Commit();
    Check(current_cmdbuf.begin(vk::CommandBufferBeginInfo{
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    }));
    deferred_submits_done.fetch_add(1, std::memory_order_release);
}

void Scheduler::QueueSubmit(vk::CommandBuffer cmdbuf, SubmitInfo& info, u64 signal_value) {
    const vk::Semaphore timeline = work_semaphore.Handle();
    info.AddSignal(timeline, signal_value);

    static constexpr std::array<vk::PipelineStageFlags, 2> wait_stage_masks = {
        vk::PipelineStageFlagBits::eAllCommands,
        vk::PipelineStageFlagBits::eColorAttachmentOutput,
    };

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .waitSemaphoreValueCount = info.num_wait_semas,
        .pWaitSemaphoreValues = info.wait_ticks.data(),
        .signalSemaphoreValueCount = info.num_signal_semas,
        .pSignalSemaphoreValues = info.signal_ticks.data(),
    };

    const vk::SubmitInfo submit_info = {
        .pNext = &timeline_si,
        .waitSemaphoreCount = info.num_wait_semas,
        .pWaitSemaphores = info.wait_semas.data(),
        .pWaitDstStageMask = wait_stage_masks.data(),
        .commandBufferCount = 1U,
        .pCommandBuffers = &cmdbuf,
        .signalSemaphoreCount = info.num_signal_semas,
        .pSignalSemaphores = info.signal_semas.data(),
    };

    {
        std::scoped_lock lk{g_recent_mutex};
        auto& record = g_recent[g_recent_count++ % g_recent.size()];
        record = {this, timeline, signal_value, info.num_wait_semas, {}, {},
                  g_checkpoint_seq ? g_checkpoint_seq() : 0, std::chrono::steady_clock::now()};
        for (u32 w = 0; w < info.num_wait_semas && w < 4; ++w) {
            record.wait_semaphore[w] = info.wait_semas[w];
            record.wait_value[w] = info.wait_ticks[w];
        }
    }
    ImGui::Core::TextureManager::Submit();
    static const bool submit_stats = std::getenv("BB_SYNC_STATS") != nullptr; // g2-arm64
    const auto t0 = submit_stats ? std::chrono::steady_clock::now()
                                 : std::chrono::steady_clock::time_point{};
    auto submit_result = instance.GetGraphicsQueue().submit(submit_info, info.fence);
    if (submit_result == vk::Result::eErrorDeviceLost) {
        std::printf("Device lost during submit\n");
        DeviceLost(instance.GetDevice());
    }
    const auto t1 = submit_stats ? std::chrono::steady_clock::now() : t0;

    if (submit_stats) {
        // Time in vkQueueSubmit, and in getting the next command buffer (it may wait for the
        // GPU to release one).
        static std::mutex stats_mutex; // the schedulers submit from different threads
        static u64 submits, submit_ns, alloc_ns, max_ns;
        static std::chrono::steady_clock::time_point window_start;
        std::scoped_lock stats_lk{stats_mutex};
        const auto t2 = std::chrono::steady_clock::now();
        const auto ns = [](auto d) {
            return u64(std::chrono::duration_cast<std::chrono::nanoseconds>(d).count());
        };
        ++submits;
        submit_ns += ns(t1 - t0);
        alloc_ns += ns(t2 - t1);
        max_ns = std::max(max_ns, ns(t2 - t0));
        if (window_start.time_since_epoch().count() == 0) {
            window_start = t2;
        } else if (t2 - window_start >= std::chrono::seconds(5)) {
            const double secs = std::chrono::duration<double>(t2 - window_start).count();
            std::printf("Submissions over %.1f s: %.0f/s, vkQueueSubmit %.0f us each, next "
                        "command buffer %.0f us each (max both %.0f us)\n",
                        secs, submits / secs, submit_ns / 1e3 / submits, alloc_ns / 1e3 / submits,
                        max_ns / 1e3);
            submits = submit_ns = alloc_ns = max_ns = 0;
            window_start = t2;
        }
    }
}

void Scheduler::PriorityPendingOpsThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:GpuSchedPriorityPendingOpsRunner");

    while (!stoken.stop_requested()) {
        PendingOp op;
        {
            std::unique_lock lk(priority_pending_ops_mutex);
            priority_pending_ops_cv.wait(lk, stoken,
                                         [this] { return !priority_pending_ops.empty(); });
            if (stoken.stop_requested()) {
                break;
            }

            op = std::move(priority_pending_ops.front());
            priority_pending_ops.pop();
        }

        work_semaphore.Wait(op.gpu_tick);
        if (stoken.stop_requested()) {
            break;
        }

        op.callback();
    }
}

void DynamicState::Commit(const Instance& instance, const vk::CommandBuffer& cmdbuf) {
    // A null command buffer only updates the dirty flags, exactly as recording would.
    CommitWith(instance.IsDepthBoundsSupported(), instance.IsDynamicColorWriteMaskSupported(),
               instance.IsAttachmentFeedbackLoopLayoutSupported(), [&](auto&& command) {
                   if (cmdbuf) {
                       command(cmdbuf);
                   }
               });
}

} // namespace Vulkan
