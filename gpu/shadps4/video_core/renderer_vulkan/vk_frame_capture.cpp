// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_frame_capture.h"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <format>
#include <mutex>

#include "video_core/renderer_vulkan/vk_common.h"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan_format_traits.hpp>
#include <xxhash.h>

#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/image_info.h"

namespace Vulkan {

namespace {

struct Target {
    VAddr address{};
    vk::Format format{};
    u32 width{}, height{};
    bool operator==(const Target&) const = default;
};

struct Entry {
    bool compute{};
    std::vector<Target> colors;
    Target depth;
    u32 draws{};
    u64 indices{};
    std::vector<u64> shaders; ///< vs and ps (graphics) or cs (compute), the first 64
    std::vector<std::pair<Target, bool>> sampled; ///< textures (false) and storage images (true)
    std::string note;
    std::vector<std::string> buffers; ///< dumped constants, per draw
};

// GPU thread only.
std::vector<Entry> entries;
std::vector<std::pair<Target, bool>> pending_sampled;
std::vector<std::string> pending_buffers;

void AddBuffers(Entry& entry) {
    // Constants of full-screen passes (post-processing: camera matrices) and of the first draw
    // of each geometry pass.
    if (entry.draws <= 4) {
        for (auto& b : pending_buffers) {
            entry.buffers.push_back(std::move(b));
        }
    }
    pending_buffers.clear();
}
bool pass_open = false;
// BB_CAPTURE_BY_FLIP=1: the recording spans the passes between two flips instead of starting
// and ending at passes that write a display buffer (Uncharted writes two per frame).
u64 record_start_flip = 0;
// F10 (ArmTextureDump): a by-flip recording that also saves the sampled textures.
std::atomic<bool> dump_requested{false};
// Texture dump of the current recording (GPU thread).
bool dumping = false;
std::string dump_dir;
std::vector<Target> dumped;
u64 dump_bytes = 0;
u32 dump_skipped = 0;
bool CaptureByFlip() {
    static const bool value = [] {
        const char* env = std::getenv("BB_CAPTURE_BY_FLIP");
        return env && env[0] == '1';
    }();
    return value || dump_requested.load(std::memory_order_acquire) || dumping;
}
std::mutex display_mutex;
std::vector<VAddr> display_buffers;
// bbport: read on every draw; the game registers a handful of display buffers once.
std::array<std::atomic<VAddr>, 16> display_list{};
std::atomic<u32> display_count{0};

Target ToTarget(const VideoCore::ImageInfo& info) {
    return {info.guest_address, info.pixel_format, info.size.width, info.size.height};
}

std::string Describe(const Target& t) {
    if (!t.address) {
        return "-";
    }
    return std::format("{:#x} {} {}x{}", t.address, vk::to_string(t.format), t.width, t.height);
}

void AddSampled(Entry& entry) {
    for (const auto& s : pending_sampled) {
        if (entry.sampled.size() < 256 &&
            std::ranges::find(entry.sampled, s) == entry.sampled.end()) {
            entry.sampled.push_back(s);
        }
    }
    pending_sampled.clear();
}

void AddShader(Entry& entry, u64 hash) {
    if (entry.shaders.size() < 64 && std::ranges::find(entry.shaders, hash) == entry.shaders.end()) {
        entry.shaders.push_back(hash);
    }
}

void StartDump() {
    dump_requested.store(false, std::memory_order_release);
    const char* user = std::getenv("BB_GPU_USER_DIR");
    dump_dir = std::format("{}/texture_dump/{}", user && user[0] ? user : "user",
                           static_cast<long long>(std::time(nullptr)));
    std::error_code ec;
    std::filesystem::create_directories(dump_dir, ec);
    dumped.clear();
    dump_bytes = 0;
    dump_skipped = 0;
    dumping = !ec;
    std::printf("Texture dump: recording into %s%s\n", dump_dir.c_str(),
                ec ? " failed (cannot create the folder)" : "");
}

void EndDump() {
    if (dumping) {
        std::printf("Texture dump: %zu textures (%llu MiB) saved to %s, %u skipped\n",
                    dumped.size() - dump_skipped,
                    static_cast<unsigned long long>(dump_bytes >> 20), dump_dir.c_str(),
                    dump_skipped);
    }
    dumping = false;
}

/// DXGI format of a DDS file for `format`, 0 when there is none (the dump is then .raw).
u32 DxgiFormat(vk::Format format) {
    switch (format) {
    case vk::Format::eBc1RgbaUnormBlock: case vk::Format::eBc1RgbUnormBlock: return 71;
    case vk::Format::eBc1RgbaSrgbBlock: case vk::Format::eBc1RgbSrgbBlock: return 72;
    case vk::Format::eBc2UnormBlock: return 74;
    case vk::Format::eBc2SrgbBlock: return 75;
    case vk::Format::eBc3UnormBlock: return 77;
    case vk::Format::eBc3SrgbBlock: return 78;
    case vk::Format::eBc4UnormBlock: return 80;
    case vk::Format::eBc4SnormBlock: return 81;
    case vk::Format::eBc5UnormBlock: return 83;
    case vk::Format::eBc5SnormBlock: return 84;
    case vk::Format::eBc6HUfloatBlock: return 95;
    case vk::Format::eBc6HSfloatBlock: return 96;
    case vk::Format::eBc7UnormBlock: return 98;
    case vk::Format::eBc7SrgbBlock: return 99;
    case vk::Format::eR8G8B8A8Unorm: return 28;
    case vk::Format::eR8G8B8A8Srgb: return 29;
    case vk::Format::eB8G8R8A8Unorm: return 87;
    case vk::Format::eB8G8R8A8Srgb: return 91;
    case vk::Format::eA2B10G10R10UnormPack32: return 24;
    case vk::Format::eB10G11R11UfloatPack32: return 26;
    case vk::Format::eR16G16B16A16Sfloat: return 10;
    case vk::Format::eR16G16B16A16Unorm: return 11;
    case vk::Format::eR8G8Unorm: return 49;
    case vk::Format::eR8Unorm: return 61;
    case vk::Format::eR16Sfloat: return 54;
    case vk::Format::eR16Unorm: return 56;
    case vk::Format::eR32Sfloat: return 41;
    default: return 0;
    }
}

/// A DDS file (DX10 header) with one 2D mip, or the bare data when `dxgi` is 0.
void WriteTexture(const std::string& path, u32 dxgi, u32 width, u32 height, const void* data,
                  u64 size) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        return;
    }
    if (dxgi) {
        u32 header[1 + 31 + 5]{};
        header[0] = 0x20534444;     // "DDS "
        header[1] = 124;            // dwSize
        header[2] = 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000; // caps, height, width, pixel format, linear size
        header[3] = height;
        header[4] = width;
        header[5] = static_cast<u32>(size);
        header[7] = 1;              // mips
        header[19] = 32;            // pixel format: size
        header[20] = 0x4;           // DDPF_FOURCC
        header[21] = 0x30315844;    // "DX10"
        header[27] = 0x1000;        // DDSCAPS_TEXTURE
        header[32] = dxgi;
        header[33] = 3;             // D3D10_RESOURCE_DIMENSION_TEXTURE2D
        header[35] = 1;             // array size
        std::fwrite(header, sizeof(header), 1, f);
    }
    std::fwrite(data, 1, size, f);
    std::fclose(f);
}

