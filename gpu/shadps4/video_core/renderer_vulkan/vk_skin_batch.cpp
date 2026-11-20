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
//
// g2-arm64 (issue #35): the game binds one skinning shader in several layouts, and the translated
// shader is compiled for each (the address of a vertex's element is the index times the V#'s
// stride, and LowerBufferFormatToRaw reads and writes it by the V#'s format). U1, U2 and most of
// U3 use one layout a shader (half4 normals and tangents; 28-, 36- and 20-byte output vertices);
// from U3's chapter 16 on the meshes with cloth or morph targets (Drake's desert outfit, the
// horses) come with float32 normals and tangents in and out: 40-, 48- and 24-byte output
// vertices, the inputs of some interleaved 40 bytes apart. The batch shaders were written from
// the first layout and stored those vertices at its strides and as halfs: Drake drawn as a burst
// of stretched triangles (found by a reporter of the issue: BB_SKIN_BATCH=0 cured it). Now every
// dispatch's V#s are read: the first layout goes to the shader written from it, the other
// layouts the any-layout variants know (the same source with SKIN_ANY_LAYOUT: strides and the
// normal's and tangent's formats from the job) to those, and a layout neither knows is
// dispatched the ordinary way and named in the log once. BB_SKIN_BATCH_LAYOUTS=0: only the first
// layouts are batched; =2: every job by the any-layout shaders (the A/B of the two on one work).
// On the G2 at U3's chapter 16 checkpoint (720p, the 30 limit): 25.2 FPS with the broken batch,
// 22.6 with BB_SKIN_BATCH=0.

#include <dlfcn.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <unordered_map>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "core/debug_state.h"
#include "core/memory.h"
#include "shader_recompiler/info.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/host_shaders/skin_batch_any_comp.h"
#include "video_core/host_shaders/skin_batch_comp.h"
#include "video_core/host_shaders/skin_mv_batch_any_comp.h"
#include "video_core/host_shaders/skin_mv_batch_comp.h"
#include "video_core/host_shaders/skin_u3_batch_any_comp.h"
#include "video_core/host_shaders/skin_u3_batch_comp.h"
#include "video_core/renderer_vulkan/vk_draw_pipe.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace VideoCore {
/// g2-arm64 (BB_SKIN_GUARD): the span of the device addresses handed out (buffer.cpp).
std::pair<u64, u64> BdaRange();
} // namespace VideoCore

