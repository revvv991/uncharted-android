// SPDX-License-Identifier: GPL-2.0-or-later
// g2-arm64: Uncharted 1's GPU skinning (cs 8f067540) batched, and the same way Uncharted 2's
// skinning with motion vectors (cs 7d7e7206, host_shaders/skin_mv_batch.comp: ~95 dispatches a
// frame of 4 workgroups, 5-8% of the GPU's time in the palace). The game dispatches it ~750 times a
// frame, a few hundred vertices each; on the GPU those ran one after another (each a compute
// state change for one workgroup: ~19% of the GPU's frame time at 720p on the G2). Stage B
// collects consecutive direct dispatches of it and records them as one dispatch of
// host_shaders/skin_batch.comp, which does the same arithmetic with each dispatch's buffers by
// device address (BB_SKIN_BATCH=1). Uncharted 3's skinning (cs 0c936dd6, host_shaders/
// skin_u3_batch.comp: U1's algorithm without the tangent, one dispatch of 1024-thread groups per
// character, 93 a frame in the Cartagena market at ~8 us each) joins the same way with
// BB_SKIN_BATCH_U3=1: its batches take 0.27 ms a frame where the dispatches took ~0.75, and the
// chase's frame did not move (-0.1 0.0 0.0 0.0 over 4 pairs on the G2, 2026-11-09: the GPU's gain
// hides behind the display's cadence), so it stays opt-in until a frame shows it.
//
// A dispatch joins the batch after the regular resource binding (BindResources: the same buffer
// lookups, uploads and barrier checks); its buffer accesses go to the barrier tracker when the
// batch is recorded. Dispatches in one batch run concurrently, so one that reads or writes what
// an earlier one in the batch writes (or writes what it reads) starts a new batch. The batch is
// recorded before any other packet, when stage A drains the draw pipe, and before the recording
// thread sleeps (DrawPipe::SetHeld).

#include <dlfcn.h>
#include <algorithm>
#include <array>
#include <unordered_map>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "core/debug_state.h"
#include "core/memory.h"
#include "shader_recompiler/info.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/host_shaders/skin_batch_comp.h"
#include "video_core/host_shaders/skin_mv_batch_comp.h"
#include "video_core/host_shaders/skin_u3_batch_comp.h"
#include "video_core/renderer_vulkan/vk_draw_pipe.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

extern bool g_gpu_checkpoints;

