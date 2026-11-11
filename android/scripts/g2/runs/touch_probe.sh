#!/usr/bin/env bash
# touch_probe.sh <label> <x> <y> <dx> <dy> [secs]: while a focus_hold.sh game runs with TOUCH=1 (the
# on-screen gamepad on), put a finger down at x,y (screen pixels, landscape), slide it by dx,dy in
# four steps (`input motionevent`, ~0.3 s each), hold it for secs (2) under a screen recording,
# lift it; pull the recording, tile its frames (2 a second) into <label>-tile.png in
# out/g2/hold/probe and print the overlay's stick bytes (EXTRA="BB_TOUCH_PAD_LOG=1" on the hold).
# The left half of the screen away from the D-pad is the stick: 450,850 on the G2. The stick's
# ring is 0.11 of the screen height times the pad's Size (the owner's 1.58: 188 px).
set -uo pipefail
RUNS=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
. "$RUNS/g2.sh"
label=$1; x=$2; y=$3; dx=$4; dy=$5; secs=${6:-2}
D=$OUT/hold/probe; mkdir -p "$D"
moves=""
for i in 1 2 3 4; do
    moves="$moves input motionevent MOVE $((x + dx * i / 4)) $((y + dy * i / 4));"
done
$ADB shell "rm -f /sdcard/Download/probe.mp4; screenrecord --time-limit $((secs + 3)) --size 960x540 --bit-rate 6000000 /sdcard/Download/probe.mp4 & sleep 0.6; input motionevent DOWN $x $y; $moves sleep $secs; input motionevent UP $((x + dx)) $((y + dy)); wait"
$ADB pull /sdcard/Download/probe.mp4 "$(cygpath -w "$D/$label.mp4")" > /dev/null
$ADB shell rm -f /sdcard/Download/probe.mp4
. "$RUNS/tile.sh"
tile "$D/$label.mp4" "$D/$label-tile.png"
$ADB shell "grep -a 'Touch pad:' $G/bb-probe.log | tail -6" | tr -d '\r'
echo "touch $label at $x,$y by $dx,$dy ${secs}s -> $D/$label-tile.png"