namespace Vulkan {

extern bool g_gpu_checkpoints;

namespace {
constexpr u32 GuestGroupSize = 1024;
constexpr u32 GroupSize = 64; ///< the batch shaders' workgroup
constexpr u32 MaxBuffers = 12;
constexpr u32 MaxJobs = 512;
constexpr u32 MaxGroups = 65535;

/// g2-arm64 (issue #35): what a buffer is to a batched shader. The per-vertex ones are read and
/// written at the vertex's index times the V#'s stride, the normals and tangents as the V#'s
/// format says (half4, or float3 and float4); the tables are read at byte offsets the shader
/// computes.
enum class Role : u8 {
    Table,       ///< influences, matrices: all of it may be read
    Header,      ///< per vertex: the influence count and the offset of the first, two raw dwords
    Position,    ///< per vertex: float3
    Normal,      ///< per vertex: half4 (w unused) or float3
    Tangent,     ///< per vertex: half4 or float4 (w: the handedness)
    OutPosition, ///< float3
    OutNormal,   ///< half4 (w 0) or float3
    OutTangent,  ///< half4 or float4
    OutMotion,   ///< half4 (w 0)
};

/// The batched guest shaders. Their V#s are the inputs, then the outputs.
struct SkinKind {
    u32 hash;
    u32 num_buffers;
    u32 first_output;
    u32 flat_count; ///< flattened user data word with the vertex count
    u32 flat_scale; ///< ... with the motion scale (0: none)
    std::array<Role, MaxBuffers> roles;
    /// The first layout, the one the shader without SKIN_ANY_LAYOUT is written from: the
    /// per-vertex buffers' strides in bytes (halfs for the normals and tangents).
    std::array<u8, MaxBuffers> first_stride;
    const char* label;
};
constexpr std::array<SkinKind, 3> Kinds{{
    // U1: position, normal, tangent; 28-byte output vertices.
    {0x8f067540, 9, 6, 52, 0,
     {Role::Header, Role::Position, Role::Normal, Role::Tangent, Role::Table, Role::Table,
      Role::OutPosition, Role::OutNormal, Role::OutTangent},
     {8, 12, 8, 8, 0, 0, 28, 28, 28}, "skinning batch (cs 8f067540)"},
    // U2: position, normal, tangent, motion; 36-byte output vertices.
    {0x7d7e7206, 12, 8, 64, 65,
     {Role::Header, Role::Position, Role::Normal, Role::Tangent, Role::Table, Role::Table,
      Role::Table, Role::Table, Role::OutPosition, Role::OutNormal, Role::OutTangent,
      Role::OutMotion},
     {8, 12, 8, 8, 0, 0, 0, 0, 36, 36, 36, 36}, "skinning batch (cs 7d7e7206)"},
    // U3: position, normal (g2-arm64, 2026-11-09); 20-byte output vertices.
    {0x0c936dd6, 7, 5, 44, 0,
     {Role::Header, Role::Position, Role::Normal, Role::Table, Role::Table, Role::OutPosition,
      Role::OutNormal},
     {8, 12, 8, 0, 0, 20, 20}, "skinning batch (cs 0c936dd6)"},
}};

/// g2-arm64 (issue #35): the batch shader of a kind: the one written from the first layout, or
/// its any-layout variant.
enum Variant : u32 { FirstLayout, AnyLayout, NumVariants };

/// A per-vertex buffer as a dispatch binds it.
struct Element {
    u32 bytes;   ///< of a vertex's element
    bool floats; ///< 32-bit floats where the first layout has halfs
    bool absent; ///< an invalid format: the translated shader reads zero and stores nothing
    bool known;  ///< a batch shader reads or writes it as the translated shader does
};

Element ElementOf(Role role, const Shader::BufferResource& desc, const AmdGpu::Buffer& sharp) {
    using AmdGpu::CompSwizzle;
    using AmdGpu::DataFormat;
    const bool plain = !sharp.swizzle_enable && !sharp.add_tid_enable && sharp.GetStride() != 0 &&
                       sharp.GetStride() % 4 == 0;
    if (role == Role::Header) {
        return {8, false, false, plain && !desc.is_formatted};
    }
    if (!desc.is_formatted) {
        return {0, false, false, false};
    }
    const DataFormat format = sharp.GetDataFmt();
    if (format == DataFormat::FormatInvalid) {
        return {0, false, true, true};
    }
    const auto select = sharp.DstSelect();
    const bool xyz = plain && sharp.GetNumberFmt() == AmdGpu::NumberFormat::Float &&
                     sharp.GetNumberConversion() == AmdGpu::NumberConversion::None &&
                     select.r == CompSwizzle::Red && select.g == CompSwizzle::Green &&
                     select.b == CompSwizzle::Blue;
    const bool xyzw = xyz && select.a == CompSwizzle::Alpha;
    const bool half4 = format == DataFormat::Format16_16_16_16;
    switch (role) {
    case Role::Position:
    case Role::OutPosition:
        return {12, false, false, xyz && format == DataFormat::Format32_32_32};
    case Role::Normal:
        return half4 ? Element{8, false, false, xyz}
                     : Element{12, true, false, xyz && format == DataFormat::Format32_32_32};
    case Role::OutNormal:
        return half4 ? Element{8, false, false, xyzw}
                     : Element{12, true, false, xyz && format == DataFormat::Format32_32_32};
    case Role::Tangent:
    case Role::OutTangent:
        return half4 ? Element{8, false, false, xyzw}
                     : Element{16, true, false,
                               xyzw && format == DataFormat::Format32_32_32_32};
    case Role::OutMotion:
        return {8, false, false, xyzw && half4};
    default:
        return {0, false, false, false};
    }
}

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
    u32 stride[MaxBuffers]; ///< g2-arm64 (issue #35): the per-vertex buffers' strides in dwords
    u32 count;
    u32 first_group;
    u32 scale;  ///< the motion scale (float bits)
    u32 floats; ///< bit i: buffer i's normal or tangent is 32-bit floats, not half4
};
static_assert(sizeof(SkinJob) == 208);

struct SkinPush {
    u64 jobs;
    u64 groups;
    u32 num_groups;
    u32 num_jobs; ///< g2-arm64 (BB_SKIN_GUARD): the jobs in the table
    u64 low;      ///< the device addresses handed out: [low, high)
    u64 high;
    u64 report;   ///< the guard's report (SkinGuardReport), 0 without the guard
    u32 batch;    ///< the batch's number, for the report
    u32 check;    ///< g2-arm64 (BB_SKIN_GUARD): SkinPushChecksum of the fields above
};
static_assert(sizeof(SkinPush) == 56);

/// g2-arm64 (BB_SKIN_GUARD): FNV-1a over the 32-bit words of the fields before `check`, as the
/// shaders compute it (PushChecksum). memc-u1-1 (2026-11-19) had a dispatch whose num_jobs was
/// garbage: a dispatch whose constants arrive changed is caught here or in the shader.
u32 SkinPushChecksum(const SkinPush& p) {
    const u32 words[] = {
        static_cast<u32>(p.jobs),   static_cast<u32>(p.jobs >> 32),
        static_cast<u32>(p.groups), static_cast<u32>(p.groups >> 32),
        p.num_groups,               p.num_jobs,
        static_cast<u32>(p.low),    static_cast<u32>(p.low >> 32),
        static_cast<u32>(p.high),   static_cast<u32>(p.high >> 32),
        static_cast<u32>(p.report), static_cast<u32>(p.report >> 32),
        p.batch,
    };
    u32 h = 0x811C9DC5u;
    for (const u32 w : words) {
        h = (h ^ w) * 0x01000193u;
    }
    return h;
}

/// g2-arm64 (BB_SKIN_GUARD=1): the shaders' PushReport, 64 bytes into the guard's buffer: the
/// first mismatching workgroup's view of the constants, word by word in SkinPush's layout.
struct SkinPushReport {
    u32 count;
    u32 check_computed;
    u32 words[14];
};
static_assert(sizeof(SkinPushReport) == 64);
static_assert(sizeof(SkinPush) == sizeof(SkinPushReport::words));
constexpr u64 SkinPushReportOffset = 64;

/// g2-arm64 diagnosis BB_SKIN_PUSH_TEST=N (with BB_SKIN_GUARD=1): every Nth batch's dispatch gets
/// a wrong checksum, alternately in the recording chunk (the recording thread's check skips it)
/// and only in what is pushed (the shaders' check skips it), so both reports are seen working.
u32 SkinPushTestEvery() {
    static const u32 every = [] {
        const char* env = std::getenv("BB_SKIN_PUSH_TEST");
        return env ? static_cast<u32>(std::strtoul(env, nullptr, 10)) : 0u;
    }();
    return every;
}

/// g2-arm64 diagnosis BB_SKIN_JOB_TEST=N (with the guard): every Nth batch's first job reaches the
/// shaders with buffer 1 at an address of the storms (0x960697069700, memc-u1-2), in the stream's
/// copy only, so the job check and its report are seen working.
u32 SkinJobTestEvery() {
    static const u32 every = [] {
        const char* env = std::getenv("BB_SKIN_JOB_TEST");
        return env ? static_cast<u32>(std::strtoul(env, nullptr, 10)) : 0u;
    }();
    return every;
}

/// g2-arm64 (BB_SKIN_GUARD=1): a recorded skin dispatch whose constants changed in the recording
/// chunk before the recording thread ran it; it is skipped.
void NoteRecordedPushChanged(const SkinPush& p) {
    static std::atomic<u32> count{0};
    const u32 n = count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 10) {
        std::printf("Skin guard: a recorded skin dispatch's constants changed before the recording "
                    "thread ran it (%u so far): batch %u, %u groups, %u jobs, tables 0x%llx "
                    "0x%llx, checksum 0x%08x, now 0x%08x - skipped\n",
                    n, p.batch, p.num_groups, p.num_jobs, static_cast<unsigned long long>(p.jobs),
                    static_cast<unsigned long long>(p.groups), p.check, SkinPushChecksum(p));
        std::fflush(stdout);
    }
}