/// Copies mip 0 of `image` (`size` bytes) into a host buffer after the commands recorded so
/// far and writes it to `path` once the GPU is done (same steps as Runtime::DownloadImage).
bool SaveImage(Runtime& runtime, VideoCore::Image& image, u64 size, const std::string& path) {
    const vk::Format format = image.info.pixel_format;
    const u32 width = image.info.size.width, height = image.info.size.height;
    const Instance& instance = runtime.GetInstance();
    const VkBufferCreateInfo buffer_ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    };
    const VmaAllocationCreateInfo alloc_ci{
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
    };
    VkBuffer buffer{};
    VmaAllocation allocation{};
    VmaAllocationInfo allocation_info{};
    if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &buffer, &allocation,
                        &allocation_info) != VK_SUCCESS) {
        return false;
    }

    Scheduler& scheduler = runtime.GetScheduler();
    scheduler.EndRendering();
    if (runtime.Transit(&image, vk::ImageLayout::eTransferSrcOptimal,
                        vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferRead)) {
        runtime.FlushBarriers();
    }
    const vk::BufferImageCopy region{
        .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .imageExtent = {width, height, 1},
    };
    scheduler.Record([src = image.GetImage(), dst = vk::Buffer(buffer),
                      region](vk::CommandBuffer cmdbuf) {
        cmdbuf.copyImageToBuffer(src, vk::ImageLayout::eTransferSrcOptimal, dst, region);
        const vk::MemoryBarrier2 to_host{
            .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask = vk::AccessFlagBits2::eHostRead,
        };
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &to_host});
    });

    const u32 dxgi = DxgiFormat(format);
    scheduler.DeferPriorityOperation([allocator = instance.GetAllocator(), buffer, allocation,
                                      allocation_info, size, dxgi, width, height, path] {
        vmaInvalidateAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);
        WriteTexture(path, dxgi, width, height, allocation_info.pMappedData, size);
        vmaDestroyBuffer(allocator, buffer, allocation);
    });
    return true;
}

