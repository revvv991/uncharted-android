// bbport: the MemoryManager surface used by the video core and GnmDriver.
// Guest memory lives at identical host addresses; the C runtime owns the VMAs.
#pragma once
#include "common/types.h"
#include "common/singleton.h"
#include "core/address_space.h"

namespace Vulkan { class Rasterizer; }

namespace Core {
class MemoryManager {
public:
    void SetRasterizer(Vulkan::Rasterizer* rasterizer_);
    AddressSpace& GetAddressSpace() { return impl; }
    bool IsValidGpuMapping(VAddr virtual_addr, u64 size) {
        // The PS4's GPU can only handle 40 bit addresses.
        return virtual_addr + size < 0x100'0000'0000ULL;
    }
    u64 ClampRangeSize(VAddr virtual_addr, u64 size);
    /// g2-arm64: the whole range is mapped guest memory (readable at its address).
    bool IsHostMapped(VAddr virtual_addr, u64 size);
    void CopySparseMemory(VAddr source, u8* dest, u64 size);
    bool TryWriteBacking(void* address, const void* data, u64 size);
    /// g2-arm64: guest bytes through the backing view, whatever the page's protection.
    bool TryReadBacking(const void* address, void* data, u64 size);
    /// g2-arm64 (0196): `data` lands only on the words still equal to `before` (a
    /// compare-and-swap each); the bytes kept, or -1 when part had no backing.
    s64 LandBacking(void* address, const void* before, const void* data, u64 size);
    /// CPU wrote guest memory outside page tracking (e.g. decoded video frames).
    void InvalidateMemory(VAddr address, u64 size);
    Vulkan::Rasterizer* GetRasterizer() const { return rasterizer; }

private:
    AddressSpace impl;
    Vulkan::Rasterizer* rasterizer{};
};
using Memory = Common::Singleton<MemoryManager>;
} // namespace Core
