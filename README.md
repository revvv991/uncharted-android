# uncharted-android

Uncharted: The Nathan Drake Collection (PS4) on Android handhelds with Snapdragon (Adreno) GPUs.
A fork of [uncharted_pc](https://github.com/Supermedo/uncharted_pc) by SuperMedo, the native port
of the PS4 game, rebuilt for ARM64 Android.

> **Experimental, an early preview (version 0.2.8).** Tested on one device so far (Retroid Pocket G2).
> **No game files are included.** You need your own decrypted dump of the collection.
> This project is not affiliated with Sony Interactive Entertainment or Naughty Dog.

## Status

Tested on one device so far: the Retroid Pocket G2 (Snapdragon G2 Gen 2, Adreno A735, 8 GB),
at 1280x720.

- **Uncharted: Drake's Fortune**: playable, about 30-37 FPS in the places measured (GPU-bound).
- **Uncharted 2: Among Thieves**: playable, about 30 FPS in heavy places (the museum's rooftops),
  50 with the camera up (no frame limit). It runs with accurate GPU memory, as upstream does: its
  grenade explosions, vehicle damage and chapter 21's convoy need it (a few percent of the frame
  rate; the launcher's settings turn it off).
- **Uncharted 3: Drake's Deception**: playable at 25-30 FPS in the first chapter's fight. It is
  limited by the CPU: the game's job workers spin while idle, as designed for the PS4's dedicated
  cores, and leave little CPU time for the port's renderer threads.
- The app has a launcher: it finds your dump, makes the loader's files from it on the device and
  writes the settings. The APK is on the
  [Releases](https://github.com/revvv991/uncharted-android/releases) page.

## How it works

- The game's x86-64 code runs through [FEXCore](https://github.com/FEX-Emu/FEX), which translates it
  to ARM64 inside an ordinary Android app (no Linux userland, Wine or Box64).
- The rest of the port is native ARM64: the replacements for the PS4 system libraries and the
  renderer derived from [shadPS4](https://github.com/shadps4-emu/shadPS4), which translates PS4 GPU
  commands and shaders to Vulkan.
- Vulkan driver: Turnip (Mesa), loaded with libadrenotools, with a patch for the KGSL kernel driver.
  The stock Adreno driver hangs the GPU in this game.
- Most changes to the port are about performance on mobile hardware: a staged GPU command pipeline,
  batched skinning, fewer waits between the CPU and the GPU. They are marked `g2-arm64:` in the code.

## Requirements

- Android 13 or later, a Snapdragon SoC with an Adreno GPU supported by Turnip, 8 GB of RAM.
- A kernel with 4 KB memory pages (FEXCore needs them; the launcher tells when a device uses
  16 KB pages).
- Your own decrypted dump of the collection (CUSA02344, CUSA02320, CUSA02343). The resolution
  recipes (sizes below 1920x1080) fit version 1.02 of the executables; the launcher checks that
  and makes the patches from your own files.
- "All files access" for the app: the loader reads the dump from its folder by path.

## Repository layout

- The root is the port: upstream uncharted_pc with our work on top, one commit per release
  (base: uncharted_pc 0.3.9).
  The upstream README is kept as [README.uncharted_pc.md](README.uncharted_pc.md).
- [`android/`](android/): the Android part. Build scripts, the APK and its launcher, device tools,
  the patches for FEX and Mesa.

## Building

Linux x86-64 (we use WSL2 with Ubuntu 24.04), the Android NDK r30, the SDK's build-tools 35.0.1
and platform 35, a JDK:

    WORK=~/uncharted-android-work bash android/scripts/build_all.sh

The script initializes this repository's submodules if the clone has none, fetches the pinned
sources of FEX, Mesa (Turnip), libadrenotools, SDL3, FFmpeg and the renderer's other
dependencies, applies our patches (`android/scripts/port-arm64/patches/`), builds
them and this repository, and makes `WORK/apk/uncharted-android.apk`. Its header lists the tools
it needs and its options. The resolution recipes (`android/scripts/g2/recipes/`) go into the APK
as assets; they hold no bytes of the game. The steps one by one are the scripts in
`android/scripts/port-arm64/` and `android/scripts/g2/game_app/build.sh`.

## Using the app

1. Install the APK (`uncharted-android-0.2.8.apk` from
   [Releases](https://github.com/revvv991/uncharted-android/releases)) and open it. Allow access to
   all files when the launcher asks: the loader reads the dump from its folder by path.
2. **Choose folder**: the folder of your decrypted collection - with `eboot.bin` (Uncharted 1),
   `big2-ps4_Shipping.elf` and `big3-ps4_Shipping.elf`, `sce_module/` (`libc.prx`,
   `libSceFios2.prx`) and `sce_sys/param.sfo`. The launcher says what is missing.
3. **Prepare**: the launcher makes the loader's files from the game's executables (as the port's
   `scripts/prepare.py` and `link_modules.py` do on a PC), in a few seconds, into the app's
   folder. Run it again after you replace the dump.
4. **Settings**: the size the game renders at - 640x360, 960x540, 1280x720 (recommended) or the
   game's own 1920x1080 (much slower); below 1080p a few byte patches, made on the device from
   your executables, set it. The Vulkan driver: the bundled Turnip (tested, with the port's KGSL
   fixes; keep it on Adreno), the system's driver, or a driver package you add - the zips the
   Android emulators use (meta.json and the driver library), from a file or downloaded in the
   app from the GitHub releases of K11MCH1, MrPurple and StevenMXZ (the Gen8 builds are for the
   Adreno 8xx of the Snapdragon 8 Elite); they are untested with these games - on the G2 one of
   three worked. Showing each frame at once, the frame rate counter, the frame rate limit
   (30 / 60 / 120 / off; the panel in the game has it too), the on-screen gamepad (Auto: shown
   while no gamepad is connected), Uncharted 2's accurate GPU memory (on: its grenade explosions,
   vehicle damage and chapter 21's convoy need it; off is a few percent faster in busy scenes),
   extra environment variables for the port (for example
   `TU_DEBUG=sysmem` for a driver package).
5. **Play**: the collection's menu starts; Uncharted 2 and 3 are started from it (the app
   restarts itself).
6. **In the game**: swipe in from the right edge of the screen (or press L3+R3) for the settings
   panel - the upscaler (FSR 3.1 and TAA: temporal anti-aliasing at the size the game renders
   at; off: the game's own; the port scales the picture to the screen), sharpening, frame
   generation, the frame rate limit, the FPS counter, and the output size and finer options
   under Advanced; changes apply at once and are kept in the app's `bbport.ini`. A tap outside
   the panel, B or Close closes it.
7. **On a phone, without a gamepad**: the on-screen gamepad is drawn over the game (Auto, or On
   in the settings): a finger on the left half of the screen is the left stick (it appears under
   the finger), a drag on the right half turns the camera (the right stick), the face buttons
   are at the bottom right, L1/L2 and R1/R2 in the top corners, the D-pad on the left, L3/R3 at
   the bottom middle, the touch pad button and Options at the top. Several fingers at once.
   Opacity, size and the camera drag's sensitivity are in the panel.
8. **Updates**: when the app starts it asks this repository's releases for a newer version (once
   an hour at most; **Updates** in the launcher turns that off and has **Check now**) and offers
   it with its notes. **Update** downloads the APK from the release, checks its checksum and that
   it is this app signed with the same key, and Android installs it over the app, keeping the
   settings and saves; the first time Android asks to let the app install apps. A build of your
   own (signed with another key) is not offered the releases: Android would not install them
   over it.

In the game, L3+R3 opens the port's menu. The first time a place is shown its shaders are compiled
(stutters). The saves live in the app's folder, which Android deletes with the app: **Back up
saves** copies them to `Documents/uncharted-android`, **Restore saves** brings them back. For a bug
report, **Copy the log** puts the game's log (`bb-probe.log`) into the same folder, since other
apps cannot read the app's own folder on recent Android. When the game stops (no new frame for 10
seconds with the app in front), the log gets a report of where it waits: leave the game open that
long before closing it.

## Authors

revvv991, with Claude (an AI model by Anthropic) as co-author: much of the Android port's code and
documentation was written by Claude.

## Credits

- [uncharted_pc](https://github.com/Supermedo/uncharted_pc) by SuperMedo: the port this is built on.
- [bbport / bloodborne_pc](https://github.com/deadinside28/bloodborne_pc) by deadinside28: the native
  port uncharted_pc grew from.
- [uncharted_Linux](https://github.com/spacepepperoni/uncharted_Linux) by spacepepperoni and its
  contributors: fixes taken from it (the texture cache, the draw scheduler, the frame limiter,
  failures that no longer stop the game; the change list names its commits).
- [shadPS4](https://github.com/shadps4-emu/shadPS4): the renderer and the shader recompiler.
- [FEX-Emu](https://github.com/FEX-Emu/FEX): FEXCore.
- [Mesa](https://gitlab.freedesktop.org/mesa/mesa) (Turnip) and
  [libadrenotools](https://github.com/bylaws/libadrenotools).
- [SDL3](https://github.com/libsdl-org/SDL) and [FFmpeg](https://ffmpeg.org).
- [Bachata S4](https://github.com/zenithblue-oss/shadps4-arm64): the resolution recipes are made
  from the resolution patches in its patch catalog.
- [Bloodborne-Android](https://github.com/ducvd89/Bloodborne-Android): the same idea for Bloodborne.
- The components used by uncharted_pc are listed in [README.uncharted_pc.md](README.uncharted_pc.md#credits).

## License

GNU GPL v2 or later, like uncharted_pc ([LICENSE](LICENSE)). The patches for FEX and Mesa are under
the MIT license of those projects. Third-party components keep their own licenses; the list of
what the APK contains is in [android/LICENSES.md](android/LICENSES.md).
