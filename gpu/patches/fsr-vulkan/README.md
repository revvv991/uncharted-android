# Local changes to gpu/third_party/fsr-vulkan

The submodule points at upstream FireBurn/FSR-Vulkan (`c64f093`). `build.sh` applies the
patches here to its working tree when they are not applied yet:

- `0001-...`: `BB_FSR4_PROFILE` (GPU time per FSR 4 pass) and `BB_FSR4_STATS` (driver
  statistics of each pass) in the FSR 4 v07 provider.
- `0002-...`: `#include <mutex>` in the FFX Vulkan backend; libc++ (the Windows MSYS2 CLANG64
  build) does not include it transitively.
- `0003-...`: FSR 3.1 frame generation tells the bridge its frame and retires it, so the
  constant buffers and descriptor sets allocated per dispatch are freed (they leaked ~2-3 MB of
  video memory a frame until the GPU ran out, in all three games).
- `0004-...`: the FFX Vulkan backend takes a host-visible device-local memory type when no
  other device-local type fits. It skipped them all for device-local requests (a guard for
  the small host-visible heap of discrete GPUs), and every device-local type of Turnip is
  host-visible: no FSR 3.1 resource could be allocated, the FSR 3.1 context failed with
  `FFX_ERROR_BACKEND_API_ERROR` (-7 from the portable API) at every size, and FSR 3.1 never
  ran on Android with Turnip (g2-arm64, 2026-10-25, issue #13).
  The Android build (`android/scripts/port-arm64/build_gpu_android.sh`) applies these patches
  too (g2-arm64: upstream 0.3.11's 0003 replaced our fix of the same leak).
