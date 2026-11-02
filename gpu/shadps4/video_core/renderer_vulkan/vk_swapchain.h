// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <mutex>
#include <vector>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Frontend {
class WindowSDL;
}

namespace Vulkan {

class Instance;
class Scheduler;

class Swapchain {
public:
    explicit Swapchain(const Instance& instance, const Frontend::WindowSDL& window);
    ~Swapchain();

    /// Creates (or recreates) the swapchain with a given size.
    void Create(u32 width, u32 height);

    /// Recreates the swapchain with a given size and current surface.
    void Recreate(u32 width, u32 height);

    /// Acquires the next image in the swapchain.
    bool AcquireNextImage();

    /// g2-arm64 (Android): whether there is a surface to present to now: not while the app is in
    /// the background or Android has no window for it; after it gave a new window, a new surface
    /// and swapchain. Elsewhere always true.
    bool SurfaceReady();

    /// g2-arm64: the last AcquireNextImage timed out (the compositor took no buffer): skip the
    /// frame rather than recreate the swapchain.
    bool AcquireTimedOut() const {
        return acquire_timed_out;
    }

    /// Presents the current image and move to the next one. g2-arm64: with display timing
    /// (BB_PRESENT_PACING) a desired present time (CLOCK_MONOTONIC ns, 0 = at once).
    bool Present(s64 desired_present_ns = 0, const s64* trace = nullptr, s64 trace_arg = 0);

    vk::SurfaceKHR GetSurface() const {
        return surface;
    }

    vk::Image Image() const {
        return images[image_index];
    }

    vk::ImageView ImageView() const {
        return images_view[image_index];
    }

    vk::SurfaceFormatKHR GetSurfaceFormat() const {
        return surface_format;
    }

    vk::SwapchainKHR GetHandle() const {
        return swapchain;
    }

    u32 GetWidth() const {
        return width;
    }

    u32 GetHeight() const {
        return height;
    }

    u32 GetImageCount() const {
        return image_count;
    }

    u32 GetFrameIndex() const {
        return frame_index;
    }

    /// g2-arm64: with pre-rotation, the image size in the display's orientation; the frame's
    /// (logical) size is LogicalExtent().
    vk::Extent2D GetExtent() const {
        return extent;
    }

    /// g2-arm64: Android pre-rotation: clockwise quarter turns the presenter applies to the frame
    /// (the surface's transform), 0 when the compositor rotates.
    u32 QuarterTurns() const {
        return quarter_turns;
    }

    /// The extent in the frame's orientation (landscape on the G2).
    vk::Extent2D LogicalExtent() const {
        return quarter_turns & 1 ? vk::Extent2D{extent.height, extent.width} : extent;
    }

    [[nodiscard]] vk::Semaphore GetImageAcquiredSemaphore() const {
        return image_acquired[frame_index];
    }

    [[nodiscard]] vk::Semaphore GetPresentReadySemaphore() const {
        return present_ready[image_index];
    }

    bool HasHDR() const {
        return supports_hdr;
    }

    void SetHDR(bool hdr);

    bool GetHDR() const {
        return needs_hdr;
    }

private:
    /// Selects the best available swapchain image format
    void FindPresentFormat();

    /// Selects the best available present mode
    void FindPresentMode();

    /// Sets the surface properties according to device capabilities (g2-arm64: false when the
    /// surface is gone on Android)
    bool SetSurfaceProperties();

    /// Destroys current swapchain resources
    void Destroy();

    /// Performs creation of image views and framebuffers from the swapchain images
    void SetupImages();

    /// Creates the image acquired and present ready semaphores
    void RefreshSemaphores();

    /// g2-arm64: BB_PRESENT_PACING: the vsyncs of the frames SurfaceFlinger has shown since the
    /// last call (bbport_present_pacing.h).
    void NotePresentTimes();

private:
    const Instance& instance;
    const Frontend::WindowSDL& window;
    vk::SwapchainKHR swapchain{};
    vk::SurfaceKHR surface{};
    vk::SurfaceFormatKHR surface_format;
    vk::Format view_format;
    vk::PresentModeKHR present_mode;
    vk::Extent2D extent;
    vk::SurfaceTransformFlagBitsKHR transform;
    u32 quarter_turns = 0; ///< g2-arm64: pre-rotation applied by the presenter
    vk::CompositeAlphaFlagBitsKHR composite_alpha;
    std::vector<vk::Image> images;
    std::vector<vk::ImageView> images_view;
    std::vector<vk::Semaphore> image_acquired;
    std::vector<vk::Semaphore> present_ready;
    u32 width = 0;
    u32 height = 0;
    u32 image_count = 0;
    u32 image_index = 0;
    u32 frame_index = 0;
    bool needs_recreation = true;
    void* surface_window = nullptr; ///< g2-arm64: the native window the surface was made from
    bool surface_lost = false;      ///< g2-arm64: no usable surface or swapchain until a new one
    bool acquire_timed_out = false;
    bool present_timing = false; ///< g2-arm64: presents carry IDs and desired times
    u32 present_id = 0;
    bool needs_hdr = false;    // The game requested HDR swapchain
    bool supports_hdr = false; // SC supports HDR output
};

} // namespace Vulkan
