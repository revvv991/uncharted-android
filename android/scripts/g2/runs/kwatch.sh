#!/usr/bin/env bash
# kwatch.sh <prefix> [secs]: the kernel log, the temperatures and the game's log of the G2 streamed
# to the PC for <secs> seconds (60) - <prefix>.klive, <prefix>.temps and <prefix>.glive, as the run
# scripts record them through g2.sh's kwatch_start (see there). On its own: while the owner plays,
# around an install, or to see that the streams work. Read-only on the device. Prints the hottest
# values at the end.
set -uo pipefail
. "$(dirname -- "${BASH_SOURCE[0]}")/g2.sh"
[ $# -ge 1 ] || { echo "usage: kwatch.sh <prefix> [secs]"; exit 1; }
pre=$1 secs=${2:-60}
device_here || { echo "no device"; exit 1; }
kwatch_start "$pre"
trap kwatch_stop EXIT
sleep "$secs"
kwatch_stop
echo "$(wc -l < "$pre.klive") kernel lines, $(wc -l < "$pre.temps") samples; hottest: $(kwatch_hottest "$pre")"
