#!/usr/bin/env bash
# rebuild_fex.sh: FEXCore rebuilt in ~/src/release-work with the current patches/fex-*.patch, for a
# change to the FEX patches. ~/src/dev-work links fex, fex-android and done to release-work: a test
# build's own FEX step (build_all.sh after `rm done/fex`) would replace the links with its own
# directories and mark the shared done/fex while release-work kept the old FEX, so the next release
# would ship it. Here the release-work tree is reset to FEX's revision (no download), patched and
# built; then test builds (WORK=~/src/dev-work) and releases both use it. Run it in WSL (long:
# start it from the Windows side, see the test-build skill).
set -euo pipefail
HERE=$(cd -- "$(dirname -- "$0")" && pwd)
WORK=${WORK:-$HOME/src/release-work}
export NDK=${NDK:-$HOME/android/android-ndk-r30}
[ -d "$WORK/fex/.git" ] || { echo "no FEX tree in $WORK/fex: run build_all.sh with WORK=$WORK once" >&2; exit 1; }
rm -f "$WORK/done/fex"
git -C "$WORK/fex" checkout -q -- .
if [ -n "$(git -C "$WORK/fex" status --short --ignore-submodules=all)" ]; then
    echo "$WORK/fex is not clean after the checkout:" >&2
    git -C "$WORK/fex" status --short >&2
    exit 1
fi
for p in "$HERE"/patches/fex-*.patch; do echo "   $(basename "$p")"; git -C "$WORK/fex" apply --whitespace=nowarn "$p"; done
rm -rf "$WORK/fex-android"
SRC=$WORK/fex OUT=$WORK/fex-android SMOKE= bash "$HERE/build_fex_android.sh" > "$WORK/fex.log" 2>&1 ||
    { tail -20 "$WORK/fex.log"; exit 1; }
touch "$WORK/done/fex"
echo "== fex: done ($WORK/fex-android)"