namespace {
constexpr u32 GuestGroupSize = 1024;
constexpr u32 GroupSize = 64; ///< the batch shaders' workgroup
constexpr u32 MaxBuffers = 12;
constexpr u32 MaxJobs = 512;
constexpr u32 MaxGroups = 65535;

/// The batched guest shaders. Their V#s are the inputs, then the outputs: fields of one
/// interleaved output vertex.
struct SkinKind {
    u32 hash;
    u32 num_buffers;
    u32 first_output;
    u32 flat_count; ///< flattened user data word with the vertex count
    u32 flat_scale; ///< ... with the motion scale (0: none)
    std::array<u32, MaxBuffers> read_bytes; ///< inputs: bytes per vertex (0: all of the buffer)
    u32 vertex_bytes;                       ///< the output vertex
    std::array<u32, 4> written_bytes;       ///< the outputs' fields
    const char* label;
};
constexpr std::array<SkinKind, 3> Kinds{{
    // U1: position, normal, tangent.
    {0x8f067540, 9, 6, 52, 0, {8, 12, 8, 8, 0, 0}, 28, {12, 8, 8}, "skinning batch (cs 8f067540)"},
    // U2: position, normal, tangent, motion.
    {0x7d7e7206, 12, 8, 64, 65, {8, 12, 8, 8, 0, 0, 0, 0}, 36, {12, 8, 8, 8},
     "skinning batch (cs 7d7e7206)"},
    // U3: position, normal (g2-arm64, 2026-11-09).
    {0x0c936dd6, 7, 5, 44, 0, {8, 12, 8, 0, 0}, 20, {12, 8}, "skinning batch (cs 0c936dd6)"},
}};

/// g2-arm64: the kind of a compute shader, or Kinds.size() for any other.
constexpr u32 KindIndex(u64 hash) {
    for (u32 k = 0; k < Kinds.size(); ++k) {
        if (Kinds[k].hash == u32(hash)) {
            return k;
        }
    }
    return static_cast<u32>(Kinds.size());
}

/// One dispatch, as the batch shaders read it (std430).
struct SkinJob {
    u64 address[MaxBuffers];
    u32 size[MaxBuffers];
    u32 count;
    u32 first_group;
    u32 scale; ///< the motion scale (float bits)
    u32 pad;
};
static_assert(sizeof(SkinJob) == 160);

struct SkinPush {
    u64 jobs;
    u64 groups;
    u32 num_groups;
    u32 pad;
};

/// Sorted, disjoint [begin, end) ranges of guest memory; touching ones are merged.
class RangeSet {
public:
    [[nodiscard]] bool Overlaps(VAddr begin, VAddr end) const {
        if (begin >= end) {
            return false;
        }
        auto it = std::upper_bound(ranges.begin(), ranges.end(), begin,
                                   [](VAddr at, const Range& r) { return at < r.begin; });
        if (it != ranges.begin() && std::prev(it)->end > begin) {
            return true;
        }
        return it != ranges.end() && it->begin < end;
    }

    void Add(VAddr begin, VAddr end) {
        if (begin >= end) {
            return;
        }
        auto first = std::lower_bound(ranges.begin(), ranges.end(), begin,
                                      [](const Range& r, VAddr at) { return r.end < at; });
        auto last = first;
        while (last != ranges.end() && last->begin <= end) {
            begin = std::min(begin, last->begin);
            end = std::max(end, last->end);
            ++last;
        }
        if (last == first) {
            ranges.insert(first, Range{begin, end});
            return;
        }
        // g2-arm64: merged in place into the first touching range (a range already covered,
        // as the buffers every dispatch shares, moves nothing); before, an erase and an insert
        // each moved the tail of the vector.
        *first = Range{begin, end};
        ranges.erase(first + 1, last);
    }

    void Clear() noexcept {
        ranges.clear();
    }

private:
    struct Range {
        VAddr begin;
        VAddr end;
    };
    std::vector<Range> ranges;
};

bool SkinBatchWanted() {
    static const bool wanted = [] {
        const char* env = std::getenv("BB_SKIN_BATCH");
        return env && env[0] == '1';
    }();
    return wanted;
}

/// BB_SKIN_BATCH_MV=0: U2's 7d7e7206 dispatches one by one again (with BB_SKIN_BATCH=1).
bool MvBatchWanted() {
    static const bool wanted = [] {
        const char* env = std::getenv("BB_SKIN_BATCH_MV");
        return !(env && env[0] == '0');
    }();
    return wanted;
}

/// g2-arm64: BB_SKIN_BATCH_U3=1: U3's 0c936dd6 dispatches batched too (with BB_SKIN_BATCH=1).
bool U3BatchWanted() {
    static const bool wanted = [] {
        const char* env = std::getenv("BB_SKIN_BATCH_U3");
        return env && env[0] == '1';
    }();
    return wanted;
}

bool SkinBatchStats() {
    static const bool stats = std::getenv("BB_SKIN_BATCH_STATS") != nullptr;
    return stats;
}

struct SkinPipeline {
    vk::UniquePipelineLayout layout;
    vk::UniquePipeline pipeline;
};

SkinPipeline CreateSkinPipeline(const Instance& instance, std::span<const u32> code) {
    const auto device = instance.GetDevice();
    SkinPipeline p;
    const vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                      .offset = 0,
                                      .size = sizeof(SkinPush)};
    p.layout = Check(device.createPipelineLayoutUnique({
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &range,
    }));
    const auto module = CompileSPV(code, device);
    p.pipeline = Check(device.createComputePipelineUnique(
        {}, vk::ComputePipelineCreateInfo{
                .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                          .module = module,
                          .pName = "main"},
                .layout = *p.layout,
            }));
    device.destroyShaderModule(module);
    return p;
}
} // namespace

