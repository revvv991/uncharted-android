#!/usr/bin/env bash
# np3_boatscene.sh <tag> <secs> (np3_scene.sh from the user-boat saves)
# np3_scene.sh <tag> <secs> [KEY=VALUE ...]: U1 from its Continue save on the phone (the collection's
# menu stands on U1), <secs> of the camera turning with frame stats, screenshots while it turns
# (spin1 at 10 s, spin2 at 20 s) and while running forward (run1), GPU faults from logcat.
# DRV as in np3.sh (turnip, system, pkg:<folder>). Waits until the phone is cool first (np3.sh
# cool); stops the load early when the battery passes HOT (440 = 44.0 C).
# Output line: "<tag>: FPS ... | faults N".
here=$(cygpath -m "$(cd "$(dirname "$0")" && pwd)")
tag=$1 secs=$2; shift 2; extras="$*"
N="bash $here/np3.sh"
OUT=${NP3_OUT:-$(cygpath -m "$(cd "$here/../../.." && pwd)")/out/np3}
HOT=${HOT:-440}
$N cool
# the boat: every run from the same checkpoint (BOAT: user-boat2, the middle of chapter 1's fight; files are overwritten: adb cannot delete the app's own)
$N sh "cd /sdcard/Android/data/dev.unchartedandroid/files && cp -a ${BOAT:-user-boat2}/. user/ 2>/dev/null; diff -r ${BOAT:-user-boat2} user > /dev/null && echo boat-checkpoint ${BOAT:-user-boat2} || echo BOAT-RESTORE-FAILED"
$N start "$tag" BB_FRAME_STATS=1 "$@" > /dev/null
sleep 45
$N press cross; sleep 10          # the collection's title
$N press cross; sleep 45          # Start Uncharted: Drake's Fortune
$N press cross; sleep 8           # U1's title
$N press cross; sleep 60          # Continue
$N shot load > /dev/null
$N press cross; sleep 50          # a dialog if any; the loading
$N shot start > /dev/null
D=$(cat "$OUT/current")
f0=$($N state 100000 | grep -c 'Frame rate')
$N pad rx=200
t=0 hot=""
while [ $t -lt "$secs" ]; do
    sleep 5; t=$((t + 5))
    [ $t -eq 10 ] && $N shot spin1 > /dev/null
    [ $t -eq 20 ] && $N shot spin2 > /dev/null
    set -- $($N temps)
    [ "${2:-0}" -ge "$HOT" ] && { hot=" (stopped at $t s: bat $2)"; break; }
done
$N pad ''
$N shot end > /dev/null
if [ -z "$hot" ]; then
    $N pad ly=0; sleep 4
    $N shot run1 > /dev/null
    sleep 2; $N pad ''
fi
temps=$($N temps)
$N pull > /dev/null
fps=$(grep 'Frame rate' "$OUT/$D/bb-probe.log" | tail -n +$((f0 + 1)) | sed -E 's/Frame rate: ([0-9.]+) FPS.*/\1/' | tr '\n' ' ')
draws=$(grep -o '[0-9]* draws/frame' "$OUT/$D/bb-probe.log" | tail -1)
pf=$(grep -c 'GPU PAGE FAULT' "$OUT/$D/logcat-all.txt")
hang=$(grep -c -E 'gpu timeout ctx|GPU hang detected|GPC error|opcode error' "$OUT/$D/logcat-all.txt")
lost=$(grep -c -i -E 'device lost|DEVICE_LOST' "$OUT/$D/bb-probe.log")
echo "$tag (DRV=${DRV:-turnip} $extras): $fps| $draws; page faults $pf; hangs $hang; device lost $lost; $temps$hot"
