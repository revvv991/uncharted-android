#!/usr/bin/env bash
# queue_after.sh <marker file> <marker regex> <log> -- <command...>: waits until a line matching
# the regex appears in the marker file (another series' log ending with AB-DONE, QUEUE-DONE ...),
# then runs the command when the game is not running and the battery allows, appending its output
# to <log> and a final "QUEUE-DONE <time>" line (or QUEUE-ABORT with the reason). Chains device
# work without attention: start it with nohup from Git Bash and watch <log> with a Monitor.
#   WAIT_MAX  seconds to wait for the marker (10800)
#   PAUSE     seconds to idle before the command, the device cooling (120)
# Example: nohup bash queue_after.sh out/g2/ab/first.txt '^AB-DONE' out/g2/ab/second.txt -- \
#          env COOL=60 bash ab_pairs.sh second u3 4 90 '' 'BB_X=1' > /dev/null 2>&1 &
set -uo pipefail
. "$(dirname -- "${BASH_SOURCE[0]}")/g2.sh"
marker_file=$1 marker=$2 log=$3; shift 3
[ "${1:-}" = -- ] && shift
mkdir -p "$(dirname -- "$log")"
echo "== queue: waiting for '$marker' in $marker_file $(date +%H:%M)" | tee -a "$log"
for ((i = 0; i < ${WAIT_MAX:-10800} / 10; i++)); do grep -q -E "$marker" "$marker_file" 2>/dev/null && break; sleep 10; done
grep -q -E "$marker" "$marker_file" 2>/dev/null || { echo "QUEUE-ABORT: no '$marker' within ${WAIT_MAX:-10800} s $(date +%H:%M)" | tee -a "$log"; exit 1; }
if game_running; then echo "QUEUE-ABORT: the game is running $(date +%H:%M)" | tee -a "$log"; exit 1; fi
if ! battery_ok; then echo "QUEUE-ABORT: the battery is low and not charging $(date +%H:%M)" | tee -a "$log"; exit 1; fi
sleep ${PAUSE:-120}
echo "== queue: starting $* $(date +%H:%M)" | tee -a "$log"
"$@" >> "$log" 2>&1
echo "QUEUE-DONE $(date +%H:%M)" | tee -a "$log"
