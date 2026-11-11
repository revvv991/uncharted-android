#!/usr/bin/env bash
# stick_probe.sh <label> '<pad tokens>' [secs]: while a focus_hold.sh game runs, hold the pad tokens
# (pad.txt, BB_PAD_FILE: ly=26, lx=0 cross, ...) for secs (2) under a screen recording, release,
# pull the recording and tile its frames (2 a second) into <label>-tile.png in out/g2/hold/probe.
# RELOAD=u2 first reloads U2's last checkpoint from its pause menu (Options, Down x4 to "reload
# the save", Cross, Left to Yes, Cross; the menu opens on its first item), so every probe starts
# at the same place. The walk/run threshold of issue #34 was measured this way (2026-11-10): the
# distance covered in the same time and the pose tell a run from a walk; the tiles of several
# probes side by side, read at the same moments.
set -uo pipefail
RUNS=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
. "$RUNS/g2.sh"
label=$1; tokens=$2; secs=${3:-2}
D=$OUT/hold/probe; mkdir -p "$D"
if [ "${RELOAD:-}" = u2 ]; then
    press options 0.3; sleep 1.5
    for i in 1 2 3 4; do press down 0.2; sleep 0.6; done
    press cross 0.2; sleep 1.5
    press left 0.2; sleep 0.6
    press cross 0.2; sleep 30
fi
$ADB shell "rm -f /sdcard/Download/probe.mp4; screenrecord --time-limit $((secs + 1)) --size 960x540 --bit-rate 6000000 /sdcard/Download/probe.mp4 & sleep 0.6; echo '$tokens' > $G/pad.txt; sleep $secs; echo > $G/pad.txt; sleep 2; wait"
$ADB pull /sdcard/Download/probe.mp4 "$(cygpath -w "$D/$label.mp4")" > /dev/null
$ADB shell rm -f /sdcard/Download/probe.mp4
. "$RUNS/tile.sh"
tile "$D/$label.mp4" "$D/$label-tile.png"
$ADB shell "grep -a 'pad file' $G/bb-probe.log | tail -2" | tr -d '\r'
echo "probe $label [$tokens] ${secs}s -> $D/$label-tile.png"