void Write(VAddr presented) {
    const char* env_dir = std::getenv("BB_CAPTURE_DIR");
    const std::string out_dir = dumping ? dump_dir : env_dir ? env_dir : ".";
    const char* dir = out_dir.c_str();
    const std::string path =
        std::format("{}/frame_{}.txt", dir, static_cast<long long>(std::time(nullptr)));
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) {
        std::printf("Frame capture: cannot write %s\n", path.c_str());
        return;
    }
    std::fprintf(f, "presented buffer %#llx\n", static_cast<unsigned long long>(presented));
    u32 index = 0;
    for (const auto& e : entries) {
        if (e.compute) {
            std::fprintf(f, "\n#%u COMPUTE dispatches %u, cs", index++, e.draws);
        } else {
            std::fprintf(f, "\n#%u PASS draws %u (indices %llu), vs/ps", index++, e.draws,
                         static_cast<unsigned long long>(e.indices));
        }
        for (const u64 h : e.shaders) {
            std::fprintf(f, " %016llx", static_cast<unsigned long long>(h));
        }
        std::fprintf(f, "\n");
        for (size_t i = 0; i < e.colors.size(); ++i) {
            std::fprintf(f, "  color%zu %s\n", i, Describe(e.colors[i]).c_str());
        }
        if (!e.compute) {
            std::fprintf(f, "  depth  %s\n", Describe(e.depth).c_str());
        }
        for (const auto& [t, storage] : e.sampled) {
            std::fprintf(f, "  %s %s\n", storage ? "writes " : "samples", Describe(t).c_str());
        }
        if (!e.note.empty()) {
            std::fprintf(f, "  note: %s\n", e.note.c_str());
        }
        for (const auto& b : e.buffers) {
            std::fprintf(f, "%s\n", b.c_str());
        }
    }
    std::fclose(f);
    std::printf("Frame capture: %zu passes written to %s\n", entries.size(), path.c_str());
}

} // namespace

void FrameCapture::OnFlip(VAddr presented_address) {
    last_presented.store(presented_address, std::memory_order_relaxed);
    flips.fetch_add(1, std::memory_order_release);
    static const char* trigger = std::getenv("BB_CAPTURE_TRIGGER");
    if (trigger && state.load(std::memory_order_relaxed) == Idle &&
        std::filesystem::exists(trigger)) {
        std::error_code ec;
        std::filesystem::remove(trigger, ec);
        state.store(Armed, std::memory_order_release);
        std::printf("Frame capture: armed\n");
    }
}

