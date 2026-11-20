#!/usr/bin/env bash
# build_pairs.sh <tag> <u1|u2|u3> <pairs> <secs> <apk A> <apk B> [KEY=VALUE ...]: two builds of the
# app turn by turn on one scene from the owner's Continue save, for a crash or a hang that comes
# once in many runs (2026-11-17: 0.3.4's code against 0.3.5's on U3's roofs, 20 pairs: 1 and 2
# failures - no difference; the cause was then caught by a counter of its own, port 0196). ABBA (A
# B, B A, A B ...): one install a pair, and each build has as many runs right after an install as
# not (the first run of a game after an install compiles FEX's code cache). Both APKs carry the
# release key and a `-test` versionName, so each installs over the other and keeps the app's data
# (the owner's settings, saves and shader store). Before each run charge_wait (g2.sh: an hour at
# most before the first, 15 minutes after) and continue_run.sh's cool_wait; the extras go to every
# run. Per run: continue_run.sh's line, then `<tag>-<A|B><n>: ...` - the failure (a fault line, a
# STOP, or a hang: `STALL: no frame` in the log while the game lived), the compute queue notes, the
# late GPU data lines, the battery. A skipped run is tried once more as `<tag>-<X><n>r`. Stops on a
# lost device, the owner's game, a failed install, three skipped runs in a row. At the end B stays
# installed; with REF=<stamp> (a backup pair from before the series) the owner's user/ is compared
# with it. Output: $OUT/pairs/<tag>.txt; count it with ../failure_tally.py "$OUT/runs" <tag>.
# Start it with nohup from Git Bash; for hours of it also start a waiter on its last line
# (`until grep -q -E '^PAIRS-(DONE|STOPPED)' <file>; do sleep 30; done`, run in the background):
# a Monitor's expiry may not wake the session.
set -uo pipefail
. "$(dirname -- "${BASH_SOURCE[0]}")/g2.sh"
[ $# -ge 6 ] || { echo "usage: build_pairs.sh <tag> <u1|u2|u3> <pairs> <secs> <apk A> <apk B> [KEY=VALUE ...]"; exit 1; }
tag=$1 game=$2 pairs=$3 secs=$4 apk_a=$5 apk_b=$6
shift 6
extra="$*"
[ -f "$apk_a" ] && [ -f "$apk_b" ] || { echo "no such APK: $apk_a or $apk_b"; exit 1; }
PAIRS=$OUT/pairs
mkdir -p "$PAIRS"
LOG=$PAIRS/$tag.txt
RUNDIR=$OUT/runs
cur=
installed=no
skips=0
first_wait=3600
say() { echo "$*" | tee -a "$LOG"; }
stop() { say "PAIRS-STOPPED $(date +%H:%M): $*"; exit 2; }
put() {         # put A|B: that APK installed when the other one is; the game not running
    local apk=$apk_a i
    [ "$1" = B ] && apk=$apk_b
    [ "$cur" = "$1" ] && return 0
    game_running && stop "the game is running (before the install of $1)"
    for i in 1 2; do
        if $ADB install -r "$(cygpath -w "$apk")" 2>&1 < /dev/null | tail -n 1 | grep -q Success; then
            cur=$1
            installed=yes
            return 0
        fi
        sleep 20
    done
    stop "the install of $1 failed twice"
}
one() {         # one <run tag>: a run, its line and notes; 1 when it was skipped
    local run=$1 out L waited failure cq lg bat
    waited=$(charge_wait "$first_wait")
    first_wait=900
    [ -n "$waited" ] && say "$run: $waited"
    out=$(bash "$RUNS/continue_run.sh" "$run" "$game" "$secs" $extra 2>&1 < /dev/null)
    # the 5 s samples are in the runs' summary; without them the counts at the end fit the line
    printf '%s\n' "$out" | grep -a -E "^$run |DIFFERS|WRONG" | sed -E 's/\): [0-9. ]+\|/): ... |/' \
        | cut -c1-480 | tee -a "$LOG"
    if printf '%s\n' "$out" | grep -q -E '^RUN-DONE'; then
        skips=0
        L=$RUNDIR/$run.log
        failure=$(grep -a -m 1 -o -E '^((Host |Guest )?[Ff]ault \(|STOP: ).{0,90}' "$L" | head -1)
        if [ -z "$failure" ] && grep -a -q '^STALL: no frame' "$L"; then
            failure="a hang ($(grep -a -m 1 '^STALL: no frame' "$L" | cut -c1-70))"
        fi
        cq=$(grep -a -c -E 'Compute queue [0-9]+: ' "$L")
        lg=$(grep -a -c -E '^Late GPU data: a' "$L")
        bat=$($ADB shell "cat /sys/class/power_supply/battery/capacity /sys/class/power_supply/battery/status" < /dev/null | tr -d '\r' | paste -sd' ' -)
        say "$run: after an install: $installed; failure: ${failure:-none}; compute notes: $cq; late GPU data: $lg; battery: $bat"
        installed=no
        return 0
    fi
    installed=no
    printf '%s\n' "$out" | grep -q -E '^RUN-LOST' && stop "$run: the device went away - restore_owner.sh after adb is back"
    printf '%s\n' "$out" | grep -q -E 'the game is running|started during the cool-down' && stop "$run: the owner's game is running"
    printf '%s\n' "$out" | grep -q -E 'battery is below' && stop "$run: the battery"
    printf '%s\n' "$out" | grep -q -E 'another run is in progress|no device' && stop "$run: $(printf '%s' "$out" | tail -n 1)"
    skips=$((skips + 1))
    say "PAIRS-SKIP $run: $(printf '%s' "$out" | tail -n 2 | tr '\n' ' ' | cut -c1-300)"
    [ $skips -ge 3 ] && stop "three runs skipped in a row"
    return 1
}
say "== $tag: $game, $pairs pairs ABBA of $secs s; A=$(basename "$apk_a"), B=$(basename "$apk_b"); extras [$extra]; $(date +%H:%M)"
n=1
while [ "$n" -le "$pairs" ]; do
    if [ $((n % 2)) = 1 ]; then a=A b=B; else a=B b=A; fi
    put $a; one "$tag-$a$n" || one "$tag-$a${n}r" || true
    put $b; one "$tag-$b$n" || one "$tag-$b${n}r" || true
    n=$((n + 1))
done
put B
d=
if [ -n "${REF:-}" ]; then
    d=$($ADB shell "cd $G && diff -r user user-backup-$REF > /dev/null && echo same || echo DIFFER" < /dev/null | tr -d '\r')
    d="; the owner's user/ against the pair $REF: $d"
fi
say "PAIRS-DONE $(date +%H:%M): B installed$d; their More environment: [$(owner_extra_env < /dev/null)]"
