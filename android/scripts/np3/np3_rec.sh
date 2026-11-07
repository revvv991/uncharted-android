#!/usr/bin/env bash
# np3_rec.sh <tag> [KEY=VALUE ...]: U1 from its Continue save on the phone, then screen recordings
# while Drake runs in a circle and the camera turns (ly=0 rx=170): rec-fsr with the upscaler as set,
# then the upscaler switched off at run time (BB_TOGGLE_FILE bit 24) and rec-off with the same
# input (NOFF=1 skips it). Waits until the phone is cool first; the game is closed at the end.
here=$(cygpath -m "$(cd "$(dirname "$0")" && pwd)")
tag=$1; shift; extras="$*"
N="bash $here/np3.sh"
OUT=${NP3_OUT:-$(cygpath -m "$(cd "$here/../../.." && pwd)")/out/np3}
G=/sdcard/Android/data/dev.unchartedandroid/files
INPUT=${INPUT:-ly=0 rx=170}
$N cool
$N sh "echo 0 > $G/toggle.txt"
$N start "$tag" BB_FRAME_STATS=1 BB_TOGGLE_FILE=$G/toggle.txt "$@" > /dev/null
sleep 45
$N press cross; sleep 10          # the collection's title
$N press cross; sleep 45          # Start Uncharted: Drake's Fortune
$N press cross; sleep 8           # U1's title
$N press cross; sleep 60          # Continue
$N shot load > /dev/null
$N press cross; sleep 50          # a dialog if any; the loading
$N shot start > /dev/null
D=$(cat "$OUT/current")
rec() {
    $N pad "$INPUT"; sleep 2
    $N sh "screenrecord --time-limit ${SECS:-8} --bit-rate 24000000 /sdcard/Download/np3-rec.mp4"
    $N shot "$1-after" > /dev/null
    $N pad ''
    MSYS_NO_PATHCONV=1 adb -s "$(cat "$OUT/device.txt")" pull /sdcard/Download/np3-rec.mp4 "$OUT/$D/$1.mp4" > /dev/null
    $N sh "rm /sdcard/Download/np3-rec.mp4"
    echo "$1: $(stat -c %s "$OUT/$D/$1.mp4" 2>/dev/null) bytes"
}
rec rec-fsr
if [ -z "$NOFF" ]; then
    $N sh "echo 16777216 > $G/toggle.txt"; sleep 2
    rec rec-off
    $N sh "echo 0 > $G/toggle.txt"
fi
temps=$($N temps)
$N pull > /dev/null
$N stop
hang=$(grep -c -E 'gpu timeout ctx|GPU hang detected|GPC error|opcode error' "$OUT/$D/logcat-all.txt")
echo "$tag ($extras): hangs $hang; $temps; $(grep -E 'Upscaler: scene|Object motion' "$OUT/$D/bb-probe.log" | sort -u | tr '\n' ';')"
