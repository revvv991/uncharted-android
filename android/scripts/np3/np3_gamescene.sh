#!/usr/bin/env bash
# np3_gamescene.sh <tag> <u1|u2|u3> <secs> [KEY=VALUE ...]: a game's Continue save on the phone (the
# collection's menu stands on START, the owner's last game; the ring of three wraps), <secs> of the
# camera turning with frame stats, screenshots (spin1 at 10 s, spin2 at 20 s, run1 running
# forward), GPU faults from logcat. DRV as in np3.sh; waits until the phone is cool first; stops
# early when the battery passes HOT (44.0 C). Output: "<tag>: FPS ... | draws; hangs N; ...".
#   START  the owner's last game (u1); LOAD the seconds to the gameplay (90); TITLE (45)
here=$(cygpath -m "$(cd "$(dirname "$0")" && pwd)")
tag=$1 game=$2 secs=$3; shift 3; extras="$*"
N="bash $here/np3.sh"
OUT=${NP3_OUT:-$(cygpath -m "$(cd "$here/../../.." && pwd)")/out/np3}
HOT=${HOT:-440}
$N cool
$N start "$tag" BB_FRAME_STATS=1 "$@" > /dev/null
sleep 45
$N press cross; sleep 10          # the collection's title; the menu stands on START
pos() { case "$1" in u1) echo 0;; u2) echo 1;; u3) echo 2;; esac; }
case $(( ($(pos "$game") - $(pos "${START:-u1}") + 3) % 3 )) in
    1) $N press lx=255 0.15; sleep 6;;   # one step right
    2) $N press lx=0 0.15; sleep 6;;     # one step left (the ring wraps)
esac
$N press cross; sleep ${TITLE:-45}   # the game's title
$N press cross; sleep 8              # its menu
$N press cross; sleep 40             # Continue; the offline dialog
$N press cross; sleep 15
$N shot load > /dev/null
$N press cross; sleep ${LOAD:-90}
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
draws=$(grep -o -E '[0-9]+ draws, [0-9]+ dispatches' "$OUT/$D/bb-probe.log" | tail -1)
pf=$(grep -c 'GPU PAGE FAULT' "$OUT/$D/logcat-all.txt")
hang=$(grep -c -E 'gpu timeout ctx|GPU hang detected|GPC error|opcode error|Protected mode error' "$OUT/$D/logcat-all.txt")
lost=$(grep -c -i -E 'device lost|DEVICE_LOST' "$OUT/$D/bb-probe.log")
echo "$tag (DRV=${DRV:-turnip} $game $extras): $fps| $draws; page faults $pf; hangs $hang; device lost $lost; $temps$hot"
