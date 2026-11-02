#!/usr/bin/env bash
# continue_run.sh <tag> <u1|u2|u3> <secs> [KEY=VALUE ...]: one measured run from the owner's
# Continue save - the game started through the launcher with the extra variables, the collection
# menu navigated to <game>, Continue, <secs> seconds of the camera turning (INPUT, rx=200), the
# frame rates and the GPU faults of the run, screenshots at the start and the end; then the owner's
# files, saves and settings put back from the fresh backup. Refuses while the game runs.
#   START   the game the OWNER played last (u1): the restored saves put the menu there; the route
#           counts steps from it. Wrong START = the wrong game (check <tag>-end.png or the
#           draws/dispatches line of BB_FRAME_STATS: U3's museum has ~620 dispatches a frame).
#   LIMIT   the frame limit (30; 0 = off), SIZE (1280x720), LOAD (120 s to the gameplay), TITLE (45)
#   DRIVER  a driver package folder under files/drivers (the bundled Turnip by default)
#   SF_PACING=1  SurfaceFlinger's present times of the game's frames during the turning
#           (sf_pacing.sh into <tag>.sf; the summary line gets ../sf_pacing.py's screen cadence)
#   SF_STATS=1  SurfaceFlinger's TimeStats over the turning (per layer: frames, dropped frames,
#           present-to-present histograms) into <tag>.timestats, its janky frames into <tag>.jank
#   RECORD=<secs>  a screen recording of <secs> from 10 s into the turning into <tag>.mp4 (the
#           motion between frames: ../motion.py); at most the turning less 15 s
#   GPU_BUSY=1  the GPU's busy percentage and clock every 100 ms over the turning into <tag>.gpu
#           (lines "<uptime s> <busy %> <clock Hz>")
#   OUT     where <tag>.log, <tag>.dmesg, <tag>-game.png, <tag>-end.png and summary.txt go (g2.sh)
# Output line: "<tag> (<extras>): <FPS a second...> | mean X over N; GPU faults N (BR, BV, PROT)".
set -uo pipefail
. "$(dirname -- "${BASH_SOURCE[0]}")/g2.sh"
tag=$1 game=$2 secs=$3; shift 3; extra="$*"
RUN=$OUT/runs; mkdir -p "$RUN"
LIMIT=${LIMIT:-30}

if game_running; then echo "$tag: the game is running - skipped"; exit 1; fi
if ! battery_ok; then echo "$tag: the battery is below ${BATTERY_MIN:-20}% and not charging - skipped"; exit 1; fi
if ! screen_awake; then echo "$tag: the screen does not wake - skipped"; exit 1; fi
stamp=$(date +%Y%m%d-%H%M%S)
back_up "$stamp" || exit 1
owner_env=$(owner_extra_env)
echo "owner's extra environment: [${owner_env}]"

env="BB_PAD_FILE=$G/pad.txt"
[ -n "${DRIVER:-}" ] && env="$env\\\\nBB_VK_DRIVER_DIR=/data/user/0/$P/files/drivers/$DRIVER"
for kv in $extra; do env="$env\\\\n$kv"; done
$ADB shell am force-stop $P; sleep 1
$ADB shell "echo > $G/pad.txt; input keyevent KEYCODE_WAKEUP; [ -f $G/bbport.ini.c-bak ] || cp $G/bbport.ini $G/bbport.ini.c-bak"
$ADB shell am start -n $P/dev.unchartedg2.game.Launcher --es resolution ${SIZE:-1280x720} --es fps_limit $LIMIT --es touch_pad 0 --es extra_env "$env" > /dev/null; sleep 6
$ADB shell "am force-stop $P; sed -i -e 's/^fps_limit=.*/fps_limit=$LIMIT/' -e 's/^upscaler=.*/upscaler=off/' -e 's/^frame_gen=.*/frame_gen=off/' $G/bbport.ini"
$ADB shell am start -n $P/dev.unchartedg2.game.Launcher --ez play true > /dev/null

sleep 50
# The game draws by now (a screen turned off at the start leaves it waiting for a surface).
if [ "$($ADB shell "grep -c '' $G/bb-probe.log" | tr -d '\r')" -lt 10 ] || ! $ADB shell dumpsys power | grep -q 'mWakefulness=Awake'; then
    echo "$tag ($extra): no game after 50 s (the screen off?) - skipped"
    put_back "$stamp" "$(printf '%s' "$owner_env" | sed ':a;N;$!ba;s/\n/\\\\n/g')"
    exit 1