/// g2-arm64 (BB_SKIN_GUARD=1): the shaders' GuardReport.
struct SkinGuardReport {
    u32 count;
    u32 batch;
    u32 job;
    u32 slot;
    u64 address;
};
static_assert(sizeof(SkinGuardReport) == 24);

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

/// g2-arm64 BB_SKIN_GUARD, on by default since port 0205 (=0 off). The GPU page-fault storms
/// (2026-11-07 to 11-18: up to ~300 k reads at garbage addresses that look like half-float vertex
/// data, U1 and U2 only, where this batch is the default; one reset the G2) and memc-u1-1
/// (2026-11-19), where this guard caught a dispatch whose push constants were garbage. The
/// shaders check the constants' checksum first (port 0204), then each job's index, skip what
/// fails and note the first; the log names the batch and says whether its tables' bytes in the
/// stream buffer are still the ones written. A skipped dispatch costs one frame of those
/// vertices; a garbage one cost a storm. BB_SKIN_GUARD=2 also checks each job's buffer addresses
/// against the device addresses handed out (port 0207): about 0.4 FPS in U1 (2026-11-20), and it never
/// fired outside its test, while the index check caught memc-u1-1's garbage.
u32 SkinGuardLevel() {
    static const u32 level = [] {
        const char* env = std::getenv("BB_SKIN_GUARD");
        if (env && env[0] == '0') {
            return 0u;
        }
        return env && env[0] == '2' ? 2u : 1u;
    }();
    return level;
}

bool SkinGuardWanted() {
    return SkinGuardLevel() != 0;
}

