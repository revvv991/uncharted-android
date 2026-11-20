#!/usr/bin/env bash
# build_all.sh: the public APK from source in one go. Fetches the pinned sources (FEX, Mesa,
# libadrenotools, the renderer's dependencies) into WORK, applies our patches
# (port-arm64/patches), builds them, this repository's loader and renderer, then the APK.
# Linux x86-64 (we use WSL2 with Ubuntu 24.04).
#
#   WORK=<dir> [NDK=<NDK r30>] [SDK=<Android SDK>] bash android/scripts/build_all.sh
#
# Needs the Android NDK r30, the SDK's build-tools 35.0.1 and platforms;android-35, a JDK (javac,
# keytool) and git, cmake, ninja, python3 with venv, bison, flex, pkg-config, curl, zip, strings
# (Ubuntu: apt install git cmake ninja-build python3-venv bison flex pkg-config curl zip binutils
# openjdk-21-jdk-headless).
# The fetched and built dependencies are kept (WORK/done/<step>; delete one to redo that step);
# the port and the APK are rebuilt on every run.
# The resolution recipes (scripts/g2/recipes, no bytes of the game) go into the APK as assets.
# KEYSTORE, KEYSTORE_PASS: the signing key; by default a debug key made in WORK.
# EXTRA_APK_LIBS: more libraries for the APK's lib/arm64-v8a, for test builds only (the Khronos
# validation layer for BB_VK_VALIDATION=1). Empty for a release.
# Result: WORK/apk/uncharted-android.apk.
set -euo pipefail
HERE=$(cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(cd -- "$HERE/../.." && pwd)
WORK=${WORK:?WORK=<directory for the sources and the builds>}
export NDK=${NDK:-$HOME/android/android-ndk-r30}
SDK=${SDK:-$HOME/android/sdk}
APP_ID=${APP_ID:-dev.unchartedandroid}
PATCHES=$HERE/port-arm64/patches

# The pinned sources (android_deps.sh pins the renderer's dependencies).
FEX_URL=https://github.com/FEX-Emu/FEX.git
FEX_REV=f2b679f6028ce1c38875233aecfcf5d3f8ebecec
FEX_SUBMODULES="External/fmt External/xxhash External/range-v3 External/unordered_dense External/rpmalloc
    External/jemalloc_glibc External/vixl External/zydis Source/Common/cpp-optparse"
MESA_URL=https://gitlab.freedesktop.org/mesa/mesa.git
MESA_TAG=mesa-26.2.5
MESA_PIP="meson==1.12.1 mako==1.4.3 pyyaml==6.0.3 packaging==26.3"
ADRENOTOOLS_URL=https://github.com/bylaws/libadrenotools.git
ADRENOTOOLS_REV=8fae8ce254dfc1344527e05301e43f37dea2df80

need() { command -v "$1" > /dev/null || { echo "missing: $1 (install $2)" >&2; exit 1; }; }
need git git; need cmake cmake; need ninja ninja-build; need python3 python3-venv; need bison bison
need flex flex; need pkg-config pkg-config; need curl curl; need zip zip; need javac "a JDK"
need keytool "a JDK"; need strings binutils
grep -qs 'Pkg.ReleaseName = r30' "$NDK/source.properties" ||
    { echo "no NDK r30 at $NDK (NDK=...)" >&2; exit 1; }
[ -x "$SDK/build-tools/35.0.1/aapt2" ] && [ -f "$SDK/platforms/android-35/android.jar" ] ||
    { echo "the SDK at $SDK needs build-tools;35.0.1 and platforms;android-35 (sdkmanager; SDK=...)" >&2; exit 1; }

if git -C "$ROOT" submodule status | grep -q '^-'; then
    echo "== this repository's submodules (imgui, xbyak, miniz, magic_enum, FSR, LibAtrac9)"
    git -C "$ROOT" submodule update -q --init --depth 1
fi

mkdir -p "$WORK/done"
todo() { [ ! -f "$WORK/done/$1" ]; }
done_step() { touch "$WORK/done/$1"; echo "== $1: done"; }
fetch_rev() { # <dir> <url> <commit>: that commit only, no history
    rm -rf "$1"
    git init -q "$1"
    git -C "$1" remote add origin "$2"
    git -C "$1" fetch -q --depth 1 origin "$3"
    git -C "$1" -c advice.detachedHead=false checkout -q FETCH_HEAD
}
apply_patches() { # <dir> <patch>...
    local dir=$1; shift
    for p in "$@"; do echo "   $(basename "$p")"; git -C "$dir" apply --whitespace=nowarn "$p"; done
}

if todo deps; then
    echo "== the renderer's dependencies"
    DEPS=$WORK/deps bash "$HERE/port-arm64/android_deps.sh"
    done_step deps
fi
if todo fex; then
    echo "== FEXCore"
    fetch_rev "$WORK/fex" "$FEX_URL" "$FEX_REV"
    # shellcheck disable=SC2086
    git -C "$WORK/fex" submodule update -q --init --depth 1 $FEX_SUBMODULES
    apply_patches "$WORK/fex" "$PATCHES"/fex-*.patch
    rm -rf "$WORK/fex-android"
    SRC=$WORK/fex OUT=$WORK/fex-android SMOKE= bash "$HERE/port-arm64/build_fex_android.sh" > "$WORK/fex.log" 2>&1 ||
        { tail -20 "$WORK/fex.log"; exit 1; }
    done_step fex
fi
if todo turnip; then
    echo "== Turnip"
    rm -rf "$WORK/mesa" "$WORK/turnip-android" "$WORK/mesa-venv"
    git -c advice.detachedHead=false clone -q --depth 1 --branch "$MESA_TAG" "$MESA_URL" "$WORK/mesa"
    apply_patches "$WORK/mesa" "$PATCHES"/mesa-*.patch
    python3 -m venv "$WORK/mesa-venv"
    # shellcheck disable=SC2086
    "$WORK/mesa-venv/bin/pip" install -q $MESA_PIP
    SRC=$WORK/mesa OUT=$WORK/turnip-android VENV=$WORK/mesa-venv \
        bash "$HERE/port-arm64/build_turnip_android.sh" > "$WORK/turnip.log" 2>&1 || { tail -20 "$WORK/turnip.log"; exit 1; }
    done_step turnip
fi
if todo adrenotools; then
    echo "== libadrenotools"
    fetch_rev "$WORK/libadrenotools" "$ADRENOTOOLS_URL" "$ADRENOTOOLS_REV"
    git -C "$WORK/libadrenotools" submodule update -q --init --depth 1
    rm -rf "$WORK/adrenotools-android"
    { cmake -S "$WORK/libadrenotools" -B "$WORK/adrenotools-android" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=33 -DCMAKE_BUILD_TYPE=Release &&
      cmake --build "$WORK/adrenotools-android"; } > "$WORK/adrenotools.log" 2>&1 ||
        { tail -20 "$WORK/adrenotools.log"; exit 1; }
    done_step adrenotools
fi

echo "== the loader, the renderer, the launcher's library"
DEPS=$WORK/deps OUT=$WORK/build FEX_SOURCE_DIR=$WORK/fex FEX_BUILD_DIR=$WORK/fex-android \
    ADRENOTOOLS_SOURCE_DIR=$WORK/libadrenotools ADRENOTOOLS_BUILD_DIR=$WORK/adrenotools-android \
    bash "$HERE/port-arm64/build_gpu_android.sh" bbgpu main bbprepare > "$WORK/port.log" 2>&1 ||
    { grep -E 'error|FAILED' "$WORK/port.log" | head -20; exit 1; }
grep -A2 'libadrenotools not found' "$WORK/port.log" || true

echo "== the APK"
ASSETS=$WORK/assets
rm -rf "$ASSETS"; mkdir -p "$ASSETS"
PORT=$ROOT FEX=$WORK/fex MESA=$WORK/mesa ADRENOTOOLS=$WORK/libadrenotools DEPS=$WORK/deps \
    bash "$HERE/g2/game_app/licenses.sh" "$ASSETS/licenses.txt" > /dev/null
KEYSTORE=${KEYSTORE:-$WORK/debug.keystore}
if [ ! -f "$KEYSTORE" ]; then
    keytool -genkeypair -keystore "$KEYSTORE" -storepass android -keypass android -alias androiddebugkey \
        -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Android Debug,O=Android,C=US" > /dev/null 2>&1
    KEYSTORE_PASS=pass:android
fi
HOOKS=$WORK/adrenotools-android/src/hook
PUBLIC=1 APP_ID=$APP_ID ASSETS_DIR=$ASSETS GAME_APP_DIR=$WORK/apk BUILD=$WORK/build SDK=$SDK \
    DEPS=$WORK/deps/prefix SDLJAVA=$WORK/deps/src/sdl3/android-project/app/src/main/java \
    KEYSTORE=$KEYSTORE KEYSTORE_PASS=${KEYSTORE_PASS:-pass:android} \
    EXTRA_LIBS="$WORK/turnip-android/libvulkan_freedreno.so $HOOKS/libmain_hook.so $HOOKS/libfile_redirect_hook.so
                $HOOKS/libgsl_alloc_hook.so $HOOKS/libhook_impl.so ${EXTRA_APK_LIBS:-}" \
    bash "$HERE/g2/game_app/build.sh" > "$WORK/apk.log" 2>&1 || { tail -20 "$WORK/apk.log"; exit 1; }
ls -la "$WORK/apk/uncharted-android.apk"
# What the binaries still say about this machine (should be nothing)
for lib in "$WORK"/apk/lib/arm64-v8a/*.so; do
    n=$(strings "$lib" | grep -c -e "$HOME" -e "$WORK" -e "$ROOT" || true)
    [ "$n" = 0 ] || echo "   $(basename "$lib"): $n strings with this machine's paths"
done
