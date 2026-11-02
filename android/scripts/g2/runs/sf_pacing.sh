#!/usr/bin/env bash
# sf_pacing.sh <out file> <secs>: the game's frames as SurfaceFlinger put them on the screen.
# Polls `dumpsys SurfaceFlinger --latency <the game's SurfaceView layer>` (its last 128 frames:
# desired present time, actual present time, frame ready time, in ns; the first line the refresh
# period) every 1.5 s for <secs> seconds into <out file>. ../sf_pacing.py merges the dumps and
# reads the intervals between frames on the screen in display refreshes.
set -uo pipefail
. "$(dirname -- "${BASH_SOURCE[0]}")/g2.sh"
out=$1 secs=$2

layer() {
    $ADB shell dumpsys SurfaceFlinger --list | tr -d '\r' | grep -o -m1 "SurfaceView\[$P/[^]]*\](BLAST)#[0-9]*"
}

L=$(layer)
if [ -z "$L" ]; then echo "sf_pacing: no game layer"; exit 1; fi
: > "$out"
end=$((SECONDS + secs))
while [ $SECONDS -lt $end ]; do
    echo "== $L" >> "$out"
    $ADB shell "dumpsys SurfaceFlinger --latency '$L'" | tr -d '\r' >> "$out"
    sleep 1.5
done