/// A quick hash of a table's bytes (the guard's comparison).
u64 TableHash(const void* data, size_t bytes) {
    u64 h = 0xcbf29ce484222325ull;
    const auto* p = static_cast<const u8*>(data);
    size_t i = 0;
    for (; i + 8 <= bytes; i += 8) {
        u64 w;
        std::memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x100000001b3ull;
    }
    for (; i < bytes; ++i) {
        h = (h ^ p[i]) * 0x100000001b3ull;
    }
    return h;
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

/// g2-arm64 (issue #35): BB_SKIN_BATCH_LAYOUTS: 0 - only the first layouts are batched (any
/// other is dispatched the ordinary way); 1 (the default) - the other known layouts by the
/// any-layout shaders; 2 - every job by the any-layout shaders.
u32 SkinLayoutsMode() {
    static const u32 mode = [] {
        const char* env = std::getenv("BB_SKIN_BATCH_LAYOUTS");
        return env && env[0] >= '0' && env[0] <= '2' ? static_cast<u32>(env[0] - '0') : 1u;
    }();
    return mode;
}

std::span<const u32> SkinCode(u32 kind, u32 variant) {
    switch (kind * NumVariants + variant) {
    case 0:
        return SKIN_BATCH_COMP;
    case 1:
        return SKIN_BATCH_ANY_COMP;
    case 2:
        return SKIN_MV_BATCH_COMP;
    case 3:
        return SKIN_MV_BATCH_ANY_COMP;
    case 4:
        return SKIN_U3_BATCH_COMP;
    default:
        return SKIN_U3_BATCH_ANY_COMP;
    }
}

struct SkinPipeline {
    vk::UniquePipelineLayout layout;
    vk::UniquePipeline pipeline;
};

SkinPipeline CreateSkinPipeline(const Instance& instance, std::span<const u32> code,
                                u64 report) {
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
    // g2-arm64 (BB_SKIN_GUARD): the report buffer's address as constants 0 and 1, so a dispatch
    // with garbage push constants still reports to the right place (0: no guard); constant 2: the
    // job's buffer addresses checked too (level 2).
    const std::array<u32, 3> spec_data{static_cast<u32>(report), static_cast<u32>(report >> 32),
                                       report && SkinGuardLevel() >= 2 ? 1u : 0u};
    const std::array<vk::SpecializationMapEntry, 3> spec_entries{{
        {.constantID = 0, .offset = 0, .size = sizeof(u32)},
        {.constantID = 1, .offset = sizeof(u32), .size = sizeof(u32)},
        {.constantID = 2, .offset = 2 * sizeof(u32), .size = sizeof(u32)},
    }};
    const vk::SpecializationInfo spec_info{
        .mapEntryCount = static_cast<u32>(spec_entries.size()),
        .pMapEntries = spec_entries.data(),
        .dataSize = sizeof(spec_data),
        .pData = spec_data.data(),
    };
    p.pipeline = Check(device.createComputePipelineUnique(
        {}, vk::ComputePipelineCreateInfo{
                .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                          .module = module,
                          .pName = "main",
                          .pSpecializationInfo = &spec_info},
                .layout = *p.layout,
            }));
    device.destroyShaderModule(module);
    return p;
}
} // namespace

struct Rasterizer::SkinBatch {
    /// By kind and variant (g2-arm64, issue #35: the first layout's shader, the any-layout one).
    std::array<std::vector<SkinJob>, Kinds.size() * NumVariants> jobs;
    std::array<std::vector<u32>, Kinds.size() * NumVariants> job_of_group;
    std::vector<BoundBuffer> bound;
    RangeSet reads;
    RangeSet writes;
    bool needs_barrier = false;
    u64 tick = 0;

    /// Statistics (BB_SKIN_BATCH_STATS=1): dispatches batched and why batches ended.
    enum Flush : u32 { Packet, Idle, Hazard, Full, Tick, Submit, NumFlush };
    u64 dispatches = 0, batches = 0, regular = 0;
    u64 any_layout = 0; ///< of the dispatches: by the any-layout shaders
    /// g2-arm64 (issue #35): the layouts met (a hash of the V#s' strides and formats), each
    /// named in the log once.
    std::vector<u64> layouts;
    std::array<u64, NumFlush> flushes{};
    std::unordered_map<u64, u64> causes; ///< packets that ended batches
    std::chrono::steady_clock::time_point window{};

