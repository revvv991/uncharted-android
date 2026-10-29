# Patches

- `fex-*.patch`: FEX (`~/src/fex`, branch `g2-android` on top of FEX `f2b679f`), MIT license:
  0001: the Android build of FEXCore and Common (bionic, libc++), a FEXCore-only target; 0002: the
  guest's pause is a `wfe` (`BB_PAUSE=yield`: a yield); 0003: that target without a smoke executable.
- `mesa-*.patch`: Mesa/Turnip (`~/src/mesa`, branch `g2-kgsl` on top of Mesa 26.2.5), MIT license:
  0001: polling timestamps instead of waiting with a zero timeout (fixed the same way upstream,
  e984ef294ea); 0002: u_trace gate; 0003: diagnostics: `TU_BO_LOG=1` (BO log with addresses,
  together with `TU_DEBUG=bos`), `TU_KGSL_CTX_FLAGS` (KGSL context flags); 0004: diagnostics:
  `TU_MEMCHECK=1` (a background thread compares pipeline and shader memory with its contents 2 s
  after creation and prints changes with GPU addresses); 0005: experiment `TU_DS_NO_CROSS=1` (state
  groups do not cross a page; did not help); 0006: experiment `TU_IB_ALIGN=<bytes>`:
  `CP_SET_DRAW_STATE` and `CP_(NON_)CONTEXT_REG_BUNCH` packets in IB1/IB2 do not cross block
  boundaries; 0007: the same for `CP_LOAD_STATE6*`, 1 KB by default; 0008: experiment `TU_BR_SYNC=1`
  (BV waits for BR in sysmem passes); 0009: experiment `TU_CS_FILL_NOP=1` (new command BOs filled with
  `CP_NOP`); 0010: experiment `TU_IB2_SPLIT=<dwords>` (draw IB2s are cut at draw boundaries); 0011:
  alignment off by default again (it only moved the A735 CP faults); 0012: IB2 cut every 256 dwords
  by default (`TU_IB2_SPLIT=0` turns it off); 0013: **recovery after a lost KGSL context**
  (`TU_KGSL_RECOVER`, on by default: after a series of GPU faults the queue moves to a new context
  and the game goes on; check with `TU_KGSL_FAULT_TEST=<s>`); 0014: **the command streams that become
  IBs leave the second 4 KiB page of their BOs unused** (on by default, `TU_PAGE1_SKIP=0` turns it
  off): the A735 CP faults sat in that page.
  Experiments 0004-0011 are off by default; their variables stay for diagnostics.

The port's changes on top of upstream `b787c10` (uncharted_pc 0.3.6), in the order they were made
(in this repository they come as one commit per release). Changes in the code are marked
`g2-arm64:`.

