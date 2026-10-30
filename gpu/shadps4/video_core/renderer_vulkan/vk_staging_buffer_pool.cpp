// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "bbport_toggles.h"
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/types.h"
#include "core/devtools/widget/gpu_memory.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/renderer_vulkan/vk_vram.h"

namespace Vulkan {

using VideoCore::MemoryType;

namespace {

// bbport: staging memory is populated (allocated and cleared) by the kernel on first CPU access,
// ~2 ms per 16 MiB block. Streaming bursts a few seconds apart re-created the blocks trimmed
// in between and copies into them ran at 0.3 GB/s (stutter). Blocks now live 30 s idle, the
// upload ring keeps BB_STAGING_KEEP_MB (512) and is populated at startup.
#ifdef __ANDROID__
// g2-arm64: a phone shares its RAM with the GPU. In U2's palace the pool held 722 MiB (rings 370,
// large buffers 352) while the system had ~200 MB left. Rings take the first block with room (they
// settle on as few blocks as the load needs and the rest go idle), keep 64 MiB unless
// BB_STAGING_KEEP_MB says otherwise, and idle memory goes after ~20 s.
constexpr u64 RING_IDLE_FRAMES = 600;
constexpr u64 LARGE_IDLE_FRAMES = 600;
constexpr u64 DEFAULT_KEEP_MB = 64;
constexpr bool FIRST_FIT_RING = true;
#else
constexpr u64 RING_IDLE_FRAMES = 3000;
constexpr u64 LARGE_IDLE_FRAMES = 3000;
constexpr u64 DEFAULT_KEEP_MB = 512;
constexpr bool FIRST_FIT_RING = false;
#endif
constexpr u64 PREWARM_BLOCKS = 8;
constexpr u64 LARGE_MIN_GRANULARITY = 64_KB;
constexpr u64 BLOCK_SIZE = 16_MB;

u64 RoundAllocationSize(u64 size) {
    const u64 step = std::max(std::bit_floor(size) / 4, LARGE_MIN_GRANULARITY);
    return Common::AlignUp(size, step);
}

// g2-arm64 diagnosis: BB_STAGING_LOG=1 prints the pool's large buffers coming and going.
bool StagingLog() {
    static const bool on = [] {
        const char* env = std::getenv("BB_STAGING_LOG");
        return env && env[0] == '1';
    }();
    return on;
}

const char* MemoryTypeName(MemoryType type) {
    switch (type) {
    case MemoryType::DeviceLocal:
        return "device-local";
    case MemoryType::HostUncached:
        return "host-uncached";
    case MemoryType::HostCached:
        return "host-cached";
    default:
        return "other";
    }
}

} // Anonymous namespace

StagingBufferPool::StagingBufferPool(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {
    const char* env = std::getenv("BB_STAGING_KEEP_MB");
    keep_blocks = (env ? std::strtoull(env, nullptr, 10) : DEFAULT_KEEP_MB) * 1_MB / BLOCK_SIZE;
    Ring& ring = rings[u32(MemoryType::HostUncached)];
    for (u64 i = 0; i < PREWARM_BLOCKS; ++i) {
        auto& block = ring.blocks.emplace_back(Block{
            .buffer = std::make_unique<VideoCore::StreamBuffer>(
                instance, scheduler, MemoryType::HostUncached, BLOCK_SIZE),
            .last_used_frame = frame,
        });
        if (!block.buffer->mapped_data.empty()) {
            std::memset(block.buffer->mapped_data.data(), 0, block.buffer->mapped_data.size());
        }
    }
}

StagingBufferPool::~StagingBufferPool() = default;

StagingBufferRef StagingBufferPool::Request(u64 size, MemoryType type, u64 alignment,
                                            bool deferred) {
    Ring& ring = rings[u32(type)];
    if (deferred || size > BLOCK_SIZE) {
        return RequestLarge(size, type, deferred);
    }
    return RequestFromRing(ring, size, alignment, type);
}

StagingBufferRef StagingBufferPool::RequestFromRing(Ring& ring, u64 size, u64 alignment,
                                                    MemoryType type) {
    Block* best{};
    u64 best_offset{};
    const size_t start = FIRST_FIT_RING ? 0 : ring.current;
    for (size_t i = 0; i < ring.blocks.size(); ++i) {
        const size_t index = (start + i) % ring.blocks.size();
        Block* block = &ring.blocks[index];
        if (const auto offset = block->buffer->Reserve(size, alignment, false)) {
            ring.current = index;
            best = block;
            best_offset = *offset;
            break;
        }
    }

    if (!best) {
        best = &ring.blocks.emplace_back(Block{
            .buffer =
                std::make_unique<VideoCore::StreamBuffer>(instance, scheduler, type, BLOCK_SIZE),
            .last_used_frame = frame,
        });
        ring.current = ring.blocks.size() - 1;
        best_offset = *best->buffer->Reserve(size, alignment, false);
    }

    auto& buffer = *best->buffer;
    best->last_used_frame = frame;

    StagingBufferRef ref{
        .buffer = &buffer,
        .offset = best_offset,
        .size = size,
        .type = type,
    };
    if (!buffer.mapped_data.empty()) {
        ref.mapped = buffer.mapped_data.data() + best_offset;
    }
    return ref;
}

StagingBufferRef StagingBufferPool::RequestLarge(u64 size, MemoryType type, bool deferred) {
    auto& cache = large_caches[u32(type)];
    const u64 max_size = std::max(size + size / 4, RoundAllocationSize(size));

    LargeBuffer* best{};
    for (LargeBuffer& entry : cache) {
        const u64 entry_size = entry.buffer->SizeBytes();
        if (entry.held || entry_size < size || entry_size > max_size ||
            !scheduler.IsFree(entry.tick)) {
            continue;
        }
        if (!best || entry_size < best->buffer->SizeBytes()) {
            best = &entry;
        }
    }

    if (!best) {
        best = &cache.emplace_back(LargeBuffer{
            .buffer = std::make_unique<VideoCore::Buffer>(instance, 0, RoundAllocationSize(size),
                                                          type, "StagingBufferPool:Dedicated"),
            .tick = 0,
            .last_used_frame = frame,
            .held = false,
        });
        // g2-arm64 diagnosis (BB_STAGING_LOG=1): every large buffer the pool creates, with the
        // request behind it and the frame: the VMA swings in the Memory line (U3: 128 MiB coming
        // and going every ~30 s) start here.
        if (StagingLog()) {
            std::printf("Staging: new %s buffer of %llu KiB for a request of %llu KiB%s, frame %llu, "
                        "%zu such buffers\n", MemoryTypeName(type),
                        (unsigned long long)(best->buffer->SizeBytes() >> 10),
                        (unsigned long long)(size >> 10), deferred ? " (deferred)" : "",
                        (unsigned long long)frame, cache.size());
        }
    }

    best->tick = scheduler.CurrentTick();
    best->last_used_frame = frame;
    best->held = deferred;

    VideoCore::Buffer& buffer = *best->buffer;
    StagingBufferRef ref{
        .buffer = &buffer,
        .offset = 0,
        .size = size,
        .type = type,
    };
    if (!buffer.mapped_data.empty()) {
        ref.mapped = buffer.mapped_data.data();
    }
    return ref;
}

void StagingBufferPool::FreeDeferred(const StagingBufferRef& ref) {
    auto& cache = large_caches[u32(ref.type)];
    const auto it = std::ranges::find_if(
        cache, [&](const LargeBuffer& entry) { return entry.buffer.get() == ref.buffer; });
    ASSERT_MSG(it != cache.end() && it->held, "FreeDeferred on an allocation that is not held");
    it->held = false;
    it->tick = scheduler.CurrentTick();
    it->last_used_frame = frame;
}

void StagingBufferPool::TickFrame() {
    ++frame;
    // bbport: the device-local rings (detiling scratch) hold video memory: on a small card idle
    // blocks go after ~4 s instead of staying (up to 512 MiB) for 30 s.
    const bool tight = instance.GetTotalMemoryBudget() < 5_GB && !Vram::Legacy();
    for (size_t i = 0; i < rings.size(); ++i) {
        TrimRing(rings[i], tight && i == u32(MemoryType::DeviceLocal));
    }
    for (size_t i = 0; i < large_caches.size(); ++i) {
        TrimLarge(large_caches[i], tight && i == u32(MemoryType::DeviceLocal));
    }
    PublishStats();
}

void StagingBufferPool::TrimRing(Ring& ring, bool eager) {
    auto& blocks = ring.blocks;
    for (size_t i = blocks.size(); i-- > 0;) {
        const Block& block = blocks[i];
        if (blocks.size() <= (eager ? 1 : keep_blocks) ||
            frame - block.last_used_frame < (eager ? 240 : RING_IDLE_FRAMES) ||
            !scheduler.IsFree(block.buffer->LastTick())) {
            continue;
        }
        blocks.erase(blocks.begin() + i);
        if (i < ring.current) {
            --ring.current;
        } else if (i == ring.current) {
            ring.current = 0;
        }
    }
    if (ring.current >= blocks.size()) {
        ring.current = 0;
    }
}

void StagingBufferPool::TrimLarge(std::vector<LargeBuffer>& cache, bool eager) {
    std::erase_if(cache, [this, eager](const LargeBuffer& entry) {
        const bool idle = !entry.held &&
                          frame - entry.last_used_frame >= (eager ? 240 : LARGE_IDLE_FRAMES) &&
                          scheduler.IsFree(entry.tick);
        if (idle && StagingLog()) {
            std::printf("Staging: %llu KiB buffer idle for %llu frames, freed at frame %llu\n",
                        (unsigned long long)(entry.buffer->SizeBytes() >> 10),
                        (unsigned long long)(frame - entry.last_used_frame),
                        (unsigned long long)frame);
        }
        return idle;
    });
}

void StagingBufferPool::PublishStats() const {
    using namespace Core::Devtools::Widget;
    if (!GpuMemoryViewer::IsEnabled()) {
        return;
    }
    static constexpr std::array<const char*, NUM_TYPES> names = {"DeviceLocal", "HostUncached",
                                                                 "HostCached"};
    GpuMemoryGroup group{.name = "Staging"};
    for (size_t i = 0; i < NUM_TYPES; ++i) {
        u64 bytes = 0;
        for (const Block& block : rings[i].blocks) {
            bytes += block.buffer->SizeBytes();
        }
        for (const LargeBuffer& entry : large_caches[i]) {
            bytes += entry.buffer->SizeBytes();
        }
        if (bytes != 0) {
            group.rows.push_back({.name = names[i], .bytes = bytes});
        }
    }
    GpuMemoryViewer::Publish(std::move(group));
}

} // namespace Vulkan
