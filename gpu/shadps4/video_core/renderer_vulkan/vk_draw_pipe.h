// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: two-stage draw pipeline (docs/parallel_gpu.md, "Two-stage draw pipeline").
//
// The GPU command thread (stage A) decodes PM4, keeps the register file and selects pipelines;
// for a direct draw it writes a packet (the register blocks changed since the previous packet,
// the stages' user data, the draw parameters) into this ring and goes on decoding. The draw
// recording thread (stage B) owns the caches, the barriers and the command recording while
// packets are in flight: it applies each packet to its own copy of the registers and runs the
// rest of the draw. Anything else stage A does on that state waits for stage B to run dry
// (Drain) and then runs on stage A as before.

#pragma once

#include <cstdlib>
#include "bbport_threads.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include "bbport_tsc.h"

#include "common/assert.h"
#include "common/thread.h"
#include "common/types.h"

namespace Vulkan {

class DrawPipe {
public:
    /// Runs a packet on stage B.
    using Handler = void (*)(void* context, const u8* packet, u32 size);

    DrawPipe(Handler handler_, void* context_) : handler{handler_}, context{context_} {
        ring = std::make_unique<u8[]>(Capacity);
        thread = std::jthread([this](std::stop_token stop) { Run(stop); });
    }

    ~DrawPipe() {
        Drain();
        thread.request_stop();
        published.fetch_add(0, std::memory_order_seq_cst);
        wake.fetch_add(1, std::memory_order_seq_cst);
        wake.notify_one();
    }

    /// True on the draw recording thread.
    [[nodiscard]] static bool OnStageB() noexcept {
        return stage_b_thread.IsCurrent();
    }

    [[nodiscard]] u32 StageBThreadId() const noexcept {
        return stage_b_tid.load(std::memory_order_acquire);
    }

    /// Stage A: space for a packet of `size` bytes (8-byte aligned), valid until Commit().
    u8* Begin(u32 size) {
        size = Align(size + sizeof(Header));
        ASSERT(size <= Capacity / 4);
        u64 at = head;
        const u64 offset = at % Capacity;
        if (offset + size > Capacity) {
            // Wrap: the rest of the ring is skipped (a header with zero size marks it).
            WaitForSpace(at, Capacity - offset);
            reinterpret_cast<Header*>(ring.get() + offset)->size = 0;
            at += Capacity - offset;
            head = at;
            published.store(at, std::memory_order_release);
        }
        WaitForSpace(at, size);
        pending_size = size;
        auto* header = reinterpret_cast<Header*>(ring.get() + at % Capacity);
        header->size = size;
        return reinterpret_cast<u8*>(header + 1);
    }

    /// Stage A: hands the packet from Begin() to stage B.
    void Commit(u32 payload_size) {
        reinterpret_cast<Header*>(ring.get() + head % Capacity)->payload = payload_size;
        head += pending_size;
        published.store(head, std::memory_order_seq_cst);
        ++packets;
        if (sleeping.load(std::memory_order_seq_cst)) {
            wake.fetch_add(1, std::memory_order_seq_cst);
            wake.notify_one();
        }
    }

    /// Why stage A drained (statistics): a PM4 opcode, or one of these.
    enum Reason : u32 {
        ReasonConstRam = 256,
        ReasonCommands,
        ReasonCompute,
        ReasonSubmissionEnd,
        ReasonDraw,
        ReasonRasterizer,
        NumReasons,
    };
    std::array<u64, NumReasons> drains_by_reason{};
    std::array<u64, NumReasons> cycles_by_reason{};

