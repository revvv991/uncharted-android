#!/usr/bin/env bash
# test_build.sh <tag> [string to find in libbbgpu.so] [dir for the APK]: a test build of the WSL tree
# (the test-build skill's command), its log in ~/src/dev-work/build-<tag>.log (the compiler's own in
# ~/src/dev-work/port.log); then the string looked for in the APK's libbbgpu.so (1 or more: the
# change is in), the APK copied as uncharted-android-<version>-test-<tag>.apk into the dir (default
# ~/src/dev-work/apk) and the first 16 digits of its SHA-256. Run inside WSL, started from Windows:
#   MSYS_NO_PATHCONV=1 nohup wsl -e bash /mnt/c/repos/uncharted-g2/android/scripts/g2/test_build.sh \
#       <tag> <string> > <file>.nohup 2>&1 &
# (alone in its Bash call: `a && b && nohup ... &` sends the whole chain to the background).
set -u
tag=$1
needle=${2:-}
dest=${3:-$HOME/src/dev-work/apk}
cd ~/src/uncharted-android || exit 1
WORK=~/src/dev-work VERSION_SUFFIX=-test KEYSTORE=~/android/uncharted-android-release.jks \
    KEYSTORE_PASS=file:$HOME/android/uncharted-android-release.pass \
    bash android/scripts/build_all.sh > ~/src/dev-work/build-"$tag".log 2>&1
echo "build exit $?"
grep -n -E "error:|FAILED|warning:" ~/src/dev-work/build-"$tag".log ~/src/dev-work/port.log | head -10
apk=~/src/dev-work/apk/uncharted-android.apk
[ -n "$needle" ] && echo "'$needle' in libbbgpu.so: $(unzip -p "$apk" lib/arm64-v8a/libbbgpu.so | grep -a -c "$needle")"
mkdir -p "$dest"
out="$dest/uncharted-android-$(cat android/VERSION)-test-$tag.apk"
cp "$apk" "$out" && echo "$out"
sha256sum "$out" | cut -c1-16
