#!/usr/bin/env bash
# chapter_sweep.sh <tag> <u1|u2|u3> <first> <last> <secs> [KEY=VALUE ...]: chapter_run.sh for the
# chapters <first>..<last> of a game, one process a chapter, COOL seconds (20) between them; the
# runs are <tag>-<game>-<NN>. For the chapters no save of ours reaches: SAVES=<a finished game's
# save> (see chapter_run.sh; START as there). Stops when a run is refused (the owner's game
# running, the battery, the screen) or lost (adb gone: RUN-LOST) - the next start of the same
# command with <first> moved on goes on. Prints the runs' summary lines, then SWEEP-DONE or
# SWEEP-STOPPED.
# The count of chapters: U1 22, U2 26, U3 22.
set -uo pipefail
RUNS=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
[ $# -ge 5 ] || { echo "usage: chapter_sweep.sh <tag> <u1|u2|u3> <first> <last> <secs> [KEY=VALUE ...]"; exit 1; }
tag=$1 game=$2 first=$3 last=$4 secs=$5; shift 5
c=$first
while [ "$c" -le "$last" ]; do
    out=$(bash "$RUNS/chapter_run.sh" "$tag-$game-$(printf %02d "$c")" "$game" "$c" "$secs" "$@" 2>&1)
    printf '%s\n' "$out" | grep -a -E "^$tag-$game-|DIFFERS" | cut -c1-600
    if ! printf '%s\n' "$out" | grep -q '^RUN-DONE'; then echo "SWEEP-STOPPED at chapter $c"; exit 1; fi
    c=$((c + 1))
    sleep "${COOL:-20}"
done
echo SWEEP-DONE
