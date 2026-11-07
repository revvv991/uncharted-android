#!/usr/bin/env bash
# np3_live.sh <tag> <secs> <step file> [KEY=VALUE ...]: the boat from user-boat2 like np3_boatscene.sh,
# with BB_SHADER_DEBUG_FILE on the phone; the step file's lines "<seconds into the scene> <line>"
# are written to that file at those times ("trace <hash>", "skip <hash>,...", or "-" to empty it),
# so draws can be traced or skipped in the fire without a restart. Prints the hang times.
here=$(cygpath -m "$(cd "$(dirname "$0")" && pwd)")
tag=$1 secs=$2 steps=$3; shift 3; extras="$*"
N="bash $here/np3.sh"
OUT=${NP3_OUT:-$(cygpath -m "$(cd "$here/../../.." && pwd)")/out/np3}
G=/sdcard/Android/data/dev.unchartedandroid/files
$N cool
$N sh "cd $G && cp -a ${BOAT:-user-boat2}/. user/ 2>/dev/null; diff -r ${BOAT:-user-boat2} user > /dev/null && echo boat-checkpoint || echo BOAT-RESTORE-FAILED; echo > $G/shaderdbg.txt"
$N start "$tag" BB_FRAME_STATS=1 BB_SHADER_DEBUG_FILE=$G/shaderdbg.txt "$@" > /dev/null
sleep 45
$N press cross; sleep 10
$N press cross; sleep 45
$N press cross; sleep 8
$N press cross; sleep 60
$N shot load > /dev/null
$N press cross; sleep 50
$N shot start > /dev/null
D=$(cat "$OUT/current")
t0=$(date +%s)
$N pad rx=200
while read -r at line <&3; do
    [ -z "$at" ] && continue
    while [ $(( $(date +%s) - t0 )) -lt "$at" ]; do sleep 1; done
    if [ "$line" = "-" ]; then $N sh "echo > $G/shaderdbg.txt"; else $N sh "echo '$line' > $G/shaderdbg.txt"; fi
    echo "$(date +%H:%M:%S) +$at s: $line"
done 3< "$steps"
while [ $(( $(date +%s) - t0 )) -lt "$secs" ]; do sleep 2; done
$N pad ''
$N shot end > /dev/null
temps=$($N temps)
$N pull > /dev/null
hang=$(grep -E 'gpu timeout ctx|GPU hang detected|GPC error|opcode error' "$OUT/$D/logcat-all.txt" | cut -c7-18 | tr '\n' ' ')
echo "$tag ($extras): hangs at $hang; $temps"
