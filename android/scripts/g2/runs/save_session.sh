#!/usr/bin/env bash
# save_session.sh start <tag> <u1|u2|u3> <saves dir> [KEY=VALUE ...]
#               | step '<pad tokens>' [secs] | hold '<pad tokens>' | release | shot
#               | log '<regex>' [lines] | stop
# A hand-driven session of one game from ANOTHER save - a reporter's, for an issue that lives at
# their place in the game (#37: U2's Chapter Select; #35: U3's desert chapters). start backs the
# owner's files and saves up, pushes that game's save folders from <saves dir> (SAVEDATA0n0,
# SAVEDATA0nP and their .sce_sys; n = 1, 2, 3) over the owner's, starts the game with the
# variables and leaves it at the game's main menu. Then: step = a pad input and a screenshot,
# hold / release = an input kept down (walking, the camera), shot = a screenshot, log = lines of
# the game's log. stop pulls the log and puts the owner's files, saves and settings back.
# Refuses to start while the game runs.
#   START  the game the OWNER played last (owner_game by default): the collection's menu stands there (the
#          collection's own save is not replaced); see continue_run.sh
#   LIMIT  the frame limit (30), SIZE (1280x720), TITLE (45 s from the choice to the title screen)
# Outputs: $OUT/hold/<tag>-NN.png and <tag>-<HHMM>.log. A reporter's saves are their files: keep
# them outside the repository, and never leave them on the device (stop restores the owner's).
set -uo pipefail
. "$(dirname -- "${BASH_SOURCE[0]}")/g2.sh"
HOLD=$OUT/hold; mkdir -p "$HOLD"; STATE=$HOLD/session-state.txt
shot() {
    local n f
    n=$(ls "$HOLD/$TAG"-[0-9][0-9][0-9].png 2>/dev/null | wc -l); f=$HOLD/$TAG-$(printf %03d "$n").png
    $ADB exec-out screencap -p > "$f"; echo "$(basename "$f")"
}
cmd=${1:-}
if [ "$cmd" != start ]; then
    [ -f "$STATE" ] || { echo "no session (start one first)"; exit 1; }
    . "$STATE"
fi
case "$cmd" in
start)
    [ $# -ge 4 ] || { echo "usage: save_session.sh start <tag> <u1|u2|u3> <saves dir> [KEY=VALUE ...]"; exit 1; }
    TAG=$2 game=$3 saves=$4; shift 4
    case "$game" in u1) n=1;; u2) n=2;; u3) n=3;; *) echo "game: u1, u2 or u3"; exit 1;; esac
    [ -f "$STATE" ] && { echo "a session is open ($STATE): stop it first"; exit 1; }
    if game_running; then echo "the game is running: not started"; exit 1; fi
    START=${START:-$(owner_game)}       # the owner's last game: where the menu stands
    if ! battery_ok; then echo "the battery is below ${BATTERY_MIN:-20}% and not charging: not started"; exit 1; fi
    screen_awake || { echo "the screen does not wake: not started"; exit 1; }
    for d in SAVEDATA0${n}0 SAVEDATA0${n}0.sce_sys SAVEDATA0${n}P SAVEDATA0${n}P.sce_sys; do
        [ -d "$saves/$d" ] || { echo "no $d in $saves"; exit 1; }
    done
    title=$($ADB shell "ls $G/user/savedata/1/" | tr -d '\r' | head -1)
    [ -n "$title" ] || { echo "no save folder of the owner's on the device"; exit 1; }
    stamp=$(date +%Y%m%d-%H%M%S)
    back_up "$stamp" || exit 1
    owner_extra_env > "$HOLD/session-oenv.txt"
    printf 'STAMP=%s\nTAG=%s\n' "$stamp" "$TAG" > "$STATE"
    echo "owner's extra environment: [$(cat "$HOLD/session-oenv.txt")]"
    for d in SAVEDATA0${n}0 SAVEDATA0${n}0.sce_sys SAVEDATA0${n}P SAVEDATA0${n}P.sce_sys; do
        $ADB push "$(cygpath -w "$saves/$d")" "$G/user/savedata/1/$title/" > /dev/null || { echo "push of $d failed: stop the session"; exit 1; }
    done
    $ADB shell "ls -la $G/user/savedata/1/$title/SAVEDATA0${n}0/" | tr -d '\r'
    env="BB_PAD_FILE=$G/pad.txt"
    for kv in "$@"; do env="$env\\\\n$kv"; done
    LIMIT=${LIMIT:-30}
    $ADB shell am force-stop $P; sleep 1
    $ADB shell "echo > $G/pad.txt; input keyevent KEYCODE_WAKEUP; [ -f $G/bbport.ini.c-bak ] || cp $G/bbport.ini $G/bbport.ini.c-bak"
    $ADB shell am start -n $P/dev.unchartedg2.game.Launcher --es resolution ${SIZE:-1280x720} --es fps_limit $LIMIT --es touch_pad 0 --es extra_env "$env" > /dev/null; sleep 6
    $ADB shell "am force-stop $P; sed -i -e 's/^fps_limit=.*/fps_limit=$LIMIT/' -e 's/^upscaler=.*/upscaler=off/' -e 's/^frame_gen=.*/frame_gen=off/' $G/bbport.ini"
    $ADB shell am start -n $P/dev.unchartedg2.game.Launcher --ez play true > /dev/null
    sleep 50; press cross                   # the collection's title
    sleep 10                                # the menu stands on the owner's last game: START
    pos() { case "$1" in u1) echo 0;; u2) echo 1;; u3) echo 2;; esac; }
    case $(( ($(pos "$game") - $(pos "${START:-u1}") + 3) % 3 )) in
        1) press lx=255 0.15; sleep 6;;     # one step right
        2) press lx=0 0.15; sleep 6;;       # one step left (the ring of three wraps)
    esac
    press cross
    sleep ${TITLE:-45}; press cross         # the game's title screen
    sleep 10
    echo "SESSION-STARTED ($game's main menu: $(shot))"
    ;;
step)
    press "$2" 0.25; sleep "${3:-2}"
    echo "step [$2] ${3:-2}s -> $(shot)"
    ;;
hold)
    $ADB shell "echo '$2' > $G/pad.txt"; echo "held [$2]"
    ;;
release)
    $ADB shell "echo > $G/pad.txt"; echo released
    ;;
shot)
    echo "shot -> $(shot)"
    ;;
log)
    $ADB shell "grep -a -E '$2' $G/bb-probe.log | tail -${3:-12}" | tr -d '\r' | cut -c1-240
    ;;
stop)
    $ADB shell "echo > $G/pad.txt"
    $ADB shell am force-stop $P
    $ADB shell "cp $G/bb-probe.log /sdcard/Download/session.log"
    $ADB pull /sdcard/Download/session.log "$(cygpath -w "$HOLD/$TAG-$(date +%H%M).log")" > /dev/null; $ADB shell rm -f /sdcard/Download/session.log
    put_back "$STAMP" "$(sed ':a;N;$!ba;s/\n/\\\\n/g' "$HOLD/session-oenv.txt")"
    rm -f "$STATE" "$HOLD/session-oenv.txt"
    echo SESSION-STOPPED
    ;;
*)
    echo "usage: save_session.sh start <tag> <u1|u2|u3> <saves dir> [KEY=VALUE ...] | step '<tokens>' [secs] | hold '<tokens>' | release | shot | log '<regex>' [lines] | stop"; exit 1
    ;;
esac