struct Rasterizer::SkinBatch {
    std::array<std::vector<SkinJob>, Kinds.size()> jobs; ///< by kind
    std::array<std::vector<u32>, Kinds.size()> job_of_group;
    std::vector<BoundBuffer> bound;
    RangeSet reads;
    RangeSet writes;
    bool needs_barrier = false;
    u64 tick = 0;

    /// Statistics (BB_SKIN_BATCH_STATS=1): dispatches batched and why batches ended.
    enum Flush : u32 { Packet, Idle, Hazard, Full, Tick, Submit, NumFlush };
    u64 dispatches = 0, batches = 0, regular = 0;
    std::array<u64, NumFlush> flushes{};
    std::unordered_map<u64, u64> causes; ///< packets that ended batches
    std::chrono::steady_clock::time_point window{};
};

void Rasterizer::InitSkinBatch() {
    if (!draw_pipe || !SkinBatchWanted()) {
        return;
    }
    skin_batch = std::make_shared<SkinBatch>();
    skin_batch->window = std::chrono::steady_clock::now();
    draw_pipe->SetIdleHandler(&FlushSkinBatchIdle);
    std::printf("Skin batch: dispatches of cs %08x%s%s run in batches\n", Kinds[0].hash,
                MvBatchWanted() ? ", cs 7d7e7206" : "", U3BatchWanted() ? ", cs 0c936dd6" : "");
}

bool Rasterizer::IsSkinBatchShader(u64 hash) const noexcept {
    if (!skin_batch) {
        return false;
    }
    const u32 k = KindIndex(hash);
    return k == 0 || (k == 1 && MvBatchWanted()) || (k == 2 && U3BatchWanted());
}

namespace {
/// g2-arm64: no job of any kind collected.
template <typename Batch>
bool NoJobs(const Batch& batch) {
    for (const auto& jobs : batch.jobs) {
        if (!jobs.empty()) {
            return false;
        }
    }
    return true;
}
} // namespace

void Rasterizer::FlushSkinBatchIdle(void* rasterizer) {
    auto& self = *static_cast<Rasterizer*>(rasterizer);
    self.FlushSkinBatch(SkinBatch::Idle);
}

