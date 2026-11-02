# Runs on the G2

Scripted runs from Git Bash on the PC: start the game with the needed `env.txt`, input through
`BB_PAD_FILE`, measure, stop, put the owner's env back. The scripts leave a running game (the owner
playing) alone.

- `pad.sh`: start/stop, button presses, screenshots. Before a start and after a stop it restores the
  saves from `files/$SAVES_BACKUP/savedata` (the game autosaves during a run). The backup is made
  once by hand.
- `nav.sh <env> <prefix>`: from the start to gameplay through the "Continue" save (timings for U1).
- `abrun.sh <dir> <tag> [KEY=VALUE ...]`: one A/B run: FPS, GPU load, temperatures.
- `session.sh <dir> <tag> [KEY=VALUE ...]`: the same; `PROFILE=1`: a simpleperf profile
  (`PROF_EXTRA=--trace-offcpu`: time off the CPU), otherwise waits `WAIT` seconds. At the end: a
  thread dump into the game's log (`pad.sh dump`; `DUMP_AT_END=0`: without it). Do not edit
  `session.sh` and `nav.sh` while a run is going: bash keeps reading the file at the old offset.
- `pad.sh dump`: the stacks of all threads with the guest state into `bb-probe.log`, the game goes
  on (`BB_DUMP_REQUEST`, set by `pad.sh start`). For hangs: without root `debuggerd` cannot attach
  to the app's process.
- `boot.sh <secs> [KEY=VALUE ...]`: a start check: the game's threads after N seconds.
- `absum.py <log>`: FPS and stage time summary from `bb-probe.log`.
- `u2_ab.sh start|route|measure|stop`: A/B in U2 from the owner's last checkpoint (U2 through
  `loadexec.txt`, `PATCHES=` swaps the preset, saves from `SAVES_BACKUP`); see its header.
- `u3_ab.sh start|bench|fight|stop`: the same for U3. `bench`: the owner's checkpoint through
  "Continue" (a static scene makes a bench: the pub's washroom, 2026-10-23). `fight`: New Game,
  the intro skipped from the pause menu, then `fight.sh` on the device plays the pub brawl
  blind through `BB_PAD_FILE` (the first two rooms; the washroom's thug needs a timed dodge).
- `movie_pause.sh <tag> <pause secs> [menu-keys]`: a pause in U3's intro movie (New Game), presses
  in the pause menu, the wait, Continue, 70 s of watching for a hang (a thread dump then); for
  the castle movie hang of 2026-10-24, which it did not reproduce.
- `driver_test.sh <tag> zip:<path>|turnip|system [install] [KEY=VALUE ...]`: the public app with
  that Vulkan driver (a driver package from a zip on the device through the launcher's `driver_zip`
  extra, the bundled Turnip or the system driver), extra environment lines, Play, screenshots of
  the launcher and the game, the driver and frame rate lines, the log pulled; the hook's logcat
  lines say which driver file was loaded (2026-10-24).
- `background.sh <tag> home|sleep [secs]`: the app in the background in U2 (the home screen or the
  sleep button) and back; prints the lifecycle lines and port 0's audio queue (`BB_AUDIO_STATS=1`):
  how the sound 28.7 s late after 30 s on the home screen was measured (port 0078).
- `abab.sh u2|u3 <tag> [KEY=VALUE ...]`: an interleaved A/B (B A B A, A with the variables) with
  a cool-down before each run: a back-to-back series drifts with heat, so compare the pairs.
- `flick.sh <tag> [secs]`: records the screen of whatever runs (the owner playing included),
  extracts the real frames and scores them with `aba.py` (an A-B-A score per cell: a pixel that
  changes and changes back, motion subtracted) - how the jacket flicker of 2026-10-23 was seen
  in numbers; crop the reported cells from the frames and look.
- Note (2026-10-23): the collection menu starts on the last game played; a U1 route from the
  selection screen needs as many presses left as that game is from U1.

The owner's env by default: `../env-g2.txt` (another one: `OWNER_ENV=<file>`). The screen is woken
up automatically (otherwise the game hangs before the GPU starts). Screenshots and logs stay
local, not in git.

## The 2026-10-31 generation (the public app, adb over Wi-Fi, the owner's Continue saves)

Sourced by all of them: `g2.sh` - the device (`G2=<ip:port>` or `../device.txt`, git-ignored),
the package `dev.unchartedandroid`, `OUT` (`<repo>/out/g2`, git-ignored), the backup/restore of
the owner's launcher files and saves around every run, `press` through `BB_PAD_FILE`. The rules
and the pitfalls: `.claude/skills/g2-runs/SKILL.md`.

- `continue_run.sh <tag> <u1|u2|u3> <secs> [KEY=VALUE ...]`: one run from the owner's Continue
  save, the camera turning, FPS and GPU faults, screenshots, the log; the owner's state back.
  `START` = the game the owner played last (the restored saves put the menu there).
- `ab_pairs.sh <tag> <game> <pairs> <secs> '<A>' '<B>'`: interleaved A/B with cool-downs (`COOL`),
  medians and per-pair signs; 4 pairs.
- `owner_session.sh start|status|stop`: the owner plays, the GPU faults and frame rate watched.
- `focus_hold.sh start|step|value|stop`: hold an input, screenshot, the autofocus distance.
- `restore_owner.sh [stamp]`: the owner's files and settings back after a run that died.
- `queue_after.sh <marker file> <regex> <log> -- <command>`: chain series without attention.
- `symbolize.sh <addr...>`: names for `libbbgpu.so+0x...` offsets (the last test build).
- `../gprof_idle.py <log>`: the GPU idle share from `BB_GPU_PROFILE` blocks; `../cap_diff.py <a> <b>`:
  two frame-analyzer captures side by side (passes and dispatches that differ).