    /// g2-arm64 (BB_SKIN_GUARD=1): the report the shaders write and the recent batches' tables.
    std::unique_ptr<VideoCore::Buffer> guard;
    u32 guard_seen = 0;  ///< skipped workgroups already logged
    u32 guard_lines = 0;
    u32 push_seen = 0;   ///< workgroups with changed constants already logged
    u32 push_lines = 0;
    u32 number = 0;      ///< batches recorded (a kind and variant each)
    struct Recent {
        u32 number;
        u64 tick;
        u64 jobs_offset, groups_offset;
        u32 jobs, groups;
        u64 jobs_hash, groups_hash;
        SkinPush push; ///< g2-arm64 (BB_SKIN_GUARD): the constants as pushed
    };
    std::array<Recent, 512> recent{};
};

void Rasterizer::InitSkinBatch() {
    if (!draw_pipe || !SkinBatchWanted()) {
        return;
    }
    skin_batch = std::make_shared<SkinBatch>();
    skin_batch->window = std::chrono::steady_clock::now();
    if (SkinGuardWanted()) {
        skin_batch->guard = std::make_unique<VideoCore::Buffer>(
            instance, 0, 256, VideoCore::MemoryType::HostCached, "bbport skin guard");
        std::memset(skin_batch->guard->mapped_data.data(), 0, 256);
        std::printf("Skin guard: on - the constants' checksum and each job's index%s checked on "
                    "the GPU\n",
                    SkinGuardLevel() >= 2 ? " and buffer addresses" : "");
        if (const u32 every = SkinPushTestEvery()) {
            std::printf("Skin guard: a wrong checksum every %u batches, alternately for the "
                        "recording thread and for the shaders (BB_SKIN_PUSH_TEST)\n",
                        every);
        }
        if (const u32 every = SkinJobTestEvery(); every && SkinGuardLevel() >= 2) {
            std::printf("Skin guard: every %u batches the first job's buffer 1 goes to the GPU at "
                        "0x960697069700 (BB_SKIN_JOB_TEST)\n",
                        every);
        } else if (every) {
            std::printf("Skin guard: BB_SKIN_JOB_TEST needs BB_SKIN_GUARD=2, off\n");
        }
    }
    draw_pipe->SetIdleHandler(&FlushSkinBatchIdle);
    std::printf("Skin batch: dispatches of cs %08x%s%s run in batches%s\n", Kinds[0].hash,
                MvBatchWanted() ? ", cs 7d7e7206" : "", U3BatchWanted() ? ", cs 0c936dd6" : "",
                SkinLayoutsMode() == 0   ? " (BB_SKIN_BATCH_LAYOUTS=0: their first layouts only)"
                : SkinLayoutsMode() == 2 ? " (BB_SKIN_BATCH_LAYOUTS=2: all by the any-layout shaders)"
                                         : "");
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
    // g2-arm64 (issue #35): the layout of this dispatch - its per-vertex V#s' strides and
    // formats - and the batch shader that knows it, if one does.
    std::array<AmdGpu::Buffer, MaxBuffers> sharps{};
    std::array<Element, MaxBuffers> elements{};
    u32 floats = 0, absent = 0;
    bool known = true, first_layout = true;
    for (u32 i = 0; fits && i < num_buffers; ++i) {
        sharps[i] = cs.buffers[i].GetSharp(cs);
        if (kind.roles[i] == Role::Table) {
            continue;
        }
        const Element element = ElementOf(kind.roles[i], cs.buffers[i], sharps[i]);
        elements[i] = element;
        known &= element.known;
        floats |= u32(element.floats) << i;
        absent |= u32(element.absent) << i;
        first_layout &= element.absent ||
                        (!element.floats && sharps[i].GetStride() == kind.first_stride[i]);
    }
    const u32 variant = first_layout && SkinLayoutsMode() != 2 ? FirstLayout : AnyLayout;
    const bool batched = fits && known && (variant == FirstLayout || SkinLayoutsMode() != 0);
    // Each layout is named in the log once: one the batch does not take always (a report's log
    // shows it), the batched ones with the statistics.
    u64 layout = 0xcbf29ce484222325ull ^ k;
    const bool note = fits && (!batched || SkinBatchStats()) && batch.layouts.size() < 32;
    for (u32 i = 0; note && i < num_buffers; ++i) {
        if (kind.roles[i] == Role::Table) {
            continue;
        }
        const auto& sharp = sharps[i];
        const auto select = sharp.DstSelect();
        for (const u64 value :
             {u64(sharp.GetStride()), u64(sharp.data_format), u64(sharp.num_format),
              u64(select.r) | u64(select.g) << 8 | u64(select.b) << 16 | u64(select.a) << 24,
              u64(sharp.swizzle_enable) | u64(sharp.add_tid_enable) << 1 |
                  u64(cs.buffers[i].is_formatted) << 2}) {
            layout = (layout ^ value) * 0x100000001b3ull;
        }
    }
    if (note &&
        std::find(batch.layouts.begin(), batch.layouts.end(), layout) == batch.layouts.end()) {
        batch.layouts.push_back(layout);
        std::printf("Skin batch: cs %08x layout", kind.hash);
        for (u32 i = 0; i < num_buffers; ++i) {
            if (kind.roles[i] == Role::Table) {
                continue;
            }
            const auto& sharp = sharps[i];
            const auto select = sharp.DstSelect();
            std::printf(" %u:%u/%u.%u%s%s%s", i, u32(sharp.GetStride()), u32(sharp.data_format),
                        u32(sharp.num_format), cs.buffers[i].is_formatted ? "" : "r",
                        select.r == AmdGpu::CompSwizzle::Red &&
                                select.g == AmdGpu::CompSwizzle::Green &&
                                select.b == AmdGpu::CompSwizzle::Blue
                            ? (select.a == AmdGpu::CompSwizzle::Alpha ? "" : "a")
                            : "s",
                        sharp.swizzle_enable || sharp.add_tid_enable ? "t" : "");
        }
        std::printf(" (buffer:stride/data format.number format; r raw, a w not from the alpha, "
                    "s swizzled, t swizzle or thread id addressing): %s\n",
                    !known                 ? "a layout the batch does not know, dispatched one by one"
                    : !batched             ? "dispatched one by one (BB_SKIN_BATCH_LAYOUTS=0)"
                    : variant == AnyLayout ? "batched by the any-layout shader"
                                           : "batched, the first layout");
    }
    if (!batched) {
        FlushSkinBatch(SkinBatch::Packet);
        ++batch.regular;
        return false;
    }
    const u32 list = k * NumVariants + variant;
    const u32 count = static_cast<u32>(
        std::min<u64>(flat[kind.flat_count], u64(program.dim_x) * GuestGroupSize));

    // The guest memory the dispatch reads and writes (its buffers' V#s; the per-vertex inputs and
    // the outputs exactly: a vertex's element at its index times the V#'s stride).
    auto* memory = Core::Memory::Instance();
    std::array<VAddr, MaxBuffers> base{};
    std::array<u64, MaxBuffers> bytes{};
    // Unused entries stay empty ranges: they neither overlap nor add.
    std::array<std::pair<VAddr, VAddr>, MaxBuffers> reads{};
    std::array<std::pair<VAddr, VAddr>, MaxBuffers> writes{};
    for (u32 i = 0; i < num_buffers; ++i) {
        const auto& sharp = sharps[i];
        base[i] = sharp.base_address;
        bytes[i] = base[i] && sharp.GetSize() ? memory->ClampRangeSize(base[i], sharp.GetSize())
                                              : 0;
        u64 size = bytes[i];
        if (kind.roles[i] != Role::Table) {
            const u64 last = count ? u64(count - 1) * sharp.GetStride() + elements[i].bytes : 0;
            size = elements[i].absent ? 0 : std::min<u64>(size, last);
        }
        (i < kind.first_output ? reads : writes)[i] = {base[i], base[i] + size};
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
    } else if (batch.jobs[list].size() >= MaxJobs ||
               batch.job_of_group[list].size() + groups > MaxGroups) {
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
    const u32 index = static_cast<u32>(batch.jobs[list].size());
    for (u32 i = 0; i < num_buffers; ++i) {
        if (kind.roles[i] == Role::Table) {
            continue;
        }
        if (absent >> i & 1) {
            // g2-arm64 (issue #35): as the translated shader with an invalid format: nothing
            // read (zeros), nothing stored.
            job.address[i] = 0;
            job.size[i] = 0;
        }
        job.stride[i] = sharps[i].GetStride() / 4;
    }
    job.floats = floats;
    job.count = count;
    job.first_group = static_cast<u32>(batch.job_of_group[list].size());
    job.scale = kind.flat_scale ? static_cast<u32>(flat[kind.flat_scale]) : 0;
    batch.job_of_group[list].insert(batch.job_of_group[list].end(), groups, index);
    batch.jobs[list].push_back(job);
    if (SkinBatchStats()) {
        // The first jobs of each kind and variant: are the outputs one interleaved buffer (the
        // batch shaders' row stores)?
        static std::array<u32, Kinds.size() * NumVariants> logged{};
        if (logged[list] < 4) {
            ++logged[list];
            const u32 f = kind.first_output;
            std::printf("Skin batch: cs %08x job%s, %u vertices, outputs", kind.hash,
                        variant == AnyLayout ? " (any layout)" : "", job.count);
            for (u32 i = f; i < num_buffers; ++i) {
                std::printf(" +%lld/%u", static_cast<long long>(job.address[i] - job.address[f]),
                            job.size[i]);
            }
            std::printf("\n");
        }
    }
    batch.any_layout += variant == AnyLayout;
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
    // g2-arm64 (BB_SKIN_GUARD=1): a job the shaders skipped since the last look.
    if (batch.guard) {
        batch.guard->Invalidate(0, SkinPushReportOffset + sizeof(SkinPushReport));
        SkinGuardReport report;
        std::memcpy(&report, batch.guard->mapped_data.data(), sizeof(report));
        if (report.count != batch.guard_seen && batch.guard_lines < 40) {
            ++batch.guard_lines;
            const auto& r = batch.recent[report.batch % batch.recent.size()];
            auto& stream = buffer_cache.GetStreamBuffer();
            const char* jobs_now = "?";
            const char* groups_now = "?";
            u64 now[4]{};
            const bool known = r.number == report.batch && !stream.mapped_data.empty();
            if (known) {
                const u8* base = stream.mapped_data.data();
                jobs_now = TableHash(base + r.jobs_offset, r.jobs * sizeof(SkinJob)) == r.jobs_hash
                               ? "as written"
                               : "CHANGED";
                groups_now =
                    TableHash(base + r.groups_offset, r.groups * sizeof(u32)) == r.groups_hash
                        ? "as written"
                        : "CHANGED";
                if (report.job < r.jobs) {
                    std::memcpy(now, base + r.jobs_offset + u64(report.job) * sizeof(SkinJob),
                                sizeof(now));
                }
            }
            std::printf("Skin guard: %u workgroups skipped their job since the last line (the "
                        "first: batch %u, job %u, %s %u at 0x%llx); the batch %s: tick %llu "
                        "(%s), %u jobs at stream +0x%llx %s, %u groups at +0x%llx %s; that job's "
                        "first addresses in the stream now 0x%llx 0x%llx 0x%llx 0x%llx\n",
                        report.count - batch.guard_seen, report.batch, report.job,
                        report.slot < 12 ? "buffer" : "index", report.slot,
                        static_cast<unsigned long long>(report.address),
                        known ? "recorded" : "no longer kept",
                        static_cast<unsigned long long>(r.tick),
                        known && scheduler.IsFree(r.tick) ? "done" : "pending", r.jobs,
                        static_cast<unsigned long long>(r.jobs_offset), jobs_now, r.groups,
                        static_cast<unsigned long long>(r.groups_offset), groups_now,
                        static_cast<unsigned long long>(now[0]),
                        static_cast<unsigned long long>(now[1]),
                        static_cast<unsigned long long>(now[2]),
                        static_cast<unsigned long long>(now[3]));
            std::fflush(stdout);
        }
        batch.guard_seen = report.count;
        // g2-arm64 (BB_SKIN_GUARD=1): dispatches whose constants failed their checksum.
        SkinPushReport push_report;
        std::memcpy(&push_report, batch.guard->mapped_data.data() + SkinPushReportOffset,
                    sizeof(push_report));
        if (push_report.count != batch.push_seen && batch.push_lines < 40) {
            ++batch.push_lines;
            // The words the first one saw and, when its word 12 (the batch's number) still names a
            // batch kept here, the words that batch pushed and a mask of those that differ.
            const u32 seen_batch = push_report.words[12];
            const auto& r = batch.recent[seen_batch % batch.recent.size()];
            const bool known = r.number == seen_batch && r.push.batch == seen_batch;
            u32 pushed[14]{};
            std::memcpy(pushed, &r.push, sizeof(pushed));
            char seen[14 * 9 + 1]{};
            char was[14 * 9 + 1]{};
            u32 differ = 0;
            for (u32 i = 0; i < 14; ++i) {
                std::snprintf(seen + i * 9, 10, "%08x ", push_report.words[i]);
                std::snprintf(was + i * 9, 10, "%08x ", pushed[i]);
                if (known && push_report.words[i] != pushed[i]) {
                    differ |= 1u << i;
                }
            }
            seen[14 * 9 - 1] = '\0';
            was[14 * 9 - 1] = '\0';
            std::printf("Skin guard: %u workgroups found their dispatch's constants changed since "
                        "the last line (the first saw: %s; checksum computed 0x%08x); batch %u %s: "
                        "%s; the words that differ: 0x%04x\n",
                        push_report.count - batch.push_seen, seen, push_report.check_computed,
                        seen_batch, known ? "pushed" : "is not kept (no comparison), slot",
                        was, differ);
            std::fflush(stdout);
        }
        batch.push_seen = push_report.count;
    }
    if (reason == SkinBatch::Packet && SkinBatchStats()) {
        ++batch.causes[cause];
    }
    // g2-arm64 (issue #35): made at a variant's first batch (the any-layout ones only where the
    // game brings such a layout).
    static std::array<SkinPipeline, Kinds.size() * NumVariants> pipes;
    if (batch.needs_barrier) {
        runtime.FlushBarriers();
    }
    scheduler.EndRendering();
    // The kinds and variants write different vertices (an overlap ends a batch): one dispatch
    // each.
    for (u32 k = 0; k < Kinds.size() * NumVariants; ++k) {
        const u32 num_groups = static_cast<u32>(batch.job_of_group[k].size());
        if (num_groups == 0) {
            continue;
        }
        if (!pipes[k].pipeline) {
            pipes[k] = CreateSkinPipeline(instance, SkinCode(k / NumVariants, k % NumVariants),
                                          batch.guard ? batch.guard->BufferDeviceAddress() : 0);
        }
        if (auto* profiler = GpuProfiler::Get()) {
            profiler->Mark(0x5C1Bull + k, [k] {
                return std::string{Kinds[k / NumVariants].label} +
                       (k % NumVariants == AnyLayout ? ", any layout" : "");
            });
        }
        auto& stream = buffer_cache.GetStreamBuffer();
        // g2-arm64 diagnosis BB_SKIN_JOB_TEST=N: the stream's copy of the first job gets a bad
        // buffer 1 (the table hashed for the guard's log is the one before, so it says CHANGED).
        const u32 job_test = batch.guard && SkinGuardLevel() >= 2 ? SkinJobTestEvery() : 0u;
        const bool test_job = job_test && (batch.number + 1) % job_test == 0;
        u64 tested_address = 0;
        if (test_job) {
            tested_address = batch.jobs[k][0].address[1];
            batch.jobs[k][0].address[1] = 0x960697069700ull;
        }
        const u64 jobs_offset =
            stream.Copy(batch.jobs[k].data(), batch.jobs[k].size() * sizeof(SkinJob), 16);
        if (test_job) {
            batch.jobs[k][0].address[1] = tested_address;
        }
        const u64 groups_offset =
            stream.Copy(batch.job_of_group[k].data(), num_groups * sizeof(u32), 16);
        const u64 address = stream.BufferDeviceAddress();
        const u32 num_jobs = static_cast<u32>(batch.jobs[k].size());
        SkinPush push{address + jobs_offset, address + groups_offset, num_groups, num_jobs};
        if (batch.guard) {
            const auto [low, high] = VideoCore::BdaRange();
            push.low = low;
            push.high = high;
            push.report = batch.guard->BufferDeviceAddress();
            push.batch = ++batch.number;
            batch.recent[push.batch % batch.recent.size()] = {
                push.batch,
                scheduler.CurrentTick(),
                jobs_offset,
                groups_offset,
                num_jobs,
                num_groups,
                TableHash(batch.jobs[k].data(), num_jobs * sizeof(SkinJob)),
                TableHash(batch.job_of_group[k].data(), num_groups * sizeof(u32))};
        }
        push.check = SkinPushChecksum(push); // g2-arm64 (BB_SKIN_GUARD)
        u32 gpu_flip = 0;
        if (batch.guard) {
            batch.recent[push.batch % batch.recent.size()].push = push;
            // g2-arm64 diagnosis BB_SKIN_PUSH_TEST=N: a wrong checksum now and then, alternately
            // for the recording thread's check and for the shaders'.
            if (const u32 every = SkinPushTestEvery(); every && push.batch % every == 0) {
                if ((push.batch / every) % 2 == 1) {
                    push.check ^= 0x00F0F0F0u;
                } else {
                    gpu_flip = 0x0F0F0F0Fu;
                }
            }
        }
        scheduler.Record([pipeline = *pipes[k].pipeline, layout = *pipes[k].layout, push,
                          gpu_flip](vk::CommandBuffer cmdbuf) {
            if (push.report && SkinPushChecksum(push) != push.check) {
                NoteRecordedPushChanged(push); // g2-arm64 (BB_SKIN_GUARD): the chunk's copy
                return;
            }
            SkinPush pushed = push;
            pushed.check ^= gpu_flip; // BB_SKIN_PUSH_TEST only
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
            cmdbuf.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(pushed),
                                 &pushed);
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
            std::printf("Skin batch: %.0f dispatches/s (%.0f/s by the any-layout shaders) in %.0f "
                        "batches/s (%.1f each), %.0f/s regular; batches ended by packet %llu, idle "
                        "%llu, hazard %llu, full %llu, tick %llu, submission %llu\n",
                        batch.dispatches / secs, batch.any_layout / secs, batch.batches / secs,
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
            batch.dispatches = batch.batches = batch.regular = batch.any_layout = 0;
            batch.flushes = {};
        }
    }
    for (u32 k = 0; k < Kinds.size() * NumVariants; ++k) {
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