bool Rasterizer::AddSkinJob(const ComputePipeline* pipeline,
                            const AmdGpu::ComputeProgram& program, bool& bound) {
    auto& batch = *skin_batch;
    const auto& cs = pipeline->GetStage(Shader::SwStage::Compute);
    const auto flat = cs.FlatUserData();
    const u32 k = std::min<u32>(KindIndex(cs.pgm_hash), Kinds.size() - 1);
    const SkinKind& kind = Kinds[k];
    const u32 num_buffers = kind.num_buffers;
    // What the batch shader assumes of the guest shader and its dispatch; anything else takes
    // the regular path.
    bool fits = DrawPipe::OnStageB() && !FrameCapture::Active() && !g_gpu_checkpoints &&
                cs.buffers.size() >= num_buffers &&
                flat.size() > std::max(kind.flat_count, kind.flat_scale) &&
                program.num_thread_x.full == GuestGroupSize && program.num_thread_y.full == 1 &&
                program.num_thread_z.full == 1 && program.dim_y == 1 && program.dim_z == 1;
    for (u32 i = 0; fits && i < num_buffers; ++i) {
        fits = !cs.buffers[i].IsSpecial() && cs.buffers[i].is_written == (i >= kind.first_output);
    }
    if (!fits) {
        FlushSkinBatch(SkinBatch::Packet);
        ++batch.regular;
        return false;
    }
    const u32 count = static_cast<u32>(
        std::min<u64>(flat[kind.flat_count], u64(program.dim_x) * GuestGroupSize));

    // The guest memory the dispatch reads and writes (its buffers' V#s; the per-vertex inputs and
    // the outputs exactly: fields of the interleaved output vertices).
    auto* memory = Core::Memory::Instance();
    std::array<VAddr, MaxBuffers> base{};
    std::array<u64, MaxBuffers> bytes{};
    for (u32 i = 0; i < num_buffers; ++i) {
        const auto sharp = cs.buffers[i].GetSharp(cs);
        base[i] = sharp.base_address;
        bytes[i] = base[i] && sharp.GetSize() ? memory->ClampRangeSize(base[i], sharp.GetSize())
                                              : 0;
    }
    const auto used = [&](u32 i, u64 size) { return std::min<u64>(bytes[i], size); };
    // Unused entries stay empty ranges: they neither overlap nor add.
    std::array<std::pair<VAddr, VAddr>, MaxBuffers> reads{};
    for (u32 i = 0; i < kind.first_output; ++i) {
        const u64 size = kind.read_bytes[i] ? used(i, u64(count) * kind.read_bytes[i]) : bytes[i];
        reads[i] = {base[i], base[i] + size};
    }
    std::array<std::pair<VAddr, VAddr>, 4> writes{};
    for (u32 i = kind.first_output; i < num_buffers; ++i) {
        const u32 field = kind.written_bytes[i - kind.first_output];
        const u64 size = count ? u64(count - 1) * kind.vertex_bytes + field : 0;
        writes[i - kind.first_output] = {base[i], base[i] + used(i, size)};
    }
    bool hazard = false;
    for (const auto& [begin, end] : reads) {
        hazard |= batch.writes.Overlaps(begin, end);
    }
    for (const auto& [begin, end] : writes) {
        hazard |= batch.writes.Overlaps(begin, end) || batch.reads.Overlaps(begin, end);
    }
    const u32 groups = (count + GroupSize - 1) / GroupSize;
    if (hazard) {
        FlushSkinBatch(SkinBatch::Hazard);
    } else if (batch.jobs[k].size() >= MaxJobs ||
               batch.job_of_group[k].size() + groups > MaxGroups) {
        FlushSkinBatch(SkinBatch::Full);
    } else if (!NoJobs(batch) && scheduler.CurrentTick() != batch.tick) {
        // Constant ring data is retired with the submission it was stamped in.
        FlushSkinBatch(SkinBatch::Tick);
    }

    if (!BindResources(pipeline)) {
        return true; // as the regular path: nothing to dispatch
    }
    bound = true;
    // The device addresses of the bound ranges: constant ring snapshots, or buffer cache
    // buffers in the order BindBuffers bound them.
    SkinJob job{};
    bool harvested = buffer_infos.size() >= num_buffers && bound_images.empty();
    u32 next_bound = 0;
    const vk::Buffer ring = constant_ring ? constant_ring->Handle() : vk::Buffer{};
    for (u32 i = 0; harvested && i < num_buffers; ++i) {
        const auto& info = buffer_infos[i];
        if (!info.buffer) {
            continue; // null descriptor: reads zero, writes dropped
        }
        if (info.buffer == ring) {
            job.address[i] = constant_ring->DeviceAddress() + info.offset;
            job.size[i] = static_cast<u32>(info.range);
            continue;
        }
        if (next_bound >= bound_buffers.size() ||
            bound_buffers[next_bound].buffer->Handle() != info.buffer) {
            harvested = false;
            break;
        }
        const auto& binding = bound_buffers[next_bound++];
        job.address[i] = binding.buffer->BufferDeviceAddress() + binding.offset;
        job.size[i] = binding.size;
    }
    if (!harvested) {
        // Bound for the regular path. The batch goes first; its writes reach the barrier
        // tracker only then, so this dispatch takes a barrier.
        FlushSkinBatch(SkinBatch::Packet);
        needs_barrier = true;
        ++batch.regular;
        return false;
    }

    const bool first_job = NoJobs(batch);
    const u32 index = static_cast<u32>(batch.jobs[k].size());
    job.count = count;
    job.first_group = static_cast<u32>(batch.job_of_group[k].size());
    job.scale = kind.flat_scale ? static_cast<u32>(flat[kind.flat_scale]) : 0;
    batch.job_of_group[k].insert(batch.job_of_group[k].end(), groups, index);
    batch.jobs[k].push_back(job);
    if (SkinBatchStats()) {
        // The first jobs of each kind: are the outputs one interleaved buffer (the batch shaders'
        // row stores)?
        static std::array<u32, Kinds.size()> logged{};
        if (logged[k] < 4) {
            ++logged[k];
            const u32 f = kind.first_output;
            std::printf("Skin batch: cs %08x job, %u vertices, outputs", kind.hash, job.count);
            for (u32 i = f; i < num_buffers; ++i) {
                std::printf(" +%lld/%u", static_cast<long long>(job.address[i] - job.address[f]),
                            job.size[i]);
            }
            std::printf("\n");
        }
    }
    batch.bound.insert(batch.bound.end(), bound_buffers.begin(), bound_buffers.end());
    batch.needs_barrier |= needs_barrier;
    for (const auto& [begin, end] : reads) {
        batch.reads.Add(begin, end);
    }
    for (const auto& [begin, end] : writes) {
        batch.writes.Add(begin, end);
    }
    if (first_job) {
        batch.tick = scheduler.CurrentTick();
        draw_pipe->SetHeld(true);
    }
    ++batch.dispatches;
    DebugState.IncDispatch();
    // ResetBindings without the access tracking: that waits for the batch (no checkpoints:
    // batching is off with them).
    bound_images.clear();
    bound_buffers.clear();
    needs_barrier = false;
    return true;
}

