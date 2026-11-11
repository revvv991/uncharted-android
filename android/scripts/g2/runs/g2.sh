#!/usr/bin/env bash
# g2.sh: sourced by the run scripts of this directory (the 2026-10-31 generation: the public app
# dev.unchartedandroid over adb, Wi-Fi or USB). Defines:
#   ADB   - adb for the device: G2=<ip:port or serial> in the environment, else the first line of
#           ../device.txt (git-ignored: a Wi-Fi address is personal), else adb's only device.
#   P, G  - the package and its files directory on /sdcard.
#   OUT   - where logs, screenshots and summaries go: OUT in the environment, else <repo>/out/g2
#           (out/ is git-ignored). Never put them into the repository.
#   FILES, LAUNCHER_KEYS - the launcher files a run backs up and restores, and the env.txt lines
#           the launcher writes itself (everything else in env.txt is the owner's More environment).
#   game_running - true while the game runs (the launcher activity alone has a small RSS).
#   press <tokens> [secs] - a pad input through BB_PAD_FILE (pad.txt): cross, lx=255, rx=200 ...
# Rules (see .claude/skills/g2-runs/SKILL.md): never touch a running game; a run backs the owner's
# files and saves up first and puts them back at the end; START is the game the OWNER played
# last (the restored saves put the collection menu there), not the game your last run played.
export MSYS_NO_PATHCONV=1
RUNS=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd -- "$RUNS/../../../.." && pwd)
dev=${G2:-$(head -1 "$RUNS/../device.txt" 2>/dev/null)}
ADB="adb${dev:+ -s $dev}"
P=dev.unchartedandroid
G=/sdcard/Android/data/$P/files
OUT=${OUT:-$REPO/out/g2}
FILES="args-u1.txt args-u2.txt args-u3.txt args.txt bbport.ini env-u1.txt env-u2.txt env-u3.txt env.txt guest-argv.txt loadexec-last.txt"
LAUNCHER_KEYS='^(#|$|BB_FULLSCREEN=|BB_FPS_LOG=|BB_MIDFRAME_FLUSH=|BB_DEFERRED_SUBMIT=|BB_SKIN_BATCH=|BB_CS_SPLIT=|BB_OBJECT_MOTION=|BB_THREAD_NICE=|BB_MAX_FRAMES_AHEAD=|BB_DUMP_REQUEST=|BB_RENDER_CPUS=|BB_VK_DRIVER=|BB_VK_DRIVER_DIR=|BB_VBLANK_HZ=|BB_SHOW_FPS=|BB_OFFLINE_NPID=)'

game_running() {
    local rss
    rss=$($ADB shell 'p=$(pidof dev.unchartedandroid) && ps -o RSS= -p $p' 2>/dev/null | tr -d '\r ')
    [ "${rss:-0}" -gt 500000 ]
}

press() {
    $ADB shell "echo $1 > $G/pad.txt; sleep ${2:-0.4}; echo > $G/pad.txt"
}

# The owner's More environment: env.txt without the launcher's own lines, one line per entry.
owner_extra_env() {
    $ADB shell "grep -v -E '$LAUNCHER_KEYS' $G/env.txt" | tr -d '\r' | sed '/^$/d' | paste -sd'\n' -
}

# back_up <stamp>: the launcher files and the user data into cfg-backup-<stamp>/user-backup-<stamp>.
back_up() {
    $ADB shell "mkdir $G/cfg-backup-$1 $G/user-backup-$1 && cd $G && cp $FILES cfg-backup-$1/ && cp -a user/. user-backup-$1/ && echo backup cfg-backup-$1 user-backup-$1"
}

# put_back <stamp> <owner extra env, lines joined by \n>: the files and saves from the backup, the
# launcher's settings as the owner has them (the limit 30, the touch pad auto, Turnip, their More
# environment), the app closed. Prints DIFFERS lines when a file did not come back.
put_back() {
    local stamp=$1 oenv=$2
    # The callers join the lines with newlines or with \n (one backslash or two); the launcher's
    # extra takes \n between them, which the device shell keeps only inside quotes (unquoted, it
    # ate the backslash: the owner's More environment became one line "...=1nBB_...", 2026-11-11).
    oenv=$(printf '%s' "$oenv" | sed -e 's/\\\\n/\n/g' -e 's/\\n/\n/g' | sed ':a;N;$!ba;s/\n/\\n/g')
    [ -z "$oenv" ] && oenv=' '
    $ADB shell am force-stop $P
    $ADB shell "echo > $G/pad.txt; [ -f $G/bbport.ini.c-bak ] && mv $G/bbport.ini.c-bak $G/bbport.ini"
    $ADB shell "am start -n $P/dev.unchartedg2.game.Launcher --es resolution ${OWNER_SIZE:-1280x720} --es fps_limit 30 --es touch_pad auto --es driver turnip --es extra_env '$oenv'" > /dev/null; sleep 5
    $ADB shell am force-stop $P
    $ADB shell "cd $G && cp cfg-backup-$stamp/* . && cp -a user-backup-$stamp/. user/ && for f in $FILES; do cmp -s \$f cfg-backup-$stamp/\$f || echo \"\$f DIFFERS\"; done; diff -r user user-backup-$stamp > /dev/null && echo user-restored || echo USER-DIFFERS; pidof $P || echo not-running"
}

# screen_awake: the screen on (the game needs its surface: with the screen off it starts and waits
# without a frame). KEYCODE_WAKEUP, then mWakefulness=Awake. 2026-11-01: the power button pressed
# at a run's start left the game without a surface for the whole run.
screen_awake() {
    local i
    for i in 1 2 3 4 5; do
        $ADB shell input keyevent KEYCODE_WAKEUP
        $ADB shell dumpsys power | grep -q 'mWakefulness=Awake' && return 0
        sleep 1
    done
    return 1
}

# battery_ok: the charge is at least BATTERY_MIN percent (20) or the device is charging.
battery_ok() {
    local cap status
    cap=$($ADB shell cat /sys/class/power_supply/battery/capacity | tr -d '\r')
    status=$($ADB shell cat /sys/class/power_supply/battery/status | tr -d '\r')
    [ "${cap:-0}" -ge "${BATTERY_MIN:-20}" ] || [ "$status" = Charging ]
}
