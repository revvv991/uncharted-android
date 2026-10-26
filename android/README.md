# android/

The Android part of uncharted-android. The port's code is at the repository root (`gpu/`, `src/`,
the port's `scripts/`).

| Path | What |
|---|---|
| `scripts/build_all.sh` | The public APK from source in one go (pinned sources, our patches) |
| `scripts/publish/` | The public repository's release commit, made locally (nothing is pushed) |
| `scripts/port-arm64/` | The Android build: dependencies, FEXCore, Turnip, loader and renderer |
| `scripts/port-arm64/patches/` | FEX and Mesa patches; the list of the port's changes 0001-0096 |
| `scripts/g2/game_app/` | The APK (no Gradle): build, install, run |
| `scripts/g2/` | G2 tools: resolution recipes and patches, profiles, temperatures, data collection |
| `scripts/g2/runs/` | Scripted runs and A/B tests |
| `scripts/inventory/`, `lift/`, `rt/`, `remill/`, `pilot/`, `tools/ps4lift/` | Static recompilation research (AOT; not in the build) |

Paths in the scripts of this folder (`scripts/...`) are relative to `android/`. Some comments refer
to `docs/` and `notes/`: the development notes, which are not published.
