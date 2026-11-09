# The phone harness (Nothing Phone 3)

Scripts for runs on the owner's Nothing Phone 3 (Adreno 825, Android 17) from Git Bash on the PC.
Outputs, screenshots and the phone's adb address (`device.txt`) go to `out/np3/` (git-ignored).

- `np3.sh`: the device (wireless debugging: the address in `out/np3/device.txt`; when its port
  moves, the same IP is found again over mDNS; else the first USB device) and the commands:
  `start <tag> [KEY=VALUE ...]` (the game through the launcher; `DRV=turnip|system|pkg:<folder>`,
  `INI="key=value ..."` for bbport.ini, `PAD=0` hides the touch pad), `shot`, `press`, `pad`,
  `pull`, `state`, `stop`, `sh`, `temps` (battery, hottest CPU and GPU zones, sys-therm),
  `cool` (closes the game and waits for the battery at 37 C, CPU 48 C; `out/np3/pause` holds
  it), `backup` / `restore [stamp]` (the owner's launcher files and saves), `install <zip>`
  (a driver package).
- `np3_scene.sh <tag> <secs> [KEY=VALUE ...]`: U1 from its Continue save, the camera turning,
  screenshots, frame rates, GPU faults from logcat.
- `np3_boatscene.sh`: the same from a saved checkpoint (`BOAT=user-boat2` on the phone: the middle
  of chapter 1's boat fight, where the A825's hangs live; the copy overwrites the saves, adb cannot
  delete the app's own files).
- `np3_gamescene.sh <tag> <u1|u2|u3> <secs> [KEY=VALUE ...]`: any game's Continue save (the
  collection's menu stands on `START`, the owner's last game; the ring of three wraps), the camera
  turning, frame stats, screenshots, GPU faults; waits for a cool phone, stops at `HOT` (44 C).
- `np3_live.sh <tag> <secs> <steps> [KEY=VALUE ...]`: like `np3_boatscene.sh` with
  `BB_SHADER_DEBUG_FILE`; the steps file's lines `<seconds> <line>` ("trace <hash>",
  "skip <hash>,...", "-") are written to it during the scene.
- `np3_rec.sh <tag> [KEY=VALUE ...]`: screen recordings while Drake runs, FSR on and then off at
  run time (`BB_TOGGLE_FILE` bit 24).
- `np3_frames.sh` (WSL, the ffmpeg of `~/venv-ff`): a contact sheet of frames from a recording.
- `dumps.py <bb-probe.log> [name regex]`: the `DUMP:` sections (`BB_DUMP_REQUEST`, below) per
  thread - the name, the guest's host call, the host frames as library+offset; `offsets.txt` for
  `llvm-symbolizer`.
- `atrace_timeline.py <atrace.txt>`: a 50 ms timeline of wake-ups of the KGSL threads
  (`gmu_f2h`, `kgsl_hwsched`, `kgsl-events`) and the port's GPU-side threads from
  `adb shell atrace -b 32000 -t 4 gfx sched freq`, who woke whom, the userspace trace marks.

Diagnostics without root, with the owner's game running on (2026-11-10, findings "The Nothing
Phone 3 on main"): `adb shell touch <files>/dump-request` makes the port dump every thread's stack
and the fibers into `bb-probe.log` within 2 s (`BB_DUMP_REQUEST`, the launcher's default); the
offsets symbolize as ELF virtual addresses - the file offset plus the text segment's
`p_vaddr - p_offset` (0x4000 in our builds; `llvm-readelf -l`) - against the unstripped libraries of
the same build (`~/src/dev-work/build/libbbgpu*.so`, `libmain.so`, the Turnip build tree). `atrace`
shows the GPU's side through the kernel threads (a GMU interrupt wakes `gmu_f2h` -> `kgsl_hwsched`
-> `kgsl-events` -> the waiting threads); KGSL's own tracepoints and `/sys/kernel/gpu` are closed to
`traced_probes` and the shell on this build.

Rules: the owner's game is not closed without their word; `backup` before runs that load a save
and `restore` after (it puts back their bbport.ini too: check `object_motion`, which the launcher's
one-time reset already used); the phone has no fan: `cool` before every run.
