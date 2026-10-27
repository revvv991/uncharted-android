#!/usr/bin/env bash
# The uncharted_pc APK for the G2 without Gradle: SDL3 Java (SDLActivity) + GameActivity + native libraries
# (libmain.so: the loader with FEXCore, libbbgpu.so: the renderer, SDL3, FFmpeg, libc++_shared).
# Run in WSL: bash build.sh -> ~/src/game_app/uncharted-g2.apk. Install and run: run.sh.
set -euo pipefail
HERE=$(cd -- "$(dirname -- "$0")" && pwd)
NDK=${NDK:-$HOME/android/android-ndk-r30}
SDK=${SDK:-$HOME/android/sdk}
BT=$SDK/build-tools/35.0.1
JAR=$SDK/platforms/android-35/android.jar
DEPS=${DEPS:-$HOME/src/android-deps/prefix}
BUILD=${BUILD:-$HOME/src/uncharted-android-build}
SDLJAVA=${SDLJAVA:-$HOME/src/android-deps/src/sdl3/android-project/app/src/main/java}
W=${GAME_APP_DIR:-$HOME/src/game_app}
# PUBLIC=1: the public app - the launcher first (AndroidManifest-public.xml), the package APP_ID,
# the assets from ASSETS_DIR (licenses.txt, licenses.sh). Otherwise the G2 development app as
# before. Both carry the resolution recipes (scripts/g2/recipes, made by recipe_from_preset.py:
# no bytes of the game) as assets. KEYSTORE/KEYSTORE_PASS: the signing key (default: a debug key).
PUBLIC=${PUBLIC:-0}
APP_ID=${APP_ID:-dev.unchartedandroid}
ASSETS_DIR=${ASSETS_DIR:-${PRESETS_DIR:-}}
KEYSTORE=${KEYSTORE:-$HOME/android/debug.keystore}
KEYSTORE_PASS=${KEYSTORE_PASS:-pass:android}
MANIFEST=$HERE/AndroidManifest.xml
# The version: android/VERSION (major.minor.patch); the code is major*10000 + minor*100 + patch.
# VERSION_SUFFIX (test builds, e.g. -test) goes into the name only: the launcher's updates take
# such a build for older than the release of the same number.
VERSION=$(tr -d '[:space:]' < "$HERE/../../../VERSION")
V1=${VERSION%%.*} V3=${VERSION##*.} V2=${VERSION#*.}
V2=${V2%.*}
LINK_EXTRA=(--version-name "$VERSION${VERSION_SUFFIX:-}" --version-code $((V1 * 10000 + V2 * 100 + V3)) --replace-version)
if [ "$PUBLIC" = 1 ]; then
    MANIFEST=$HERE/AndroidManifest-public.xml
    LINK_EXTRA+=(--rename-manifest-package "$APP_ID")
fi
rm -rf "$W"; mkdir -p "$W/classes" "$W/lib/arm64-v8a" "$W/assets"
cp -r "$HERE/../recipes" "$W/assets/"
if [ -n "$ASSETS_DIR" ]; then cp -r "$ASSETS_DIR"/. "$W/assets/"; fi
LINK_EXTRA+=(-A "$W/assets")
cd "$W"
javac -nowarn -source 11 -target 11 -classpath "$JAR" -d classes \
    $(find "$SDLJAVA" -name '*.java') "$HERE/GameActivity.java" "$HERE/Launcher.java" "$HERE/Updater.java" 2>&1 | grep -v "^warning\|^Note:" || true
"$BT/d8" --release --min-api 33 --lib "$JAR" --output . $(find classes -name '*.class')
"$BT/aapt2" link -o base.apk -I "$JAR" --manifest "$MANIFEST" \
    --min-sdk-version 33 --target-sdk-version 35 "${LINK_EXTRA[@]}"
STRIP=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip
for lib in "$BUILD/libmain.so" "$BUILD/libbbgpu.so" "$BUILD/libbbprepare.so" "$DEPS"/lib/{libSDL3,libavutil,libswresample,libswscale,libavcodec,libavformat}.so \
           "$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so"; do
    "$STRIP" -o "lib/arm64-v8a/$(basename "$lib")" "$lib"
done
# Diagnostics: EXTRA_LIBS=".../libVkLayer_khronos_validation.so" (the layer is looked up in the app's lib/;
# enabled by BB_VK_VALIDATION=1).
# Stripped as the others: the NDK's CMake toolchain builds with -g (debug info with the build's paths).
for extra in ${EXTRA_LIBS:-}; do
    "$STRIP" -o "lib/arm64-v8a/$(basename "$extra")" "$extra"
done
zip -q base.apk classes.dex
zip -q -0 -r base.apk lib
"$BT/zipalign" -f -P 16 4 base.apk aligned.apk
APK=uncharted-g2.apk
[ "$PUBLIC" = 1 ] && APK=uncharted-android.apk
"$BT/apksigner" sign --ks "$KEYSTORE" --ks-pass "$KEYSTORE_PASS" --out "$APK" aligned.apk
ls -la "$APK"
