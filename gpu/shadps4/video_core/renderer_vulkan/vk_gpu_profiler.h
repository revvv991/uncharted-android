// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: BB_GPU_PROFILE=1 — GPU time per render pass, dispatch and upscaler run. A timestamp is
// written where each of them starts (and at the frame end); the time to the next timestamp is
// charged to its label, barriers and copies recorded in between included. Results are read four
// frames later and printed every 5 s: GPU ms per frame by label.
// g2-arm64: BB_GPU_PROFILE=2 also splits a pass wherever its draws' pixel shader or their depth
// and blend state change: each run of such draws is a label with its own time, draws and
// indices. True where the driver runs a pass's draws in order (Turnip: TU_DEBUG=sysmem); a
// tiler's pass split that way pays a load and a store for every piece. BB_GPU_PROFILE=3 adds
// each label's fragment shader invocations (a pipeline statistics query from one timestamp to
// the next).

#pragma once

#include <array>
#include <mutex>
#include <chrono>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class Instance;
class Scheduler;

class GpuProfiler {
    std::recursive_mutex mutex;

public:
    /// g2-arm64: the label of the GPU's wait between two submissions (Scheduler::SubmitExecution).
    static constexpr u64 IdleKey = 0x5B317ull;

    /// The profiler when BB_GPU_PROFILE=1, else null.
    static GpuProfiler* Get() noexcept {
        return instance_ptr;
    }
    static void Init(const Instance& instance, Scheduler& scheduler);

    /// Starts a segment labelled `key`; `describe` names it the first time the key is seen.
    template <typename Describe>
    void Mark(u64 key, Describe&& describe) {
        std::scoped_lock lock{mutex};
        if (!described.contains(key)) {
            described.emplace(key, describe());
        }
        WriteTimestamp(key);
    }

    /// g2-arm64: BB_GPU_PROFILE=2 or 3 (the top of the file).
    [[nodiscard]] bool ByShader() const noexcept {
        return by_shader;
    }
    /// g2-arm64: a draw of the open segment: its label's draws and indices a frame.
    void CountDraw(u64 indices) {
        std::scoped_lock lock{mutex};
        auto& count = counts[current];
        ++count.draws;
        count.indices += indices;
    }

    /// The label of the open segment, for Resume after a nested segment (transfers).
    [[nodiscard]] u64 Current() const noexcept {
        return current;
    }
    /// Continues the segment of `key` (a label seen before) where a nested one ends.
    void Resume(u64 key) {
        WriteTimestamp(key);
    }

    /// Whether `other` is the scheduler it records into (the presenter has its own).
    [[nodiscard]] bool Records(const Scheduler* other) const noexcept {
        return other == &scheduler;
    }

    /// At the display pass: closes the frame's last segment, reads an old frame's results.
    void BeginFrame();

private:
    GpuProfiler(const Instance& instance, Scheduler& scheduler);
    void WriteTimestamp(u64 key);
    /// g2-arm64: on the thread recording the command buffer, in stream order.
    void Stamp(vk::CommandBuffer cmdbuf, u64 key);
    void NextSlice(vk::CommandBuffer cmdbuf, u64 label);
    void CloseStatistics(vk::CommandBuffer cmdbuf);
    void Collect(u32 slice);
    void Print();

    static constexpr u32 NumSlices = 8; // g2-arm64: two profiler frames a guest frame (U1)
    static constexpr u32 SliceQueries = 4096;
    static inline GpuProfiler* instance_ptr = nullptr;

    vk::Device device;
    Scheduler& scheduler;
    vk::UniqueQueryPool pool;
    /// g2-arm64: BB_GPU_PROFILE=3: the fragment shader invocations of a segment, in the slot
    /// of the timestamp that starts it.
    vk::UniqueQueryPool statistics_pool;
    bool statistics_open = false;
    u32 statistics_query = 0;
    bool by_shader = false;
    double period_ns = 1.0;
    u32 slice = 0;
    std::array<std::vector<u64>, NumSlices> keys; ///< label of each timestamp but the last
    std::array<u32, NumSlices> used{};
    std::array<bool, NumSlices> pending{};
    std::unordered_map<u64, std::string> described;
    struct Total {
        double ms = 0;
        u64 segments = 0;
        u64 fragments = 0;
    };
    std::unordered_map<u64, Total> totals;
    struct Count {
        u64 draws = 0;
        u64 indices = 0;
    };
    std::unordered_map<u64, Count> counts;
    /// g2-arm64: idle gaps between submissions by length (ms bounds below), and the longest
    /// gaps of the last collected frame with the labels around them (where the GPU waits).
    static constexpr std::array<double, 6> IdleBounds{0.05, 0.2, 0.5, 1.0, 2.0, 4.0};
    std::array<Total, IdleBounds.size() + 1> idle_lengths{};
    struct Gap {
        double ms;
        u64 before, after;
        u32 index; ///< the gap's place among the frame's gaps
    };
    std::vector<Gap> last_gaps;
    u32 last_gap_count = 0;
    u64 frames = 0;
    u64 current = 0;
    u64 skipped = 0, out_of_order = 0; // g2-arm64
    std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point window_start = std::chrono::steady_clock::now();
};

} // namespace Vulkan
