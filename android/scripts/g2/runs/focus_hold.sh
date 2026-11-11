#!/usr/bin/env bash
# focus_hold.sh start | step '<pad tokens>' [secs] | value | stop: an interactive run from the
# owner's Continue save (U2 by default) for looking at something while holding an input - the
# aiming blur of 2026-10-31 was found this way: start, then `step 'l2' 3` (aim for 3 s, a
# screenshot, the autofocus distance the game got from BB_TINY_READBACK_LOG), `step 'l2 rx=0' 1`,
# ... and stop, which puts the owner's files, saves and settings back. GAME=u1|u2|u3 (u2),
# START = the owner's last game (g2.sh), EXTRA="KEY=VALUE KEY=VALUE" more variables, TOUCH=0|1|auto
# the on-screen gamepad (in bbport.ini: the launcher's touch_pad extra loses to the ini's value;
# `adb shell input motionevent` drives the pad, issue #34).
# The run carries BB_TINY_READBACK=1 BB_TINY_READBACK_LOG=1 BB_READBACK_SMALL_BUFFERS=1 (the
# launcher's U2/U3 defaults since 0.2.11, named here so the log has the focus line).
set -uo pipefail
. "$(dirname -- "${BASH_SOURCE[0]}")/g2.sh"
HOLD=$OUT/hold; mkdir -p "$HOLD"; STATE=$HOLD/state.txt
value() { $ADB shell "grep -a 'Tiny readback' $G/bb-probe.log | tail -1" | tr -d '\r' | sed -E 's/.*= ([0-9.e+-]+) .*/\1/'; }
case "${1:-}" in
start)
    if game_running; then echo "the game is running: abort"; exit 1; fi
    stamp=$(date +%Y%m%d-%H%M%S)
    back_up "$stamp" || exit 1
    owner_env=$(owner_extra_env)
    printf 'STAMP=%s\nOENV=%s\n' "$stamp" "$(printf '%s' "$owner_env" | sed ':a;N;$!ba;s/\n/\\\\n/g')" > "$STATE"
    echo "owner's extra environment: [${owner_env}]"
    env="BB_PAD_FILE=$G/pad.txt"
    for kv in BB_TINY_READBACK=1 BB_TINY_READBACK_LOG=1 BB_READBACK_SMALL_BUFFERS=1 ${EXTRA:-}; do env="$env\\\\n$kv"; done
    $ADB shell am force-stop $P; sleep 1
    $ADB shell "echo > $G/pad.txt; input keyevent KEYCODE_WAKEUP; [ -f $G/bbport.ini.c-bak ] || cp $G/bbport.ini $G/bbport.ini.c-bak"
    $ADB shell am start -n $P/dev.unchartedg2.game.Launcher --es resolution ${SIZE:-1280x720} --es fps_limit ${LIMIT:-30} --es touch_pad 0 --es extra_env "$env" > /dev/null; sleep 6
    $ADB shell "am force-stop $P; sed -i -e 's/^fps_limit=.*/fps_limit=${LIMIT:-30}/' -e 's/^upscaler=.*/upscaler=off/' -e 's/^frame_gen=.*/frame_gen=off/' ${TOUCH:+-e 's/^touch_pad=.*/touch_pad=$TOUCH/'} $G/bbport.ini"
    $ADB shell am start -n $P/dev.unchartedg2.game.Launcher --ez play true > /dev/null
    sleep 50; press cross                   # the collection's title
    sleep 10                                # the menu stands on the owner's last game: START
    pos() { case "$1" in u1) echo 0;; u2) echo 1;; u3) echo 2;; esac; }
    case $(( ($(pos "${GAME:-u2}") - $(pos "${START:-u1}") + 3) % 3 )) in
        1) press lx=255 0.15; sleep 6;;
        2) press lx=0 0.15; sleep 6;;
    esac
    press cross
    sleep ${TITLE:-45}; press cross
    sleep 8; press cross                    # Continue
    sleep 40; press cross                   # the offline dialog
    sleep 15; press cross
    sleep ${LOAD:-120}; $ADB exec-out screencap -p > "$HOLD/start-game.png"
    $ADB shell "grep -a -m1 'Patches:' $G/bb-probe.log" | tr -d '\r'
    echo "HOLD-STARTED (the game runs; step/value/stop)"
    ;;
step)
    tokens=$2; secs=${3:-3}
    $ADB shell "echo '$tokens' > $G/pad.txt"; sleep "$secs"
    n=$(ls "$HOLD"/step-*.png 2>/dev/null | wc -l); f=$HOLD/step-$(printf %02d $((n + 1))).png
    $ADB exec-out screencap -p > "$f"
    echo "step [$tokens] ${secs}s: value $(value) m -> $(basename "$f")"
    ;;
value) echo "value $(value) m";;
stop)
    . "$STATE"
    $ADB shell am force-stop $P
    $ADB shell "cp $G/bb-probe.log /sdcard/Download/hold.log"; $ADB pull /sdcard/Download/hold.log "$(cygpath -w "$HOLD/hold-$(date +%H%M).log")" > /dev/null; $ADB shell rm -f /sdcard/Download/hold.log
    put_back "$STAMP" "$OENV"
    echo HOLD-STOPPED
    ;;
*) echo "usage: focus_hold.sh start | step '<pad tokens>' [secs] | value | stop"; exit 1;;
esac