void FrameCapture::Poll() {}

bool FrameCapture::IsDisplayBuffer(VAddr address) {
    const u32 count = display_count.load(std::memory_order_acquire);
    for (u32 i = 0; i < count; ++i) {
        if (display_list[i].load(std::memory_order_relaxed) == address) {
            return true;
        }
    }
    if (count < display_list.size()) {
        return false;
    }
    std::scoped_lock lk{display_mutex};
    return std::ranges::find(display_buffers, address) != display_buffers.end();
}

void FrameCapture::AddDisplayBuffer(VAddr address) {
    std::scoped_lock lk{display_mutex};
    if (std::ranges::find(display_buffers, address) == display_buffers.end()) {
        display_buffers.push_back(address);
        const u32 count = display_count.load(std::memory_order_relaxed);
        if (count < display_list.size()) {
            display_list[count].store(address, std::memory_order_relaxed);
            display_count.store(count + 1, std::memory_order_release);
        }
    }
}

void FrameCapture::BeginPass(const VideoCore::ImageInfo* const* colors, u32 num_colors,
                             const VideoCore::ImageInfo* depth) {
    std::vector<Target> targets;
    bool display = false;
    {
        std::scoped_lock lk{display_mutex};
        for (u32 i = 0; i < num_colors; ++i) {
            targets.push_back(colors[i] ? ToTarget(*colors[i]) : Target{});
            display |= colors[i] && std::ranges::find(display_buffers, colors[i]->guest_address) !=
                                        display_buffers.end();
        }
    }
    // Frame boundaries in the command stream: the pass that writes a display buffer.
    const bool new_pass = !(pass_open && !entries.empty() && !entries.back().compute &&
                            entries.back().colors == targets);
    if (CaptureByFlip()) {
        const u64 now = flips.load(std::memory_order_acquire);
        if (state.load(std::memory_order_relaxed) == Armed) {
            entries.clear();
            pending_sampled.clear();
            pending_buffers.clear();
            pass_open = false;
            record_start_flip = now;
            if (dump_requested.load(std::memory_order_acquire)) {
                StartDump();
            }
            state.store(Recording, std::memory_order_release);
        } else if (state.load(std::memory_order_relaxed) == Recording && now >= record_start_flip + 2) {
            Write(last_presented.load(std::memory_order_relaxed));
            EndDump();
            entries.clear();
            pending_sampled.clear();
            pending_buffers.clear();
            state.store(Idle, std::memory_order_release);
            return;
        }
        display = false;
    }
    if (display && new_pass) {
        if (state.load(std::memory_order_relaxed) == Recording) {
            Write(last_presented.load(std::memory_order_relaxed));
            entries.clear();
            pending_sampled.clear();
            pending_buffers.clear();
            state.store(Idle, std::memory_order_release);
            return;
        }
        entries.clear();
        pending_sampled.clear();
        pending_buffers.clear();
        pass_open = false;
        state.store(Recording, std::memory_order_release);
    }
    if (state.load(std::memory_order_relaxed) != Recording) {
        pending_sampled.clear();
        pending_buffers.clear();
        return;
    }
    const Target d = depth ? ToTarget(*depth) : Target{};
    if (pass_open && !entries.empty() && !entries.back().compute &&
        entries.back().colors == targets && entries.back().depth == d) {
        return;
    }
    entries.push_back({.compute = false, .colors = std::move(targets), .depth = d});
    pass_open = true;
}

void FrameCapture::Draw(u64 vs_hash, u64 ps_hash, u32 num_indices, u32 num_instances) {
    if (state.load(std::memory_order_relaxed) != Recording || entries.empty() ||
        entries.back().compute) {
        return;
    }
    auto& e = entries.back();
    ++e.draws;
    e.indices += u64(num_indices) * std::max(num_instances, 1u);
    AddShader(e, vs_hash);
    AddShader(e, ps_hash);
    AddSampled(e);
    AddBuffers(e);
}