    /// Stage A: waits until stage B has run every committed packet (and recorded the work it
    /// held back, SetHeld).
    void Drain(u32 reason = ReasonRasterizer) {
        if (consumed.load(std::memory_order_acquire) == head &&
            !held_work.load(std::memory_order_acquire)) {
            return;
        }
        drain_requested.store(true, std::memory_order_seq_cst);
        ++drains;
        const u32 slot = reason < NumReasons ? reason : ReasonRasterizer;
        ++drains_by_reason[slot];
        const u64 start = bb_rdtsc();
        // g2-arm64: spin (wfe) for BB_DRAIN_SPIN_US, then sleep until stage B has run the
        // drain's last packet (WakeDrain). A drain at a submission's end waits ~5 ms (U2: 48/s,
        // a quarter of stage A's time), and a spinning stage A held its core meanwhile.
        static const u64 spin_us = [] {
            const char* env = std::getenv("BB_DRAIN_SPIN_US");
            return env ? static_cast<u64>(std::max(0, std::atoi(env))) : u64{50};
        }();
        const BbSpinDeadline spin_until{spin_us};
        for (u32 spins = 0;; ++spins) {
            const u64 at = consumed.load(std::memory_order_seq_cst);
            if (at == head && !held_work.load(std::memory_order_seq_cst)) {
                break;
            }
            if ((bb_wfe_enabled() || !(spins & 255)) && spin_until.Passed()) {
                // Seen, then the flag, then the condition again (seq_cst): stage B either sees
                // the flag after its last store or this check sees that store.
                const u32 seen = drain_wake.load(std::memory_order_seq_cst);
                drain_target.store(head, std::memory_order_relaxed);
                drain_sleeping.store(true, std::memory_order_seq_cst);
                if (!(consumed.load(std::memory_order_seq_cst) == head &&
                      !held_work.load(std::memory_order_seq_cst))) {
                    ++drain_sleeps;
                    drain_wake.wait(seen, std::memory_order_seq_cst);
                }
                drain_sleeping.store(false, std::memory_order_relaxed);
                continue;
            }
            // g2-arm64: wakes on stage B's next write to the line (consumed, held_work).
            if (bb_wfe_enabled()) {
                bb_wait_word(&consumed, at);
            } else if (spins < 4096) {
                bb_cpu_relax();
            } else {
                std::this_thread::yield();
            }
        }
        drain_requested.store(false, std::memory_order_relaxed);
        const u64 waited = bb_rdtsc() - start;
        drain_cycles += waited;
        cycles_by_reason[slot] += waited;
    }

    /// g2-arm64: stage B work held back between packets (the skinning batch,
    /// vk_skin_batch.cpp). The handler records it; stage B runs it when stage A drains and
    /// before it sleeps (any other packet records it first itself). Set before packets flow.
    using IdleHandler = void (*)(void* context);
    void SetIdleHandler(IdleHandler idle) noexcept {
        idle_handler = idle;
    }
    /// Stage B: whether work is held back (set while running the packet, before its position
    /// is published, so a drain sees it).
    void SetHeld(bool held) noexcept {
        held_work.store(held, std::memory_order_seq_cst);
        if (!held) {
            WakeDrain();
        }
    }

    /// Stage A: position after the last committed packet; Reached(position) once B ran it.
    [[nodiscard]] u64 Head() const noexcept {
        return head;
    }
    [[nodiscard]] bool Reached(u64 position) const noexcept {
        return consumed.load(std::memory_order_acquire) >= position;
    }

