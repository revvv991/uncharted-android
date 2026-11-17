#!/usr/bin/env bash
# continue_run.sh <tag> <u1|u2|u3> <secs> [KEY=VALUE ...]: one measured run from the owner's
# Continue save - the game started through the launcher with the extra variables, the collection
# menu navigated to <game>, Continue, <secs> seconds of the camera turning (INPUT, rx=200), the
# frame rates and the GPU faults of the run, screenshots at the start and the end; then the owner's
# files, saves and settings put back from the fresh backup. Refuses while the game runs.
#   START   the game the OWNER played last (by default owner_game of g2.sh: their newest save):
#           the restored saves put the menu there; the route counts steps from it. Wrong START =
#           the wrong game: the line then says WRONG GAME (and check <tag>-end.png or the
#           draws/dispatches line of BB_FRAME_STATS: U3's museum has ~620 dispatches a frame).
#   LIMIT   the frame limit (30; 0 = off), SIZE (1280x720), LOAD (120 s to the gameplay), TITLE (45)
#   DRIVER  a driver package folder under files/drivers (the bundled Turnip by default)
#   SAVES   a folder with another save of <game> (a reporter's: SAVEDATA0n0, SAVEDATA0nP and their
#           .sce_sys): pushed over the owner's for the run, so Continue is that save's place (an
#           issue's scene measured: #35's chapters 16 and 20 of U3). The owner's come back with
#           the rest at the end. See save_session.sh for looking around by hand.
#   SF_PACING=1  SurfaceFlinger's present times of the game's frames during the turning
#           (sf_pacing.sh into <tag>.sf; the summary line gets ../sf_pacing.py's screen cadence)
#   SF_STATS=1  SurfaceFlinger's TimeStats over the turning (per layer: frames, dropped frames,
#           present-to-present histograms) into <tag>.timestats, its janky frames into <tag>.jank
#   RECORD=<secs>  a screen recording of <secs> from 10 s into the turning into <tag>.mp4 (the
#           motion between frames: ../motion.py); at most the turning less 15 s
#   GPU_BUSY=1  the GPU's busy percentage and clock every 100 ms over the turning into <tag>.gpu
#           (lines "<uptime s> <busy %> <clock Hz>")
#   OUT     where <tag>.log, <tag>.dmesg, <tag>-game.png, <tag>-end.png and summary.txt go (g2.sh)
#   KWATCH=0  no live record (g2.sh's kwatch_start: <tag>.klive and <tag>.temps, the kernel log and
#           the temperatures as the run went) and no guard (kwatch_guard: the game stopped when a
#           sample is too hot - HOT_CPU, HOT_PMIC, HOT_BATTERY; HOT_STOP=0 for the guard alone)
#   COOL_CPU, COOL_BOARD, COOL_MAX  the cooling before the run (g2.sh's cool_wait)
# Output line: "<tag> (<extras>): <FPS a second...> | mean X over N; GPU faults N (BR, BV, PROT),
# page faults F, hangs H; hottest: cpu, gpu, board, pmic, battery" - the kernel's events of the
# whole run by its time window, the game's process or not. When adb stops answering in the run:
# "THE DEVICE WENT AWAY ...", RUN-LOST and no RUN-DONE; the live record keeps the last lines
# (<tag>.klive, <tag>.glive), the owner's files wait in the backup pair it names.
set -uo pipefail
. "$(dirname -- "${BASH_SOURCE[0]}")/g2.sh"
tag=$1 game=$2 secs=$3; shift 3; extra="$*"
RUN=$OUT/runs; mkdir -p "$RUN"
LIMIT=${LIMIT:-30}

if ! device_here; then echo "$tag: no device (adb) - skipped"; exit 1; fi
run_lock "$tag" || exit 1               # one run at a time; its EXIT trap stops the live record
START=${START:-$(owner_game)}       # the owner's last game: where the menu stands
if game_running; then echo "$tag: the game is running - skipped"; exit 1; fi
if ! battery_ok; then echo "$tag: the battery is below ${BATTERY_MIN:-20}% and not charging - skipped"; exit 1; fi
if ! screen_awake; then echo "$tag: the screen does not wake - skipped"; exit 1; fi
cooled=$(cool_wait)                     # a warm device starts its play that much nearer the limits
# The wait can take minutes: the owner may have started the game meanwhile.
if game_running; then echo "$tag: the game was started during the cool-down - skipped"; exit 1; fi
stamp=$(date +%Y%m%d-%H%M%S)
back_up "$stamp" || exit 1
owner_env=$(owner_extra_env)
echo "owner's extra environment: [${owner_env}]"
# lost: adb answers no more (the G2 reset in a run on 2026-11-15) - nothing can be read or put
# back; the live record (g2.sh's kwatch_start) has the kernel's and the game's last lines.
lost() {
    kwatch_stop
    echo "$tag ($extra): THE DEVICE WENT AWAY (adb lost by $(date +%H:%M:%S): a reset?) | last sample: $(tail -n 1 "$RUN/$tag.temps" 2> /dev/null) | hottest: $(kwatch_hottest "$RUN/$tag") | the kernel's and the game's last lines: $tag.klive, $tag.glive | the owner's files NOT put back: restore_owner.sh $stamp" | tee -a "$RUN/summary.txt"
    echo RUN-LOST; exit 2
}
if [ -n "${SAVES:-}" ]; then
    case "$game" in u1) n=1;; u2) n=2;; u3) n=3;; esac
    title=$($ADB shell "ls $G/user/savedata/1/" | tr -d '\r' | head -1)
    for d in SAVEDATA0${n}0 SAVEDATA0${n}0.sce_sys SAVEDATA0${n}P SAVEDATA0${n}P.sce_sys; do
        if [ -z "$title" ] || ! $ADB push "$(cygpath -w "$SAVES/$d")" "$G/user/savedata/1/$title/" > /dev/null; then
            echo "$tag ($extra): the save's $d did not go to the device - skipped"
            put_back "$stamp" "$(printf '%s' "$owner_env" | sed ':a;N;$!ba;s/\n/\\\\n/g')"
            exit 1
        fi
    done