void FrameCapture::Dispatch(u64 cs_hash, u32 x, u32 y, u32 z) {
    if (state.load(std::memory_order_relaxed) != Recording) {
        pending_sampled.clear();
        pending_buffers.clear();
        return;
    }
    if (!entries.empty() && entries.back().compute && !entries.back().shaders.empty() &&
        entries.back().shaders[0] == cs_hash) {
        ++entries.back().draws;
    } else {
        entries.push_back({.compute = true, .draws = 1, .shaders = {cs_hash}});
        entries.back().note = std::format("groups {}x{}x{}", x, y, z);
    }
    AddSampled(entries.back());
    AddBuffers(entries.back());
    pass_open = false;
}

void FrameCapture::Buffer(u64 stage_hash, u32 slot, VAddr address, const void* data, u64 size) {
    const u64 bytes = std::min<u64>(size, 1024) & ~u64(3);
    std::string out = std::format("  buffer stage {:016x} slot {} at {:#x} size {}:", stage_hash,
                                  slot, address, size);
    const auto* words = static_cast<const float*>(data);
    for (u64 i = 0; i < bytes / 4; ++i) {
        if (i % 8 == 0) {
            out += std::format("\n    [{:3}]", i);
        }
        out += std::format(" {:12.6g}", words[i]);
    }
    pending_buffers.push_back(std::move(out));
}

void FrameCapture::Sampled(const VideoCore::ImageInfo& info, bool storage) {
    if (pending_sampled.size() < 256) {
        pending_sampled.emplace_back(ToTarget(info), storage);
    }
}

void FrameCapture::Note(const char* text) {
    if (!entries.empty()) {
        entries.back().note += text;
    }
}

void FrameCapture::ArmTextureDump() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_TEXTURE_DUMP");
        return env && env[0] == '1';
    }();
    if (!enabled || state.load(std::memory_order_acquire) != Idle) {
        return;
    }
    dump_requested.store(true, std::memory_order_release);
    state.store(Armed, std::memory_order_release);
    std::printf("Texture dump: armed (F10)\n");
}

void FrameCapture::DumpTexture(Runtime& runtime, VideoCore::Image& image) {
    if (!dumping || state.load(std::memory_order_relaxed) != Recording) {
        return;
    }
    // Textures the game loaded; render targets, depth and multisampled images are left out.
    const auto& info = image.info;
    if (info.props.is_depth || info.num_samples > 1 || image.usage.render_target ||
        image.usage.depth_target || image.usage.vo_surface || image.binding.is_target ||
        image.scene_proxy || image.backing->num_samples != 1 ||
        !(image.usage_flags & vk::ImageUsageFlagBits::eTransferSrc)) {
        return;
    }
    const Target target = ToTarget(info);
    if (std::ranges::find(dumped, target) != dumped.end()) {
        return;
    }
    dumped.push_back(target);
    const vk::Format format = info.pixel_format;
    const u32 block_size = vk::blockSize(format);
    const auto block = vk::blockExtent(format);
    const u32 width = info.size.width, height = info.size.height;
    const u64 size = block_size ? u64((width + block[0] - 1) / block[0]) *
                                      ((height + block[1] - 1) / block[1]) * block_size
                                : 0;
    if (size == 0 || size > (64ull << 20) || dump_bytes + size > (2048ull << 20)) {
        ++dump_skipped;
        return;
    }
    dump_bytes += size;
    const u8* guest = reinterpret_cast<const u8*>(info.guest_address);
    const auto& mip = info.mips_layout[0];
    // The guest data hash of mip 0 names the texture for a later replacement.
    const u64 hash = guest && mip.size ? XXH3_64bits(guest + mip.offset, mip.size) : 0;
    std::string name = vk::to_string(format);
    std::erase(name, ' ');
    const std::string path =
        std::format("{}/{:04}_{:016x}_{:x}_{}x{}_{}.{}", dump_dir, dumped.size() - 1, hash,
                    info.guest_address, width, height, name, DxgiFormat(format) ? "dds" : "raw");
    if (!SaveImage(runtime, image, size, path)) {
        ++dump_skipped;
    }
}

