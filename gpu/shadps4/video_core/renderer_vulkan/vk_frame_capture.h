// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: frame analyzer. Creating the file named by BB_CAPTURE_TRIGGER records the next full
// frame: every render pass (targets, depth, draw count, shaders, sampled textures) and compute
// dispatch, in order, plus the buffer that was presented. Input for placing a temporal
// upscaler (scene color, depth, motion vectors, UI) in Bloodborne's frame.

#pragma once

#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

#include "common/types.h"

namespace VideoCore {
struct ImageInfo;
struct Image;
} // namespace VideoCore

namespace Vulkan {

class Runtime;

class FrameCapture {
public:
    /// VideoOut thread, per flip: frame boundary, presented buffer, trigger file check.
    static void OnFlip(VAddr presented_address);

    /// VideoOut: a display buffer the game registered. A pass drawing into one starts a frame
    /// in the GPU command stream (the flip itself runs a frame behind).
    static void AddDisplayBuffer(VAddr address);
    static bool IsDisplayBuffer(VAddr address);

    /// GPU thread: whether passes are being looked at (armed or recording; cheap).
    static bool Active() {
        return state.load(std::memory_order_relaxed) != Idle;
    }
    /// GPU thread, at every draw/dispatch: starts or ends a recording at frame boundaries.
    static void Poll();

    static void BeginPass(const VideoCore::ImageInfo* const* colors, u32 num_colors,
                          const VideoCore::ImageInfo* depth);
    static void Draw(u64 vs_hash, u64 ps_hash, u32 num_indices, u32 num_instances);
    static void Dispatch(u64 cs_hash, u32 x, u32 y, u32 z);
    static void Sampled(const VideoCore::ImageInfo& info, bool storage);
    /// Contents of a bound buffer (first 1 KiB), kept for small passes and a pass's first draw.
    static void Buffer(u64 stage_hash, u32 slot, VAddr address, const void* data, u64 size);
    static void Note(const char* text);

    /// Window thread, F10 with BB_TEXTURE_DUMP=1: records the frames between the next flips and
    /// saves every texture they sample (mip 0 as .dds) next to the pass list, in
    /// <user>/texture_dump/<time>/.
    static void ArmTextureDump();
    /// GPU thread, for each sampled texture: the copy for ArmTextureDump while recording.
    static void DumpTexture(Runtime& runtime, VideoCore::Image& image);

    /// BB_ICON_COLLECT=1: each compressed picture that a pass drawing into a display buffer
    /// samples (the HUD and button prompts) is saved once, in <user>/button_icons, to add new
    /// prompts to button_prompts.cpp. Those draws then skip the texture binding helper and memo.
    static bool CollectingIcons();
    static void CollectIcon(Runtime& runtime, VideoCore::Image& image);

private:
    enum : u32 { Idle, Armed, Recording };
    static inline std::atomic<u32> state{Idle};
    static inline std::atomic<u64> flips{0};
    static inline std::atomic<VAddr> last_presented{0};
};

} // namespace Vulkan