fi

env="BB_PAD_FILE=$G/pad.txt"
[ -n "${DRIVER:-}" ] && env="$env\\\\nBB_VK_DRIVER_DIR=/data/user/0/$P/files/drivers/$DRIVER"
for kv in $extra; do env="$env\\\\n$kv"; done
$ADB shell am force-stop $P; sleep 1
$ADB shell "echo > $G/pad.txt; input keyevent KEYCODE_WAKEUP; [ -f $G/bbport.ini.c-bak ] || cp $G/bbport.ini $G/bbport.ini.c-bak"
$ADB shell am start -n $P/dev.unchartedg2.game.Launcher --es resolution ${SIZE:-1280x720} --es fps_limit $LIMIT --es touch_pad 0 --es extra_env "$env" > /dev/null; sleep 6
$ADB shell "am force-stop $P; sed -i -e 's/^fps_limit=.*/fps_limit=$LIMIT/' -e 's/^upscaler=.*/upscaler=off/' -e 's/^frame_gen=.*/frame_gen=off/' $G/bbport.ini"
kmark=$(kernel_mark)
kwatch_start "$RUN/$tag"
$ADB shell am start -n $P/dev.unchartedg2.game.Launcher --ez play true > /dev/null

sleep 50
device_here || lost
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
device_here || lost
fps=$($ADB shell "grep 'Frame rate' $G/bb-probe.log" | tail -n +$((f0 + 1)) | awk '{for(i=1;i<=NF;i++) if($i=="FPS,") {printf "%s ", $(i-1); s+=$(i-1); n++}} END {if (n) printf "| mean %.1f over %d", s/n, n}')
$ADB shell "dmesg" > "$RUN/$tag.dmesg"
kev=$(kernel_events "$RUN/$tag.dmesg" "$kmark")
kwatch_stop
hot=$(kwatch_hottest "$RUN/$tag")

alive=$($ADB shell pidof $P | tr -d '\r')
$ADB shell am force-stop $P
$ADB shell "cp $G/bb-probe.log /sdcard/Download/run-$tag.log"; $ADB pull /sdcard/Download/run-$tag.log "$(cygpath -w "$RUN/$tag.log")" > /dev/null; $ADB shell rm -f /sdcard/Download/run-$tag.log
[ -s "$RUN/$tag.log" ] && rm -f "$RUN/$tag.glive"       # the whole log is here: its live copy is not needed
grep -a -E 'custom driver|^GPU: Turnip|^tu: |^tu/kgsl' "$RUN/$tag.log" 2>/dev/null | head -6 | cut -c1-160
# A game that died on the way (2026-11-16: a crash before the play left a line without frame rates
# and without a word): the log's fault or stop line and the guest address, or that it was gone.
crash=$(grep -a -m 1 -E '^((Host |Guest )?[Ff]ault \(|STOP: )' "$RUN/$tag.log" 2> /dev/null | cut -c1-100)
rip=$(grep -a -m 1 -o 'guest rip 0x[0-9a-f]*' "$RUN/$tag.log" 2> /dev/null)
if [ -n "$crash" ] || [ -z "$alive" ]; then fps="THE GAME DIED (${crash:-gone at the end of the run, no fault line}${rip:+, $rip}) | $fps"; fi
stopped=$(hot_stop_note "$RUN/$tag"); [ -n "$stopped" ] && fps="STOPPED BY THE GUARD: TOO HOT ($stopped) | $fps"
wrong=$(wrong_game "$RUN/$tag.log" "$game")
echo "$tag ($extra): ${wrong:+$wrong; }$fps; $kev; hottest: $hot${cooled:+ ($cooled)}$screen" | tee -a "$RUN/summary.txt"
put_back "$stamp" "$(printf '%s' "$owner_env" | sed ':a;N;$!ba;s/\n/\\\\n/g')"
echo RUN-DONE