bool FrameCapture::CollectingIcons() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_ICON_COLLECT");
        return env && env[0] == '1';
    }();
    return enabled;
}

void FrameCapture::CollectIcon(Runtime& runtime, VideoCore::Image& image) {
    const auto& info = image.info;
    const vk::Format format = info.pixel_format;
    bool uncompressed = false; // icon sheets (U2), but also the font cache that changes all the time
    switch (format) {
    case vk::Format::eBc1RgbaUnormBlock:
    case vk::Format::eBc1RgbaSrgbBlock:
    case vk::Format::eBc3UnormBlock:
    case vk::Format::eBc3SrgbBlock:
    case vk::Format::eBc4UnormBlock:
    case vk::Format::eBc7UnormBlock:
    case vk::Format::eBc7SrgbBlock:
        break;
    case vk::Format::eR8G8B8A8Unorm:
    case vk::Format::eR8G8B8A8Srgb:
    case vk::Format::eB8G8R8A8Unorm:
    case vk::Format::eB8G8R8A8Srgb:
        uncompressed = true;
        break;
    default:
        return; // the frame's own images
    }
    const u32 width = info.size.width, height = info.size.height;
    const u8* guest = reinterpret_cast<const u8*>(info.guest_address);
    const auto& mip = info.mips_layout[0];
    if (width < 32 || height < 32 || width > 1024 || height > 1024 || !guest || !mip.size ||
        image.usage.render_target || image.binding.is_target || image.backing->num_samples != 1 ||
        !(image.usage_flags & vk::ImageUsageFlagBits::eTransferSrc)) {
        return;
    }
    // GPU thread only. Pictures are hashed at every use: one slot can show different prompts
    // over time (a handful of small pictures per frame). An uncompressed slot is saved at most 4
    // times: a sheet can be seen empty before the game fills it, and the font cache changes all
    // the time.
    static std::vector<u64> saved;
    static std::vector<std::pair<Target, u32>> uncompressed_slots;
    const u64 hash = XXH3_64bits(guest + mip.offset, mip.size);
    if (saved.size() >= 500 || std::ranges::find(saved, hash) != saved.end()) {
        return;
    }
    if (uncompressed) {
        const Target target = ToTarget(info);
        auto it = std::ranges::find(uncompressed_slots, target, &std::pair<Target, u32>::first);
        if (it == uncompressed_slots.end()) {
            if (uncompressed_slots.size() >= 64) {
                return;
            }
            it = uncompressed_slots.insert(uncompressed_slots.end(), {target, 0});
        }
        if (it->second >= 4) {
            return;
        }
        ++it->second;
    }
    saved.push_back(hash);
    static const std::string dir = [] {
        const char* user = std::getenv("BB_GPU_USER_DIR");
        const std::string path = std::format("{}/button_icons", user && user[0] ? user : "user");
        std::error_code ec;
        std::filesystem::create_directories(path, ec);
        return path;
    }();
    std::string name = vk::to_string(format);
    std::erase(name, ' ');
    const std::string path =
        std::format("{}/{:016x}_{}x{}_{}.dds", dir, hash, width, height, name);
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        return;
    }
    const auto block = vk::blockExtent(format);
    const u64 size = u64((width + block[0] - 1) / block[0]) * ((height + block[1] - 1) / block[1]) *
                     vk::blockSize(format);
    if (SaveImage(runtime, image, size, path)) {
        std::printf("Button icons: saved %s\n", path.c_str());
    }
}

} // namespace Vulkan