void Rasterizer::FlushSkinBatch(u32 reason, u64 cause) {
    if (!skin_batch || NoJobs(*skin_batch)) {
        return;
    }
    auto& batch = *skin_batch;
    if (reason == SkinBatch::Packet && SkinBatchStats()) {
        ++batch.causes[cause];
    }
    static const std::array<SkinPipeline, Kinds.size()> pipes{
        CreateSkinPipeline(instance, SKIN_BATCH_COMP),
        CreateSkinPipeline(instance, SKIN_MV_BATCH_COMP),
        CreateSkinPipeline(instance, SKIN_U3_BATCH_COMP),
    };
    if (batch.needs_barrier) {
        runtime.FlushBarriers();
    }
    scheduler.EndRendering();
    // The kinds write different vertices (an overlap ends a batch): one dispatch each.
    for (u32 k = 0; k < Kinds.size(); ++k) {
        const u32 num_groups = static_cast<u32>(batch.job_of_group[k].size());
        if (num_groups == 0) {
            continue;
        }
        if (auto* profiler = GpuProfiler::Get()) {
            profiler->Mark(0x5C1Bull + k, [k] { return std::string{Kinds[k].label}; });
        }
        auto& stream = buffer_cache.GetStreamBuffer();
        const u64 jobs_offset =
            stream.Copy(batch.jobs[k].data(), batch.jobs[k].size() * sizeof(SkinJob), 16);
        const u64 groups_offset =
            stream.Copy(batch.job_of_group[k].data(), num_groups * sizeof(u32), 16);
        const u64 address = stream.BufferDeviceAddress();
        const SkinPush push{address + jobs_offset, address + groups_offset, num_groups, 0};
        scheduler.Record([pipeline = *pipes[k].pipeline, layout = *pipes[k].layout,
                          push](vk::CommandBuffer cmdbuf) {
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
            cmdbuf.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
                                 &push);
            cmdbuf.dispatch(push.num_groups, 1, 1);
        });
    }
    for (const auto& bound : batch.bound) {
        const auto write_flag =
            bound.is_written ? vk::AccessFlagBits2::eShaderWrite : vk::AccessFlagBits2::eNone;
        runtime.AccessBuffer(bound.buffer, bound.offset, bound.size,
                             vk::PipelineStageFlagBits2::eComputeShader,
                             vk::AccessFlagBits2::eShaderRead | write_flag);
    }
    ++batch.batches;
    ++batch.flushes[std::min<u32>(reason, SkinBatch::NumFlush - 1)];
    if (SkinBatchStats()) {
        const auto now = std::chrono::steady_clock::now();
        if (now - batch.window >= std::chrono::seconds(5)) {
            const double secs = std::chrono::duration<double>(now - batch.window).count();
            std::printf("Skin batch: %.0f dispatches/s in %.0f batches/s (%.1f each), %.0f/s "
                        "regular; batches ended by packet %llu, idle %llu, hazard %llu, full "
                        "%llu, tick %llu, submission %llu\n",
                        batch.dispatches / secs, batch.batches / secs,
                        double(batch.dispatches) / std::max<u64>(batch.batches, 1),
                        batch.regular / secs,
                        static_cast<unsigned long long>(batch.flushes[SkinBatch::Packet]),
                        static_cast<unsigned long long>(batch.flushes[SkinBatch::Idle]),
                        static_cast<unsigned long long>(batch.flushes[SkinBatch::Hazard]),
                        static_cast<unsigned long long>(batch.flushes[SkinBatch::Full]),
                        static_cast<unsigned long long>(batch.flushes[SkinBatch::Tick]),
                        static_cast<unsigned long long>(batch.flushes[SkinBatch::Submit]));
            // The packets that ended batches: 1 special draw, 2 draw, small: a shader hash,
            // else a task function (offset in its library, for llvm-symbolizer).
            std::vector<std::pair<u64, u64>> top(batch.causes.begin(), batch.causes.end());
            std::sort(top.begin(), top.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });
            std::printf("  ended by:");
            for (size_t i = 0; i < top.size() && i < 6; ++i) {
                const u64 cause = top[i].first;
                if (cause > 0xFFFFFFFFull) {
                    Dl_info info{};
                    const u64 base = dladdr(reinterpret_cast<void*>(cause), &info) && info.dli_fbase
                                         ? reinterpret_cast<u64>(info.dli_fbase)
                                         : 0;
                    std::printf(" task+%#llx %llu", static_cast<unsigned long long>(cause - base),
                                static_cast<unsigned long long>(top[i].second));
                } else {
                    std::printf(" %#llx %llu", static_cast<unsigned long long>(cause),
                                static_cast<unsigned long long>(top[i].second));
                }
            }
            std::printf("\n");
            batch.causes.clear();
            batch.window = now;
            batch.dispatches = batch.batches = batch.regular = 0;
            batch.flushes = {};
        }
    }
    for (u32 k = 0; k < Kinds.size(); ++k) {
        batch.jobs[k].clear();
        batch.job_of_group[k].clear();
    }
    batch.bound.clear();
    batch.reads.Clear();
    batch.writes.Clear();
    batch.needs_barrier = false;
    draw_pipe->SetHeld(false);
    if (reason != SkinBatch::Submit) {
        scheduler.KickRecording();
    }
}

void Rasterizer::FlushSkinBatchForSubmit() {
    // A submission from stage B (a fault resolved there may read back GPU data) carries the
    // batch: it is recorded into the command buffer being submitted.
    if (DrawPipe::OnStageB()) {
        FlushSkinBatch(SkinBatch::Submit);
    }
}

} // namespace Vulkan