fi
press cross                             # the collection's title
sleep 10                                # the menu stands on the owner's last game: START
pos() { case "$1" in u1) echo 0;; u2) echo 1;; u3) echo 2;; esac; }
case $(( ($(pos "$game") - $(pos "${START:-u1}") + 3) % 3 )) in
    1) press lx=255 0.15; sleep 6;;     # one step right
    2) press lx=0 0.15; sleep 6;;       # one step left (the ring of three wraps)
esac
press cross
sleep ${TITLE:-45}; press cross
sleep 8; press cross                    # Continue
sleep 40; press cross                   # the offline dialog
sleep 15; press cross
sleep ${LOAD:-120}; $ADB exec-out screencap -p > "$RUN/$tag-game.png"
pid=$($ADB shell pidof $P | tr -d '\r')
f0=$($ADB shell "grep -c 'Frame rate' $G/bb-probe.log" | tr -d '\r')
$ADB shell "echo '${INPUT-rx=200}' > $G/pad.txt"
sfpid=
if [ -n "${SF_PACING:-}" ]; then bash "$RUNS/sf_pacing.sh" "$RUN/$tag.sf" "$secs" > /dev/null 2>&1 & sfpid=$!; fi
[ -n "${SF_STATS:-}" ] && $ADB shell dumpsys SurfaceFlinger --timestats -enable -clear > /dev/null
gpupid=
if [ -n "${GPU_BUSY:-}" ]; then
    $ADB shell "K=/sys/class/kgsl/kgsl-3d0; i=0; while [ \$i -lt $((secs * 10)) ]; do i=\$((i + 1)); echo \$(cut -d' ' -f1 /proc/uptime) \$(cut -d' ' -f1 \$K/gpu_busy_percentage) \$(cat \$K/gpuclk); sleep 0.1; done" > "$RUN/$tag.gpu" 2>/dev/null & gpupid=$!
fi
recpid=
if [ -n "${RECORD:-}" ]; then (sleep 10; $ADB shell screenrecord --time-limit "$RECORD" /sdcard/Download/rec-$tag.mp4) > /dev/null 2>&1 & recpid=$!; fi
sleep "$secs"
if [ -n "${SF_STATS:-}" ]; then
    $ADB shell dumpsys SurfaceFlinger --timestats -dump > "$RUN/$tag.timestats"
    $ADB shell dumpsys SurfaceFlinger --frametimeline -jank > "$RUN/$tag.jank"
    $ADB shell dumpsys SurfaceFlinger --timestats -disable -clear > /dev/null
fi
$ADB shell "echo > $G/pad.txt"
if [ -n "$recpid" ]; then
    wait $recpid
    $ADB pull /sdcard/Download/rec-$tag.mp4 "$(cygpath -w "$RUN/$tag.mp4")" > /dev/null; $ADB shell rm -f /sdcard/Download/rec-$tag.mp4
fi
screen=
if [ -n "$sfpid" ]; then wait $sfpid; screen="; $(python -I "$(cygpath -w "$RUNS/../sf_pacing.py")" "$(cygpath -w "$RUN/$tag.sf")" "$LIMIT" --summary)"; fi
$ADB exec-out screencap -p > "$RUN/$tag-end.png"
fps=$($ADB shell "grep 'Frame rate' $G/bb-probe.log" | tail -n +$((f0 + 1)) | awk '{for(i=1;i<=NF;i++) if($i=="FPS,") {printf "%s ", $(i-1); s+=$(i-1); n++}} END {if (n) printf "| mean %.1f over %d", s/n, n}')
$ADB shell "dmesg" > "$RUN/$tag.dmesg"
faults=$(awk -v pid="$pid" '/gmu: CP/ { k = ($0 ~ /BV opcode/) ? "BV" : ($0 ~ /Protected/) ? "PROT" : "BR" } index($0, "[" pid "]: cmdline") { c[k]++; n++ } END { printf "%d (BR %d, BV %d, PROT %d)", n, c["BR"], c["BV"], c["PROT"] }' "$RUN/$tag.dmesg")

$ADB shell am force-stop $P
$ADB shell "cp $G/bb-probe.log /sdcard/Download/run-$tag.log"; $ADB pull /sdcard/Download/run-$tag.log "$(cygpath -w "$RUN/$tag.log")" > /dev/null; $ADB shell rm -f /sdcard/Download/run-$tag.log
grep -a -E 'custom driver|^GPU: Turnip|^tu: |^tu/kgsl' "$RUN/$tag.log" 2>/dev/null | head -6 | cut -c1-160
echo "$tag ($extra): $fps; GPU faults $faults$screen" | tee -a "$RUN/summary.txt"
put_back "$stamp" "$(printf '%s' "$owner_env" | sed ':a;N;$!ba;s/\n/\\\\n/g')"
echo RUN-DONE
