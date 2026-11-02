#!/usr/bin/env bash
# ab_pairs.sh <tag> <u1|u2|u3> <pairs> <secs> '<A extras>' '<B extras>': an interleaved A/B over
# continue_run.sh - A, B, A, B, ... <pairs> pairs from the owner's Continue save, the limit off,
# each run <secs> s of the camera turning, COOL seconds idle after every run (60); the means of
# every run, then the median, min and max of each side and the per-pair differences (B - A).
# Thermal drift cancels over the pairs; two configurations differ when every pair points the same
# way. Extras are KEY=VALUE lists in quotes ('' = the defaults). Output: $OUT/ab/<tag>.txt.
# Lessons of 2026-10-31: 4 pairs, not 3 (one thermal outlier flips a 3-pair verdict); the device on
# the charger and not above ~70 C before the series; START = the owner's last game (g2.sh); check
# the scene of the first run (<tag>-A1-end.png) before trusting the series.
# SF_PACING=1 (continue_run.sh) with a LIMIT: each run's share of frames off the limit's cadence
# on the screen too, with its per-pair differences.
set -uo pipefail
. "$(dirname -- "${BASH_SOURCE[0]}")/g2.sh"
tag=$1 game=$2 pairs=$3 secs=$4 a_extra=${5:-} b_extra=${6:-}
AB=$OUT/ab; mkdir -p "$AB"; LOG=$AB/$tag.txt
hot=$($ADB shell 'cat /sys/class/thermal/thermal_zone*/temp' | tr -d '\r' | sort -n | tail -1)
echo "== $tag: $game, $pairs pairs of $secs s, COOL ${COOL:-60}; A=[$a_extra] B=[$b_extra]; hottest zone $hot; $(date +%H:%M)" | tee -a "$LOG"
declare -a A B OA OB
for ((i = 1; i <= pairs; i++)); do
    for side in A B; do
        if [ $side = A ]; then extra=$a_extra; else extra=$b_extra; fi
        line=$(LIMIT=${LIMIT:-0} bash "$RUNS/continue_run.sh" "$tag-$side$i" "$game" "$secs" $extra 2>&1 | grep -E "^$tag-$side$i \(" | tail -1)
        mean=$(echo "$line" | sed -n -E 's/.*mean ([0-9.]+) over.*/\1/p'); faults=$(echo "$line" | sed -n -E 's/.*GPU faults ([0-9]+).*/\1/p')
        off=$(echo "$line" | sed -n -E 's/.*off the limit ([0-9.]+)%.*/\1/p')
        echo "$side$i: ${mean:-?} FPS, faults ${faults:-?}${off:+, off the cadence $off%}  ($(date +%H:%M))" | tee -a "$LOG"
        if [ $side = A ]; then A+=("${mean:-nan}"); OA+=("${off:-nan}"); else B+=("${mean:-nan}"); OB+=("${off:-nan}"); fi
        sleep ${COOL:-60}
    done
done
python -I - "${A[*]}" "${B[*]}" "${OA[*]}" "${OB[*]}" <<'EOF' | tee -a "$LOG"
import math, statistics, sys
def summary(name, a, b):
    def s(v):
        v = [x for x in v if not math.isnan(x)]
        return 'median %.1f (min %.1f, max %.1f)' % (statistics.median(v), min(v), max(v)) if v else '-'
    print(name + 'A:', s(a)); print(name + 'B:', s(b))
    pairs = [y - x for x, y in zip(a, b) if not (math.isnan(x) or math.isnan(y))]
    if pairs:
        print(name + 'B - A per pair:', ' '.join('%+.1f' % d for d in pairs), '| median %+.1f' % statistics.median(pairs),
              '| same sign:', 'yes' if all(d > 0 for d in pairs) or all(d < 0 for d in pairs) else 'no')
v = [[float(x) for x in arg.split() if x] for arg in sys.argv[1:5]]
summary('', v[0], v[1])
if any(not math.isnan(x) for x in v[2] + v[3]):
    summary('off the cadence, % - ', v[2], v[3])
EOF
echo "AB-DONE $(date +%H:%M)" | tee -a "$LOG"