| # | What |
|---|---|
| 0001 | the renderer (libbbgpu) builds for Android arm64 |
| 0002 | the guest<->host boundary (`guest_cpu.h`), native x86-64 backend, host on ARM64 |
| 0003 | the guest CPU backend on FEXCore (`guest_cpu_fex.cpp`), bb-probe for Android |
| 0004 | the app loader (`libmain.so`), Android window, SRT walkers under FEXCore |
| 0005 | Adreno and Android fixes, thread dumps with the guest state |
| 0006 | diagnostics of GPU hangs on Adreno |
| 0007 | more experiments with Adreno hangs |
| 0008 | our own Vulkan driver through libadrenotools (`BB_VK_DRIVER`, Turnip) |
| 0009 | host functions with 9-16 integer arguments; the crash report |
| 0010 | an SRT walker interpreter on ARM64; guest stack margin |
| 0011 | guest rdtsc without scaling; CP idle; the guest profiler (`BB_GUEST_PROF`) |
| 0012 | compute queues without draining the pipeline; their ReleaseMem in order (`BB_WAIT_STATS`) |
| 0013 | bounded MOVREL for known shaders (`BB_MOVREL_RANGE`) |
| 0014 | submits in the middle of a frame (`BB_MIDFRAME_FLUSH`) |
| 0015 | movies at a lower output size (`BB_VDEC_CONTENT`, `BB_VDEC_SIZE`) |
| 0016 | scalar copy down the pipeline (`BB_SCALAR_COPY_PIPELINE`) |
| 0017 | the host's GPU threads on chosen cores (`BB_RENDER_CPUS`) |
| 0018 | compute barrier diagnostics (`BB_CS_BARRIER_STATS`, `BB_CS_NO_BARRIER`) |
| 0019 | splitting big one-dimensional compute groups (`BB_CS_SPLIT`) |
| 0020 | FPS counter (`BB_SHOW_FPS`) |
| 0021 | fence sequence numbers, fewer stage A waits (`BB_IDLE_STATS`, `BB_WAIT_DIAG`) |
| 0022 | batched skinning (`BB_SKIN_BATCH`) |
| 0023 | stage A speedup (SRT block copies, dirty block scan) |
| 0024 | stage B: EOS deferred, `wfe` waits, no dynamic TLS and PLT |
| 0025 | GPU profiler in command stream order |
| 0026 | write overlap check in the scheduler (`BB_RECORD_CHECK`) |
| 0027 | Android pre-rotation: swapchain in the display's orientation |
| 0028 | thread dump on request (`BB_DUMP_REQUEST`) |
| 0029 | GPU profiler: idle between submits by length |
| 0030 | `sceSystemServiceLoadExec` and the port's "restart" restart the app; guest stack in dumps |
| 0031 | PS4 thread priorities below the default -> host nice (`BB_THREAD_NICE`) |
| 0032 | fuller crash reports (guest registers, memory at rbx, fiber), split atomic RMW log, `BB_THREAD_NICE` off by default |
| 0033 | guest traps (`int n`, INTO, privileged instructions) reported (`Guest fault (trap ...)`) instead of ending the guest thread silently |
| 0034 | the recompiler's subgroup size is the one pipelines run with (64): wave64 emulation for 32-lane subgroups no longer runs inside 64-lane ones (ReadLane of lanes 32-63, ballots) |
| 0035 | `Memory:` line every 30 s with `BB_FPS_LOG` (mappings, RSS, swap, available memory, buffer arenas); mapping count in crash reports |
| 0036 | buffer cache guest space 38 bits: the BDA page table 512 -> 128 MiB of device memory |
| 0037 | memory diagnostics: GPU breakdown (VMA, images, arenas), `BB_VMA_DUMP`, `BB_MEM_SMAPS`, mapping alarm, mapping kinds in crash reports |
| 0038 | nested indirect-buffer coroutines destroyed: the compute queue leaked ~32 KiB per IB (U2 crashed out of mappings after ~17 min) |
| 0039 | staging pool on Android: first-fit rings, 64 MiB kept, idle blocks and large buffers freed after ~20 s |
| 0040 | 64 MiB VMA blocks on Android (empty blocks go back to the system sooner) |
| 0041 | U2 skinning with motion vectors (cs 7d7e7206) batched like U1 skinning (`skin_mv_batch.comp`, `BB_SKIN_BATCH_MV=0` off): +4-7% in the palace |
| 0042 | the guest stack protector stop reports where: thread, the calling guest function, registers, stack, fiber |
| 0043 | FEX options from the environment (`FEX_<OPTION>=value` in env.txt, logged at start; FEX had read none) |
| 0044 | skinning batches store interleaved vertices as whole rows through shared memory: cs 8f067540 batch -19% |
| 0045 | helper threads (the Vulkan recording thread, the copy threads) resolve a fault that needs the cache owner themselves while the owner waits for them (`bbport_owner_wait.h`): the U3 chapter 1 freeze (fence labels in pages with GPU data, RecorderFences) |
| 0046 | diagnostics: `BB_REFRESH_LOG` (GPU-written images uploaded again from memory), `BB_POOL_LOG` (image lookups at one address, toggle bit 58) |
| 0047 | constant windows per shader (`BB_RC_WINDOW`; the tiled light 43b8ee5e: 7168 dwords, its lights past the fourth were read from a clamped 256-dword window) and the window clamp on the per-lane part of the offset, so Turnip merges the loads: the light dispatch 110 -> 56 ms/s of GPU time; `BB_FLAT_PEEK` (diagnosis) |
| 0048 | a sharp read through a constant window sits at its offset in the window (per-lane parts as 0), not at the window's start: the tiled light 43b8ee5e bound a null image for its lights' cookie texture, so its lights were black |
| 0049 | shader cache version 11: cached shaders with windowed reads kept the old translation (the cache key did not change) |
| 0050 | T# tables read through a constant window at index * stride (compute) bind as an image array indexed in the shader (`ImageTableIndex`): each light of the tiled light gets its own cookie (0048 gave all of them the first light's); ShaderMetaVersion 9 |
| 0051 | Android app lifecycle in presenting: no presenting in the background (an SDL event watch), a new VkSurfaceKHR for the window Android gives back, acquire timeout 100 ms, no stop on a dead surface: the game survives the screen off (the sleep button) |
| 0052 | Diagnostics: `BB_PEEK_FILE` (guest memory read on request from the running game, process_vm_readv, a reply file), the guest registers of a thread waiting in a host function in the thread dump |
| 0053 | The guest's thread core masks on Linux: logged (the first 64); `BB_AFFINITY_MAP=<host CPU of PS4 core 0>,...` applies them with sched_setaffinity, `BB_AFFINITY_THREADS=<name prefix>` limits that to those threads (off by default; U3 experiments) |
| 0054 | 381ba1d, 90e3030 | `src/game_prepare.c`: the loader's files from the game folder in C (scripts/prepare.py + link_modules.py + content_profile.py, byte for byte the same `boot-linked.bin`/`content.bin`), `libbbprepare.so` (JNI) for the app's launcher |
| 0055 | `BB_AFFINITY_THREADS=<prefix>,<prefix>,-<prefix>...`: several name prefixes and exclusions (U3's A3: every mask but the movie decoder's) |
| 0056 | A CMake warning when libadrenotools is missing on Android (the app could use only the system Vulkan driver); `build_all.sh` builds everything from source |
| 0057 | `BB_AFFINITY_MAP` entries name sets of host CPUs (`3-7`, `0+3-7`): U3's guest threads kept on the big cores (E1) |
| 0058 | `BB_NICE=<name prefix>:<nice>,...`: nice values of guest threads by name (experiments; U3's F1) |
| 0059 | Stage A's drain (GpuComm waiting for the draw recording thread) spins up to `BB_DRAIN_SPIN_US` (50 us), then sleeps until stage B ran its last packet: GpuComm 80-96% -> ~50% of a core, U3's fight 29.2 -> 34.8 FPS |
| 0060 | The texture cache frees an idle overlapping image after 2000 scheduler ticks instead of 32 (~1.5 frames with our submissions): U2's shadow pool was remade every few frames - the palace lamp's flicker (49 -> 2 dips in 40 s); `BB_TC_REMOVAL_TICKS`; the pool log names each free's caller |
| 0061 | Texture cache diagnosis: `BB_ZERO_UPLOAD_LOG=1` reports an image uploaded from guest memory whose first page is all zeros; `BB_COPY_SHADER_LOG=1` also names large scalar copies made on the GPU only (the black-face hunt in U3) |
| 0062 | The GPU thread wakes the guest's submission wait (`WaitGpuIdle`, `num_submits == 0`) only when the last submission retires, not on every one of ~100 a frame: ~100 futex calls a frame and 4300 context switches/s of the guest thread in U3's pub gone |
| 0063 | The userfaultfd tracker reads the kernel's features first and asks for the wanted ones among them (the fixed set failed with EINVAL before 6.4); the G2's GKI kernel has no userfaultfd write-protection at all, so it stays on mprotect, now saying why |
| 0064 | The in-game menu as a touch panel on Android (`gpu/shim/bbport_overlay_android.cpp`): the same settings on the right edge with finger-sized rows of buttons, English, only what applies on Android; a swipe in from the right edge, L3+R3 or Insert opens it, a tap outside, B or Close closes it; the PC window stays elsewhere |
| 0065 | The stream dedupe (0023) is on by default: U2's palace copied 1.37 GB/s of streamed constant buffers into the ring, 80% of them bytes already there; now 0.27 GB/s and the GPU 3 points less busy at the 30 FPS checkpoint, the same FPS in an interleaved A/B; `BB_STREAM_DEDUPE=0` turns it off |
| 0066 | The touch panel scrolls under a finger (SDL finger events; ImGui's own drag did not scroll over buttons) and has a Close button in its header; text 1.8x so it fits 1080p with Advanced closed, the FPS counter toggle in the header, the output size under Advanced |
| 0067 | Hot pages on by default (upstream's opt-in): a page the guest writes again after two faults stays writable and is uploaded on every use instead of faulting and mprotect-ing (TLB shootdowns on every core): U3's pub 27.5 -> 31 FPS, U2's palace unchanged; the set capped at `BB_HOT_PAGES_MAX` (4096 pages); `BB_HOT_PAGES=0` turns it off |
| 0068 | `BB_HOST_NICE=<thread name prefix>=<nice>,...`: the nice level of the port's own threads by name (e.g. `bb:VkRecorder=-10`); changed nothing in U2's palace, a knob for other devices |
| 0069 | The texture cache's 2000-tick removal limit (0060) for depth images only - the shadow pools the lamp needed; colour images back to upstream's 32 ticks (U3 held ~170 MiB more with 0060's rule on all); `BB_TC_REMOVAL_TICKS` (depth), `BB_TC_REMOVAL_TICKS_COLOR` |
| 0070 | The periodic Memory line prints the page tracker's host protections a second (`runtime_memory_protect_count`): ~1000 mprotect calls a second remain in U3's pub with hot pages on, each with TLB shootdowns on every core running the game's threads |
| 0071 | A page is hot after one fault and the set is rebuilt every 60 s (upstream: two faults, 20 s): U3's pub 30.2 -> 35 FPS on top of 0067, faults 520 -> 290/s; the cap process-wide, 8192 pages; `BB_HOT_FAULTS`, `BB_HOT_PERIOD`, `BB_HOT_PAGES_MAX` |
| 0072 | `BB_ADAPTIVE_FLUSH=<low>,<high>`: the mid-frame flush comes as soon as the GPU has at most <low> submissions left and waits (up to twice the interval) while it has at least <high>; U2's palace with 1,3: the same FPS, 15-30% fewer submissions, less GPU idle between them; off without the variable |
| 0073 | The first periodic Memory line prints no host protection rate (its interval is ~0 s) |
| 0074 | A page the GPU writes is never hot: `ChangeRegionState<GPU, true>` drops it from the hot set and protects it again, `CountWriteFaults` skips pages with the GPU bit; hot pages opt-in again (`BB_HOT_PAGES=1`) - a hot page counted as CPU-modified regardless of its GPU bit and its next binding copied guest memory over the GPU's data (the flicker in all three games on 2026-10-23); `BB_CLOBBER_CHECK=1` prints a running total every 100 reports |
| 0075 | The frame rate limit is a live setting (`fps_limit` in bbport.ini, `BB_FPS_LIMIT`, the panel's 30 / 60 / 120 / Off row; the Android panel saves its changes); the velocity-mode scene search compares the game's targets with its display buffer size instead of 1920x1080 (a 720p game was never found: FSR 3.1 / TAA had never run on the G2); `active_render_*` is the scene the upscaler is given; the log says `Upscaler: scene WxH -> output WxH`; the panel shows the game's and the output size, calls FSR 3.1 / TAA anti-aliasing at the game's size and drops the preset and live rows |
| 0076 | The video decoder prints its decode rate every 5 s and its flush and reset calls (`Videodec2: N decodes in 5.0 s ...`): a movie that stops while its audio goes on left no trace in the log |
| 0077 | An on-screen gamepad for phones (`bbport_touchpad.cpp`): a floating left stick, a camera drag, the face buttons, shoulders, triggers, D-pad, L3/R3, Options and the touch pad button drawn by ImGui; `runtime_touch_pad` merges it into every pad read; `touch_pad` 0 / 1 / auto (while no gamepad is connected), opacity, size and camera sensitivity in bbport.ini and the panel; taps held 90 ms; touches no longer reach the game as a mouse (SDL's synthesized left button was R2) |
| 0078 | No audio is queued while the Android app is in the background (`runtime_audio_background` from the window's lifecycle filter) and the first output after it drops what is still queued; a queue 250 ms over its target is dropped too (a device that stopped taking audio) - after the sleep button the sound came 20-30 s late and stayed late: the clock-paced output filled the queue while SDL paused the device, the cadence corrections drain 1/33 of a backlog per second |
| 0079 | `BB_VK_DRIVER_DIR=<dir>`: the custom Vulkan driver is loaded from that directory (the launcher's driver packages: the zips the Android emulators use, from a file or the GitHub releases of K11MCH1, MrPurple and StevenMXZ), the hook libraries stay in the APK's library directory |
| 0080 | FSR 3.1 frame generation no longer leaks a buffer object per job (`gpu/patches/fsr-vulkan/0003`: the bridge's per-frame constant buffers and descriptor pools tagged and retired, as the upscaler does; the Android build applies the fsr-vulkan patches now); the upscaler prints its FSR error code - a crash after minutes with frame generation on (issue #2) |
| 0081 | The log names the patch file applied (`Patches: ... from u1-960x540.bin`: the chosen size) and the game's display buffer size (`Game: display buffers WxH`), for bug reports |
| 0082 | A GPU library assertion prints the host call stack (module offsets, as for a GPU crash) before the port stops, so a bug report's log says where it failed (found with it: the Qualcomm driver v863.1's `VK_INCOMPLETE` from `vkEndCommandBuffer`, issue #8) |
| 0083 | The FPS counter says "ms" (upstream's showed its Russian abbreviation to every user) |
| 0084 | Movies: FFmpeg decodes with four threads (`BB_VDEC_THREADS`, 1 as before) and below 1080p a NEON area filter scales the picture by the exact ratios (3:2, 2:1, 3:1) into the NV12 frame, which is kept for the decoder's life; the decode rate line says the decoder's time by stage - the movie thread was busy 70-92% below 1080p on the G2, 9% now (issue #6: cutscenes falling behind their audio on a phone) |
| 0085 | FSR 3.1 runs with Turnip: fsr-vulkan patch 0004 lets the FFX Vulkan backend take a host-visible device-local memory type when there is no other (Turnip has none other, so the FSR 3.1 context failed at every size, -7); when the context cannot be made the FPS counter says "FSR 3.1 failed" and the panel says why (issue #13: FSR had no effect) |
| 0086 | The collection's blurs as at 1080p below it: stage A compresses the gameplay depth of field's profile (cs 8b10b4cb/1c3672c5, dwords 32..38 of buffer 0) and scales the step of the bloom's and the menus' blur (vs 4f38cbc0, buffer 1, targets with the render size's proportions) by height / 1080 in their copies in the constant ring (`BB_BLUR_SCALE=0`: off); `BB_TRACE_WORDS=<n>` for `BB_TRACE_BUFFERS` (issue #7: everything but Drake blurred while aiming in U2/U3) |
| 0087 | From upstream uncharted_pc 0.3.8 (722df5f, fd15af1): tiny render results the CPU reads (at most 4x4 pixels) are copied back to guest memory without waiting, with `BB_TINY_READBACK=1` - the launcher sets it for U2, whose aiming autofocus reads the depth under the crosshair from a 1x1 render target (issue #7: everything behind Drake blurred while aiming, at every size) |
| 0088 | The merge of upstream uncharted_pc 0.3.9 (754ed2a, 70 commits since b787c10). For Android: the job workers' sleep patch is off unless `BB_JOB_SLEEP=1` (late jobs outlived their frame memory on the G2), cpu_patch runs only on x86-64 hosts, upstream's landing gate (deferred raycaster copies) waits on our numbered fences and the compute ReleaseMem, shader cache versions 12/10/7; upstream's U2 extras (`BB_GPU_MERGE=0`, `BB_SCALAR_COPY64_*`) stay off |
| 0089 | The draw scheduler's `record_chunk` is never null: the replacement chunk is taken before the current one is handed over (from uncharted_Linux 53fba30; the `BB_GPU_MERGE=0` stop at U2's level load) |
| 0090 | The texture cache's collector frees images again when the oldest ones cannot go: GPU-written and tiled images neither use up a pass's deletions nor stay first in the LRU list (ten of them stopped every pass, and memory grew with every area); below the pressure mark an image goes after 20 s unused instead of 16 submissions (`BB_GC_IDLE_SECONDS`; from uncharted_Linux 0680e7c) |
| 0091 | GPU-written images are kept over stale guest memory: the MaybeCpuDirty check hashes the same range at marking and at refresh, and uploads of small images, targets and storage images record their mip hashes, so a CPU write in their page no longer puts old guest memory over the GPU's data (from uncharted_Linux 4f5a95a, e0baa3b) |
| 0092 | `FindDepthTarget` and `ResolveDepthOverlap` look the depth image up again after `slot_images.insert()`, which moves every image when the slot vector grows (a read of freed memory; from uncharted_Linux fddcf62, the call sites only) |
| 0093 | Four failures no longer stop the game: a pipeline the driver cannot create (its draws and dispatches skipped, an empty cache entry, not saved), a garbage PM4 header (the rest of the buffer dropped), a flip IRQ ahead of its label (20 ms for the label, then the flip), a null vertex buffer table (the draw skipped); the GPU, VRAM and driver in the log (from uncharted_Linux 45963a2, b3411eb, aa10427, 055fac6) |
| 0094 | The frame limiter flips no sooner than a period minus 1/16 after the last flip: on its fixed grid the frame after a late one came early (at 30 FPS 45 ms, then 22 ms; from uncharted_Linux d976ca3, without its precise sleep for Windows) |
| 0095 | `BB_READBACK_SMALL_BUFFERS=1` (U3's auto-exposure and black weapon, off by default) copies the small compute-written buffers back without waiting for the GPU (`ReadMemoryAsync`); `=sync` keeps the waiting copy (from uncharted_Linux c443a3b) |
| 0096 | The frame limiter's next slot is never more than a period after the flip made: movies flip at 60 Hz past a 30 FPS limit, and each of their flips moved the slot a period on, a second per second of movie; after a movie nothing could flip for as long as it had played (U1's black screens after its cutscenes, U3 stuck after the prologue's cutscene) |
| 0097 | With accurate GPU memory (`BB_GPU_MERGE=0`) a guest write next to at most 64 GPU-written bytes of its page no longer waits for the GPU: the bytes reach guest memory once written (`ReadMemoryAsync`), as U2's GDS counter slot beside a ring its CPU writes every frame needs; `BB_LATE_GPU_DATA=0`: off; the frame stats count them (from Uncharted-arm 27fbd42) |
| 0098 | With accurate GPU memory on unified memory (Turnip has no side queue) a read back copies completed GPU writes from the mapped arena memory instead of waiting for all queued work; every submission ends with a host-read barrier; U2's A/B scene: the GPU thread's GPU waits 47% -> 0%, ~10 us a read back; `BB_UMA_READBACK=0`: off; the log says which way read backs go (from Uncharted-arm 34d7e4d) |
| 0099 | Diagnosis: toggle bit 60 (`BB_TOGGLE_FILE`) answers every occlusion query "nothing visible" (the counter stops); the frame stats count the guest's counter dumps (U2: one query a frame) |
| 0100 | The shader trace reads constant blocks and buffers under a recovery point (a user data pair that was no pointer, 0x3fe000 on U2's rooftops, stopped the game); the live `trace <hash>` line of `BB_SHADER_DEBUG_FILE` traces the buffers too (64 words each) |
| 0101 | The tiled light (cs 43b8ee5e) binds its T# table with 128 entries (window 8244 dwords; the rasterizer holds `Shader::NUM_IMAGE_DESCRIPTORS` = 192 image descriptors): with 32 (0050) its lights 32 and up took light 31's cookie, and on U2's night rooftops (45-49 lights) a lamp lit the walls all around at some camera angles; ShaderBinaryVersion 13, ShaderMetaVersion 11 |
| 0102 | Diagnosis: `BB_NULL_TEXTURE_LOG=1` names each shader binding that samples a null T# once, with where its first dword comes from in the flat buffer (unknown: the translation could not place it) |
| 0103 | Fragment shaders: GCN's output clamp turns NaN into 0, as `NMin(NMax(x, 0), 1)`: SPIR-V's FClamp leaves NaN undefined, Adreno kept it and the output NaN guard drew it black (U1's statue moss: `clamp(exp2(power * log2(dot)))` with a zero gloss power); 3 more Turnip instructions in that shader, no GPU time measured; `BB_SATURATE_NAN_FS=0`: off (cached apart, `-sat0`); ShaderBinaryVersion 14 |
| 0104 | Diagnosis: the guest profiler (`BB_GUEST_PROF`) gives a host function's samples the guest's call site and each thread its own top entries; the host function trace (`BB_HLE_TRACE=<s>`, `BB_HLE_TRACE_FILE=<file>`, triggered by `<file>.trigger`) records every host call of the guest for that long (function, thread, call site, three arguments, times entered and left; at most 1M calls) for `scripts/g2/hle_trace.py` |
| 0105 | Diagnosis: the stall report: no flip from the game for `BB_STALL_REPORT_SECONDS` (10; 0: off) in the foreground puts every thread's stack into the log (at most twice a run), with the suspended fibers (registers, call chain, 64 bytes at rbx), the flip and display buffer state, the label the command processor sleeps on and, with `BB_WRITE_LOG=1`, the last GPU writes to guest memory; `BB_HLE_TRACE_RING=1` keeps the host function trace's last 1M calls from the start and writes them then (records carry the result: 64 bytes, `hle_trace.py --tail`) |
| 0106 | Diagnosis: the stall report also prints the interrupts signaled per kind, the command processor's submissions and the label each queue waits on, the recording thread's host copies, deferred submissions and deferred signals (issued and done) and the copy threads' pending tasks and callbacks: a stop with all of them settled is the game's own (#17) |
| 0107 | Diagnosis: `BB_COUNTER_LOG=<file>` (U2): stubs in the zero tail of the code segment (0x13e3000) log the job manager's decrements that leave a counter at or below 0 (0xf3e09c), its notifies (0xf29440) and its frees (0xf29230) into a ring of 64K events; the first counter at -1..-15 freezes the ring and a thread writes it to the file (`scripts/g2/counter_log.py`): issue #17's counters decremented once too often |
| 0108 | U2's mesh raycaster copies (64-dword copy shader 6a6422a9, step 2 into the record array the game reads behind a label) land without a side queue: a GPU copy in the open command buffer puts the source's GPU-written bytes into an 8 MiB host-cached ring right behind the work that wrote them, the buffer is submitted, and `bb:Landing` copies them into guest memory once it has completed; the guest part is only the bytes the GPU did not write (`BB_LANDING_RING=0`: off; upstream f32a685 needed a second queue and otherwise waited for the whole queue, -18%). The flip interrupt is raised on the draw scheduler's owner, never deferred (on the Vulkan recording thread the EOP flip's PrepareFrame waited for that thread itself: a freeze); `bb:Landing` is an owner-wait helper and runs on the render CPUs; the command thread runs a helper's request outside the owner-wait mutex; write data labels submit before waiting for a landing (`BB_LANDING_FLUSH=0`: off); the HLE's fallback waits for older landings; stall lines in the landing waits; `Landing:` stats every 10 s (issue #21) |
| 0109 | U2's freed job counters wait in a quarantine before the pool reuses them (issue #17): a job that finished after its counter was freed decremented the counter's next owner (0.2.7's movie stop, 0.2.8's crash in the particles' DrawReg job). WaitAndFree (0xf2a040) frees through FreeCounter (13 bytes at 0xf2a067 instead of its own copy), and FreeCounter's entry goes to a stub in the code segment's zero tail (0x13e3400, 66 bytes): a counter already in the ring is dropped (U2's navigation queries free theirs twice), otherwise it goes into a FIFO of 1024 and the one it pushes out is freed (`BB_COUNTER_QUARANTINE=<n>`, 0: off; off with `BB_COUNTER_LOG`). The counter trap: `BB_COUNTER_LOG_ARM=<s>` (log from s seconds on: U2's level load takes one counter to -1 every time), `BB_COUNTER_LOG_CAPTURES=<n>` (rings after the first) |