    /// Stage A (statistics): packets committed that stage B has not run yet.
    [[nodiscard]] u64 PendingPackets() const noexcept {
        return packets - packets_run.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool Idle() const noexcept {
        return consumed.load(std::memory_order_acquire) == head &&
               !held_work.load(std::memory_order_acquire);
    }

    /// Statistics (stage A): packets, drains that had to wait, cycles waited, cycles stage B
    /// spent running packets.
    u64 packets = 0, drains = 0, drain_cycles = 0, drain_sleeps = 0;
    alignas(64) std::atomic<u64> busy_cycles{0}; ///< stage B writes


private:
    struct Header {
        u32 size;    ///< bytes to the next packet (0: wrap to the ring start)
        u32 payload; ///< bytes written by the producer
    };
    static constexpr u64 Capacity = 16ull << 20;

    static constexpr u32 Align(u32 size) {
        return (size + 63) & ~63u;
    }

    void WaitForSpace(u64 at, u64 size) {
        for (u64 run = consumed.load(std::memory_order_acquire); at + size - run > Capacity;
             run = consumed.load(std::memory_order_acquire)) {
            bb_wait_word(&consumed, run);
        }
    }

    void Run(std::stop_token stop) {
        Common::SetCurrentThreadName("bb:DrawRec");
        stage_b_thread.SetCurrent();
        stage_b_tid.store(static_cast<u32>(gettid()), std::memory_order_release);
        u64 at = 0;
        while (true) {
            // Packets follow each other within microseconds while a frame is decoded: spin,
            // and sleep only after a longer pause (between frames).
            u64 available = published.load(std::memory_order_acquire);
            if (available == at) {
                // With few hardware threads (Steam Deck: 8) a long spin takes time from the
                // game's own threads. BB_PIPE_SPIN_US overrides.
                static const u64 spin_us = [] {
                    if (const char* env = std::getenv("BB_PIPE_SPIN_US")) {
                        return static_cast<u64>(std::max(0, std::atoi(env)));
                    }
                    return u64{BbThreads::Available() >= 12 ? 200u : 50u};
                }();
                // g2-arm64: the counter, not a steady_clock call each time the pipe runs dry;
                // with wfe (bb_wait_word) the core idles until stage A writes the line
                // (published, drain_requested).
                const BbSpinDeadline spin_until{spin_us};
                for (u32 spins = 1; available == at; ++spins) {
                    if (stop.stop_requested()) {
                        return;
                    }
                    // g2-arm64: held work goes out at once when stage A drains.
                    if (held_work.load(std::memory_order_relaxed) &&
                        drain_requested.load(std::memory_order_acquire)) {
                        idle_handler(context);
                    }
                    bb_wait_word(&published, at);
                    if ((bb_wfe_enabled() || !(spins & 255)) && spin_until.Passed()) {
                        if (held_work.load(std::memory_order_relaxed)) {
                            idle_handler(context); // not across a sleep
                        }
                        const u32 seen = wake.load(std::memory_order_seq_cst);
                        sleeping.store(true, std::memory_order_seq_cst);
                        if (published.load(std::memory_order_seq_cst) == at &&
                            !stop.stop_requested()) {
                            wake.wait(seen, std::memory_order_seq_cst);
                        }
                        sleeping.store(false, std::memory_order_relaxed);
                    }
                    available = published.load(std::memory_order_acquire);
                }
            }
            const auto* header = reinterpret_cast<const Header*>(ring.get() + at % Capacity);
            if (header->size == 0) {
                at += Capacity - at % Capacity;
                consumed.store(at, std::memory_order_seq_cst);
                WakeDrain();
                continue;
            }
            const u64 start = bb_rdtsc();
            BbStageTax::Pay(BbStageTax::StageB()); // g2-arm64 diagnosis (bbport_tsc.h)
            handler(context, reinterpret_cast<const u8*>(header + 1), header->payload);
            busy_cycles.fetch_add(bb_rdtsc() - start, std::memory_order_relaxed);
            at += header->size;
            packets_run.store(packets_run.load(std::memory_order_relaxed) + 1,
                              std::memory_order_relaxed);
            consumed.store(at, std::memory_order_seq_cst);
            WakeDrain();
        }
    }

    /// Stage B, after it advanced consumed or released held work: wakes a sleeping drain whose
    /// condition now holds (one flag load per packet otherwise).
    void WakeDrain() noexcept {
        if (!drain_sleeping.load(std::memory_order_seq_cst)) {
            return;
        }
        if (consumed.load(std::memory_order_relaxed) == drain_target.load(std::memory_order_relaxed) &&
            !held_work.load(std::memory_order_seq_cst)) {
            drain_sleeping.store(false, std::memory_order_relaxed);
            drain_wake.fetch_add(1, std::memory_order_seq_cst);
            drain_wake.notify_one();
        }
    }

    // g2-arm64: the draw recording thread's identity (a thread_local here went through the
    // dynamic TLS resolver: libbbgpu is loaded with dlopen).
    static inline BbThreadSlot stage_b_thread;
    // g2-arm64: cache lines by writer: stage A wrote head and its statistics next to what
    // stage B reads for every packet.
    alignas(64) Handler handler; ///< read-only while packets flow
    void* context;
    std::unique_ptr<u8[]> ring;
    IdleHandler idle_handler = nullptr;
    alignas(64) u64 head = 0; ///< stage A's write position
    u32 pending_size = 0;
    // Stage A writes this line, stage B waits on it.
    alignas(64) std::atomic<u64> published{0};
    std::atomic<bool> drain_requested{false};
    // Stage B writes this line, stage A waits on it.
    alignas(64) std::atomic<u64> consumed{0};
    std::atomic<bool> held_work{false};
    std::atomic<u64> packets_run{0}; ///< statistics
    alignas(64) std::atomic<u32> wake{0};
    std::atomic<bool> sleeping{false};
    // g2-arm64: a drain asleep (stage A) until consumed reaches drain_target.
    alignas(64) std::atomic<u32> drain_wake{0};
    std::atomic<bool> drain_sleeping{false};
    std::atomic<u64> drain_target{0};
    std::atomic<u32> stage_b_tid{0};
    std::jthread thread;
};

} // namespace Vulkan
