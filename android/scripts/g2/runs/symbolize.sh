#!/usr/bin/env bash
# symbolize.sh <addr> [addr ...]: the functions behind libbbgpu.so offsets printed by the port's
# diagnostics (BB_SLOW_PACKETS "task +0x86aed8", BB_RECORDER_TRACE "libbbgpu.so+0x96859c",
# backtraces), through the NDK's llvm-symbolizer in WSL on the unstripped library of the LAST
# test build (~/src/dev-work/build/libbbgpu.so). The offsets are only valid for the build that
# printed them: symbolize before building again, or rebuild that commit first.
#   LIB=<path in WSL> another library (the release work: ~/src/release-work/build/libbbgpu.so)
set -uo pipefail
[ $# -ge 1 ] || { echo "usage: symbolize.sh <addr> [addr ...]"; exit 1; }
lib=${LIB:-'$HOME/src/dev-work/build/libbbgpu.so'} # expanded in WSL, not by Git Bash
wsl -e bash -lc "ls -la $lib | cut -c25-; ~/android/android-ndk-r30/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-symbolizer --obj=$lib --functions=linkage --inlining=false $* | sed 's#/home/[a-z0-9_-]*/src/##' | paste - - -"
