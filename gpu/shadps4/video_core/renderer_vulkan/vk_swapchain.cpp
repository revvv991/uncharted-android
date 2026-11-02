// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <limits>
#include "bbport_present_pacing.h"
#include "bbport_settings.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "core/emulator_settings.h"
#include "imgui/renderer/imgui_core.h"
#include "sdl_window.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"

namespace Vulkan {

static constexpr vk::SurfaceFormatKHR SURFACE_FORMAT_HDR = {
    .format = vk::Format::eA2B10G10R10UnormPack32,
    .colorSpace = vk::ColorSpaceKHR::eHdr10St2084EXT,
};

Swapchain::Swapchain(const Instance& instance_, const Frontend::WindowSDL& window_)
    : instance{instance_}, window{window_}, surface{CreateSurface(instance.GetInstance(), window)} {
    surface_window = window.CurrentWindowInfo().render_surface;
    FindPresentFormat();
    FindPresentMode();

    Create(window.GetWidth(), window.GetHeight());
    ImGui::Core::Initialize(instance, window, image_count, surface_format.format);
}

Swapchain::~Swapchain() {
    Destroy();
    instance.GetInstance().destroySurfaceKHR(surface);
}

void Swapchain::Create(u32 width_, u32 height_) {
    width = width_;
    height = height_;
    needs_recreation = false;

    Destroy();

    if (!SetSurfaceProperties()) {
        surface_lost = true;
        return;
    }

    const std::array queue_family_indices = {
        instance.GetGraphicsQueueFamilyIndex(),
        instance.GetPresentQueueFamilyIndex(),
    };

    const bool exclusive = queue_family_indices[0] == queue_family_indices[1];
    const u32 queue_family_indices_count = exclusive ? 1u : 2u;
    const vk::SharingMode sharing_mode =
        exclusive ? vk::SharingMode::eExclusive : vk::SharingMode::eConcurrent;
    const auto format = needs_hdr ? SURFACE_FORMAT_HDR : surface_format;
    const vk::SwapchainCreateInfoKHR swapchain_info = {
        .surface = surface,
        .minImageCount = image_count,
        .imageFormat = format.format,
        .imageColorSpace = format.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = vk::ImageUsageFlagBits::eColorAttachment |
                      vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst,
        .imageSharingMode = sharing_mode,
        .queueFamilyIndexCount = queue_family_indices_count,
        .pQueueFamilyIndices = queue_family_indices.data(),
        .preTransform = transform,
        .compositeAlpha = composite_alpha,
        .presentMode = present_mode,
        .clipped = true,
        .oldSwapchain = nullptr,
    };

    auto [swapchain_result, chain] = instance.GetDevice().createSwapchainKHR(swapchain_info);
#ifdef __ANDROID__
    if (swapchain_result != vk::Result::eSuccess) {
        // g2-arm64: the window went away under the surface: present again once Android gives
        // a new one (SurfaceReady).
        std::printf("Swapchain: not created (%s): presenting waits for a new window\n",
                    vk::to_string(swapchain_result).c_str());
        swapchain = nullptr;
        surface_lost = true;
        return;
    }
#endif
    ASSERT_MSG(swapchain_result == vk::Result::eSuccess, "Failed to create swapchain: {}",
               vk::to_string(swapchain_result));
    swapchain = chain;

    SetupImages();
    RefreshSemaphores();

    // g2-arm64: BB_PRESENT_PACING (bbport_present_pacing.h): the display's refresh period.
    present_timing = instance.IsDisplayTimingSupported();
    if (present_timing) {
        VkRefreshCycleDurationGOOGLE cycle{};
        if (VULKAN_HPP_DEFAULT_DISPATCHER.vkGetRefreshCycleDurationGOOGLE(
                static_cast<VkDevice>(instance.GetDevice()), static_cast<VkSwapchainKHR>(swapchain),
                &cycle) == VK_SUCCESS &&
            cycle.refreshDuration > 0) {
            BbPresentPacing::refresh_ns.store(s64(cycle.refreshDuration), std::memory_order_relaxed);
        }
        static bool told = false;
        if (!told) {
            told = true;
            std::printf("Present pacing: %s, refresh %.3f ms, %u images, a request taken %.1f ms "
                        "before its vsync to start (%s, at most %.1f; a late one at least %.1f)\n",
                        BbPresentPacing::Mode() == 1 ? "on" : "statistics only",
                        BbPresentPacing::refresh_ns.load(std::memory_order_relaxed) / 1e6,
                        image_count, BbPresentPacing::Lead() / 1e6,
                        BbPresentPacing::Adapt() ? "adapting" : "fixed",
                        BbPresentPacing::LeadMax() / 1e6, BbPresentPacing::LeadMin() / 1e6);
        }
    }
}

void Swapchain::Recreate(u32 width_, u32 height_) {
    LOG_DEBUG(Render_Vulkan, "Recreate the swapchain: width={} height={} HDR={}", width_, height_,
              needs_hdr);
    Create(width_, height_);
}

void Swapchain::SetHDR(bool hdr) {
    if (needs_hdr == hdr) {
        return;
    }

    auto result = instance.GetDevice().waitIdle();
    if (result != vk::Result::eSuccess) {
        LOG_WARNING(ImGui, "Failed to wait for Vulkan device idle on mode change: {}",
                    vk::to_string(result));
    }

    needs_hdr = hdr;
    Recreate(width, height);
    ImGui::Core::OnSurfaceFormatChange(needs_hdr ? SURFACE_FORMAT_HDR.format
                                                 : surface_format.format);
}

bool Swapchain::AcquireNextImage() {
    vk::Device device = instance.GetDevice();
    acquire_timed_out = false;
    if (surface_lost || !swapchain) {
        needs_recreation = true;
        return false;
    }
#ifdef __ANDROID__
    // g2-arm64: with the screen off the compositor stops taking buffers: never wait for one
    // longer than a few frames.
    constexpr u64 timeout = 100'000'000;
#else
    constexpr u64 timeout = std::numeric_limits<u64>::max();
#endif
    vk::Result result = device.acquireNextImageKHR(swapchain, timeout, image_acquired[frame_index],
                                                   VK_NULL_HANDLE, &image_index);

    switch (result) {
    case vk::Result::eSuccess:
        break;
#ifdef __ANDROID__
    case vk::Result::eTimeout:
    case vk::Result::eNotReady:
        acquire_timed_out = true;
        return false;
#endif
#ifdef __ANDROID__
    case vk::Result::eSuboptimalKHR:
        // g2-arm64: an identity-transform swapchain on a rotated display (the G2's panel is
        // portrait) is suboptimal on every frame; the compositor rotates it. Not a reason to
        // recreate it each frame.
        break;
#else
    case vk::Result::eSuboptimalKHR:
#endif
    case vk::Result::eErrorSurfaceLostKHR:
        surface_lost = true; // g2-arm64: a new surface, not only a new swapchain
        [[fallthrough]];
    case vk::Result::eErrorOutOfDateKHR:
    case vk::Result::eErrorUnknown:
        needs_recreation = true;
        break;
    default:
        LOG_CRITICAL(Render_Vulkan, "Swapchain acquire returned unknown result {}",
                     vk::to_string(result));
        UNREACHABLE();
        break;
    }

    return !needs_recreation;
}

bool Swapchain::Present(s64 desired_present_ns, const s64* trace, s64 trace_arg) {
    vk::PresentInfoKHR present_info = {
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &present_ready[image_index],
        .swapchainCount = 1,
        .pSwapchains = &swapchain,
        .pImageIndices = &image_index,
    };
    // g2-arm64: BB_PRESENT_PACING: every present carries an ID (its vsync comes back in the past
    // presentation timings), a paced one its desired present time.
    const vk::PresentTimeGOOGLE present_time{
        .presentID = ++present_id,
        .desiredPresentTime = u64(std::max<s64>(desired_present_ns, 0)),
    };
    const vk::PresentTimesInfoGOOGLE present_times{
        .swapchainCount = 1,
        .pTimes = &present_time,
    };
    if (present_timing) {
        present_info.pNext = &present_times;
    }

    // g2-arm64: BB_PRESENT_TRACE: the frame's times so far, kept until it is shown.
    if (trace && present_timing) {
        BbPresentPacing::NotePresented(present_id, trace_arg, trace, BbPresentPacing::NowNs());
    }
    auto result = instance.GetPresentQueue().presentKHR(present_info);
#ifdef __ANDROID__
    if (result == vk::Result::eSuboptimalKHR) {
        result = vk::Result::eSuccess; // rotated display, see AcquireNextImage
    }
#endif
    if (result == vk::Result::eErrorOutOfDateKHR || result == vk::Result::eSuboptimalKHR) {
        needs_recreation = true;
#ifdef __ANDROID__
    } else if (result == vk::Result::eErrorSurfaceLostKHR) {
        needs_recreation = true; // g2-arm64: the window went away (SurfaceReady)
        surface_lost = true;
#endif
    } else {
        ASSERT_MSG(result == vk::Result::eSuccess, "Swapchain presentation failed: {}",
                   vk::to_string(result));
    }

    frame_index = (frame_index + 1) % image_count;
    NotePresentTimes();

    return !needs_recreation;
}

void Swapchain::NotePresentTimes() {
    if (!present_timing || !swapchain) {
        return;
    }
    std::array<VkPastPresentationTimingGOOGLE, 16> past{};
    u32 count = u32(past.size());
    const VkResult result = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPastPresentationTimingGOOGLE(
        static_cast<VkDevice>(instance.GetDevice()), static_cast<VkSwapchainKHR>(swapchain), &count,
        past.data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        return;
    }
    for (u32 i = 0; i < count; ++i) {
        BbPresentPacing::NoteShown(past[i].presentID, s64(past[i].desiredPresentTime),
                                   s64(past[i].actualPresentTime), s64(past[i].presentMargin));
    }
}

bool Swapchain::SurfaceReady() {
#ifdef __ANDROID__
    if (window.InBackground()) {
        return false;
    }
    void* const current = window.CurrentWindowInfo().render_surface;
    if (!current) {
        return false;
    }
    if (current == surface_window && !surface_lost) {
        return true;
    }
    // A new window from Android (the app came back), or the old surface died: a new surface and
    // swapchain on the current window.
    std::printf("Swapchain: a new surface for the window %p (was %p, lost %d)\n", current,
                surface_window, int(surface_lost));
    Destroy();
    swapchain = nullptr;
    instance.GetInstance().destroySurfaceKHR(surface);
    surface = CreateSurface(instance.GetInstance(), window);
    surface_window = current;
    surface_lost = false;
    Create(width, height);
    return !surface_lost;
#else
    return true;
#endif
}

void Swapchain::FindPresentFormat() {
    const auto [formats_result, formats] =
        instance.GetPhysicalDevice().getSurfaceFormatsKHR(surface);
    ASSERT_MSG(formats_result == vk::Result::eSuccess, "Failed to query surface formats: {}",
               vk::to_string(formats_result));

    // Check if the device supports HDR formats. Here we care of Rec.2020 PQ only as it is expected
    // game output. Other variants as e.g. linear Rec.2020 will require additional color space
    // rotation
    supports_hdr =
        std::find_if(formats.begin(), formats.end(), [](const vk::SurfaceFormatKHR& format) {
            return format == SURFACE_FORMAT_HDR;
        }) != formats.end();
    // Also make sure that user allowed us to use HDR
    supports_hdr &= EmulatorSettings.IsHdrAllowed();

    // If there is a single undefined surface format, the device doesn't care, so we'll just use
    // RGBA sRGB.
    if (formats[0].format == vk::Format::eUndefined) {
        surface_format.format = vk::Format::eR8G8B8A8Unorm;
        surface_format.colorSpace = vk::ColorSpaceKHR::eSrgbNonlinear;
        return;
    }

    // Try to find a suitable format.
    for (const vk::SurfaceFormatKHR& sformat : formats) {
        vk::Format format = sformat.format;
        if (format != vk::Format::eR8G8B8A8Unorm && format != vk::Format::eB8G8R8A8Unorm) {
            continue;
        }

        surface_format.format = format;
        surface_format.colorSpace = sformat.colorSpace;
        return;
    }

    UNREACHABLE_MSG("Unable to find required swapchain format!");
}

void Swapchain::FindPresentMode() {
    const auto [modes_result, modes] =
        instance.GetPhysicalDevice().getSurfacePresentModesKHR(surface);
    if (modes_result != vk::Result::eSuccess) {
        LOG_ERROR(Render, "Failed to query available present modes, falling back to Fifo as "
                          "guaranteed supported option.");
        present_mode = vk::PresentModeKHR::eFifo;
        return;
    }

    const auto requested_mode = EmulatorSettings.GetPresentMode();
    if (requested_mode == "Mailbox") {
        present_mode = vk::PresentModeKHR::eMailbox;
    } else if (requested_mode == "Fifo") {
        present_mode = vk::PresentModeKHR::eFifo;
    } else if (requested_mode == "Immediate") {
        present_mode = vk::PresentModeKHR::eImmediate;
    } else {
        LOG_ERROR(Render_Vulkan, "Unknown present mode {}, defaulting to Mailbox.",
                  EmulatorSettings.GetPresentMode());
        present_mode = vk::PresentModeKHR::eMailbox;
    }

    if (std::ranges::find(modes, present_mode) == modes.cend()) {
        // FIFO is guaranteed to be supported by the Vulkan spec.
        constexpr auto fallback = vk::PresentModeKHR::eFifo;
        LOG_WARNING(Render, "Requested present mode {} is not supported, falling back to {}.",
                    vk::to_string(present_mode), vk::to_string(fallback));
        present_mode = fallback;
    }
}

bool Swapchain::SetSurfaceProperties() {
    const auto [capabilities_result, capabilities] =
        instance.GetPhysicalDevice().getSurfaceCapabilitiesKHR(surface);
#ifdef __ANDROID__
    if (capabilities_result == vk::Result::eErrorSurfaceLostKHR) {
        return false; // g2-arm64: the window went away (SurfaceReady)
    }
#endif
    ASSERT_MSG(capabilities_result == vk::Result::eSuccess,
               "Failed to query surface capabilities: {}", vk::to_string(capabilities_result));

    extent = capabilities.currentExtent;
    if (capabilities.currentExtent.width == std::numeric_limits<u32>::max()) {
        extent.width = std::max(capabilities.minImageExtent.width,
                                std::min(capabilities.maxImageExtent.width, width));
        extent.height = std::max(capabilities.minImageExtent.height,
                                 std::min(capabilities.maxImageExtent.height, height));
    }

    // Select number of images in swap chain, we prefer one buffer in the background to work on
    // (bbport: two with frame generation, which presents several images per game frame).
    image_count = capabilities.minImageCount +
                  (BbSettings::Get().frame_gen != BbSettings::FrameGenOff ? 2 : 1);
    if (capabilities.maxImageCount > 0) {
        image_count = std::min(image_count, capabilities.maxImageCount);
    }

    // Prefer identity transform if possible
    transform = vk::SurfaceTransformFlagBitsKHR::eIdentity;
    quarter_turns = 0;
#ifdef __ANDROID__
    // g2-arm64: Android pre-rotation. The G2's panel is portrait: an identity swapchain made the
    // compositor rotate every frame on the GPU (client composition, `dumpsys SurfaceFlinger`),
    // its GPU context preempting the game's. With the display's transform the presenter draws
    // the frame rotated into an image in the panel's orientation and the compositor scans it
    // out as it is (device composition). BB_PRE_ROTATE=0: off.
    static const bool pre_rotate = [] {
        const char* env = std::getenv("BB_PRE_ROTATE");
        return !env || env[0] != '0';
    }();
    if (pre_rotate && (capabilities.supportedTransforms & capabilities.currentTransform)) {
        switch (capabilities.currentTransform) {
        case vk::SurfaceTransformFlagBitsKHR::eRotate90:
            quarter_turns = 1;
            break;
        case vk::SurfaceTransformFlagBitsKHR::eRotate180:
            quarter_turns = 2;
            break;
        case vk::SurfaceTransformFlagBitsKHR::eRotate270:
            quarter_turns = 3;
            break;
        default:
            break;
        }
        if (quarter_turns) {
            transform = capabilities.currentTransform;
            // currentExtent is in the current (rotated) orientation: the images are in the
            // panel's.
            if (quarter_turns & 1) {
                std::swap(extent.width, extent.height);
            }
        }
        static bool logged = false;
        if (!logged) {
            logged = true;
            std::printf("Swapchain: display transform %s, pre-rotation %u quarter turns, %ux%u\n",
                        vk::to_string(capabilities.currentTransform).c_str(), quarter_turns,
                        extent.width, extent.height);
        }
    }
#endif
    if (!(capabilities.supportedTransforms & transform)) {
        transform = capabilities.currentTransform;
    }

    // Opaque is not supported everywhere.
    composite_alpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
    if (!(capabilities.supportedCompositeAlpha & vk::CompositeAlphaFlagBitsKHR::eOpaque)) {
        composite_alpha = vk::CompositeAlphaFlagBitsKHR::eInherit;
    }
    return true;
}

void Swapchain::Destroy() {
    vk::Device device = instance.GetDevice();
    const auto wait_result = device.waitIdle();
    if (wait_result != vk::Result::eSuccess) {
        LOG_WARNING(Render_Vulkan, "Failed to wait for device to become idle: {}",
                    vk::to_string(wait_result));
    }

    for (auto& image_view : images_view) {
        device.destroyImageView(image_view);
    }
    images_view.clear();

    if (swapchain) {
        device.destroySwapchainKHR(swapchain);
    }

    for (const auto& sem : image_acquired) {
        device.destroySemaphore(sem);
    }
    for (const auto& sem : present_ready) {
        device.destroySemaphore(sem);
    }

    image_acquired.clear();
    present_ready.clear();
}

void Swapchain::RefreshSemaphores() {
    const vk::Device device = instance.GetDevice();
    image_acquired.resize(image_count);
    present_ready.resize(image_count);

    for (vk::Semaphore& semaphore : image_acquired) {
        auto [semaphore_result, sem] = device.createSemaphore({});
        ASSERT_MSG(semaphore_result == vk::Result::eSuccess,
                   "Failed to create image acquired semaphore: {}",
                   vk::to_string(semaphore_result));
        semaphore = sem;
    }
    for (vk::Semaphore& semaphore : present_ready) {
        auto [semaphore_result, sem] = device.createSemaphore({});
        ASSERT_MSG(semaphore_result == vk::Result::eSuccess,
                   "Failed to create present ready semaphore: {}", vk::to_string(semaphore_result));
        semaphore = sem;
    }

    for (u32 i = 0; i < image_count; ++i) {
        SetObjectName(device, image_acquired[i], "Swapchain Semaphore: image_acquired {}", i);
        SetObjectName(device, present_ready[i], "Swapchain Semaphore: present_ready {}", i);
    }
}

void Swapchain::SetupImages() {
    vk::Device device = instance.GetDevice();
    auto [images_result, imgs] = device.getSwapchainImagesKHR(swapchain);
    ASSERT_MSG(images_result == vk::Result::eSuccess, "Failed to create swapchain images: {}",
               vk::to_string(images_result));
    images = std::move(imgs);
    image_count = static_cast<u32>(images.size());
    images_view.resize(image_count);
    for (u32 i = 0; i < image_count; ++i) {
        if (images_view[i]) {
            device.destroyImageView(images_view[i]);
        }
        auto [im_view_result, im_view] = device.createImageView(vk::ImageViewCreateInfo{
            .image = images[i],
            .viewType = vk::ImageViewType::e2D,
            .format = needs_hdr ? SURFACE_FORMAT_HDR.format : surface_format.format,
            .subresourceRange =
                {
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .levelCount = 1,
                    .layerCount = 1,
                },
        });
        ASSERT_MSG(im_view_result == vk::Result::eSuccess, "Failed to create image view: {}",
                   vk::to_string(im_view_result));
        images_view[i] = im_view;
    }

    for (u32 i = 0; i < image_count; ++i) {
        SetObjectName(device, images[i], "Swapchain Image {}", i);
        SetObjectName(device, images_view[i], "Swapchain ImageView {}", i);
    }
}

} // namespace Vulkan
