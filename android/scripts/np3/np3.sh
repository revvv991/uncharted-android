#!/usr/bin/env bash
# np3.sh: the port on the Nothing Phone 3 over USB adb (the first USB device), by hand:
#   np3.sh start <tag> [KEY=VALUE ...]   the game started through the launcher (1280x720, limit 30,
#                                          no touch pad, the bundled Turnip unless DRIVER=<dir>),
#                                          pad input through BB_PAD_FILE; logcat cleared first
#   np3.sh shot <name>                   a screenshot into the tag's folder
#   np3.sh press <tokens> [secs]         a pad input (cross, circle, lx=255, rx=200, ...)
#   np3.sh pull                          bb-probe.log, env.txt, logcat (the app's lines) into the folder
#   np3.sh state                         pid, RSS, the last lines of bb-probe.log
#   np3.sh stop                          the game closed
# The tag's folder: out/np3/<tag>; the current tag in out/np3/current.
export MSYS_NO_PATHCONV=1
P=dev.unchartedandroid
FILES="args-u1.txt args-u2.txt args-u3.txt args.txt bbport.ini env-u1.txt env-u2.txt env-u3.txt env.txt guest-argv.txt loadexec-last.txt"
G=/sdcard/Android/data/$P/files
# Outputs (and device.txt, the phone's adb address) in the repository's out/np3 (git-ignored).
BASE=${NP3_OUT:-$(cygpath -m "$(cd "$(dirname "$0")/../../.." && pwd)")/out/np3}
mkdir -p "$BASE"
# The device: NP3=<ip:port>, else np3/device.txt (wireless debugging), else the first USB device.
# Wireless debugging moves its port: when the saved one is gone, the same IP is looked up over mDNS.
online() { adb devices | awk -v s="$1" '$1 == s && $2 == "device" {f = 1} END {exit !f}'; }
S=${NP3:-$(cat "$BASE/device.txt" 2>/dev/null)}
if [ -n "$S" ] && ! online "$S"; then
    adb connect "$S" > /dev/null 2>&1; sleep 1
    if ! online "$S"; then
        n=$(adb mdns services | awk -v ip="${S%:*}" '$2 ~ /_adb-tls-connect/ && index($3, ip ":") == 1 {print $3; exit}')
        if [ -n "$n" ]; then
            adb connect "$n" > /dev/null 2>&1; sleep 1
            online "$n" && { S=$n; [ -z "$NP3" ] && echo "$n" > "$BASE/device.txt"; }
        fi
    fi
fi
[ -n "$S" ] && online "$S" || S=$(adb devices | awk '/\tdevice$/ && $1 !~ /:/ {print $1; exit}')
[ -n "$S" ] || { echo "no device"; exit 1; }
ADB="adb -s $S"
cmd=$1; shift
case $cmd in
start)
    tag=$1; shift
    echo "$tag" > "$BASE/current"
    mkdir -p "$BASE/$tag"
    env="BB_PAD_FILE=$G/pad.txt"
    for kv in "$@"; do env="$env\\\\n$kv"; done
    $ADB shell am force-stop $P; sleep 1
    $ADB logcat -c
    # the launcher takes the touch pad from bbport.ini before its own settings (restore puts the owner's back)
    $ADB shell "sed -i 's/^touch_pad=.*/touch_pad=${PAD:-0}/' $G/bbport.ini"
    # INI="key=value ...": bbport.ini keys for this run (effects and the like; restore puts the owner's back)
    for kv in $INI; do $ADB shell "sed -i 's/^${kv%%=*}=.*/$kv/' $G/bbport.ini"; done
    $ADB shell "echo > $G/pad.txt; input keyevent KEYCODE_WAKEUP"
    # DRV: turnip (bundled), system, or pkg:<package folder> (installed with np3.sh install)
    $ADB shell am start -n $P/dev.unchartedg2.game.Launcher --es resolution ${SIZE:-1280x720} --es fps_limit ${LIMIT:-30} --es touch_pad 0 --es driver ${DRV:-turnip} --es extra_env "$env" > /dev/null; sleep 5
    $ADB shell am start -n $P/dev.unchartedg2.game.Launcher --ez play true > /dev/null
    sleep 3
    echo "started $tag: pid $($ADB shell pidof $P | tr -d '\r')"
    ;;
shot)
    tag=$(cat "$BASE/current")
    $ADB exec-out screencap -p > "$BASE/$tag/$1.png"
    echo "$BASE/$tag/$1.png"
    ;;
press)
    $ADB shell "echo $1 > $G/pad.txt; sleep ${2:-0.4}; echo > $G/pad.txt"
    ;;
pull)
    tag=$(cat "$BASE/current")
    $ADB pull $G/bb-probe.log "$BASE/$tag/bb-probe.log" > /dev/null
    $ADB pull $G/env.txt "$BASE/$tag/env.txt" > /dev/null
    $ADB logcat -d -v threadtime > "$BASE/$tag/logcat-all.txt"
    pid=$($ADB shell pidof $P | tr -d '\r')
    grep -i -E "adreno|turnip|tu_|vulkan|kgsl|freedreno|mesa|bbport|uncharted|DEBUG|crash|${pid:-nopid}" "$BASE/$tag/logcat-all.txt" > "$BASE/$tag/logcat.txt"
    wc -l "$BASE/$tag/bb-probe.log" "$BASE/$tag/logcat.txt"
    ;;
state)
    $ADB shell "p=\$(pidof $P) && echo pid \$p rss \$(ps -o RSS= -p \$p); tail -n ${1:-8} $G/bb-probe.log"
    ;;
stop)
    $ADB shell am force-stop $P
    ;;
sh)
    # np3.sh sh <command>: a shell command on the phone
    $ADB shell "$@"
    ;;
pad)
    # np3.sh pad <tokens>: hold an input until the next pad call (np3.sh pad '' releases)
    $ADB shell "echo $1 > $G/pad.txt"
    ;;
temps)
    # np3.sh temps: "bat <tenths C> cpu <milli C> gpu <milli C> sys <milli C>" (the battery, the
    # hottest cpu and gpu zones, sys-therm-0)
    $ADB shell 'b=$(dumpsys battery | sed -n "s/^ *temperature: //p"); m=0; g=0; s=0; for z in /sys/class/thermal/thermal_zone*; do case $(cat $z/type 2>/dev/null) in cpu*) v=$(cat $z/temp 2>/dev/null); [ "${v:-0}" -gt $m ] && m=$v;; gpu*) v=$(cat $z/temp 2>/dev/null); [ "${v:-0}" -gt $g ] && g=$v;; sys-therm-0) s=$(cat $z/temp 2>/dev/null);; esac; done; echo bat $b cpu $m gpu $g sys $s' | tr -d '\r'
    ;;
cool)
    # np3.sh cool [bat tenths] [cpu milli]: wait until the battery and the cpu zones are below
    # the limits (37.0 C and 48 C by default), at most 20 minutes; prints the waiting
    lb=${1:-370} lc=${2:-48000}
    $ADB shell am force-stop $P   # the game would keep the phone hot while it cools
    # np3/pause (touch to hold the next run between runs, e.g. for an install; rm to go on)
    while [ -f "$BASE/pause" ]; do sleep 5; done
    for i in $(seq 1 80); do
        set -- $(bash "$0" temps)
        [ "${2:-999}" -le "$lb" ] && [ "${4:-99999}" -le "$lc" ] && { echo "cool: bat $2 cpu $4 after $(( (i - 1) * 15 )) s"; exit 0; }
        sleep 15
    done
    echo "cool: still bat $2 cpu $4 after 20 min"
    ;;
backup)
    # np3.sh backup: the launcher files and the saves into cfg-backup-<stamp>/user-backup-<stamp>
    st=$(date +%m%d-%H%M%S)
    $ADB shell "cd $G && mkdir cfg-backup-$st user-backup-$st && for f in $FILES; do [ -f \$f ] && cp \$f cfg-backup-$st/; done; cp -a user/. user-backup-$st/ && echo backup $st && du -s user-backup-$st"
    echo "$st" > "$BASE/backup-stamp"
    ;;
restore)
    # np3.sh restore [stamp]: the app closed, the launcher's settings as the owner has them, the
    # files and saves from the backup (the newest one of np3.sh backup by default)
    st=${1:-$(cat "$BASE/backup-stamp")}
    $ADB shell am force-stop $P
    $ADB shell "echo > $G/pad.txt"
    $ADB shell am start -n $P/dev.unchartedg2.game.Launcher --es resolution 1280x720 --es fps_limit 30 --es touch_pad auto --es driver turnip --es extra_env "' '" > /dev/null; sleep 5
    $ADB shell am force-stop $P
    $ADB shell "cd $G && cp cfg-backup-$st/* . && cp -a user-backup-$st/. user/ && for f in \$(ls cfg-backup-$st); do cmp -s \$f cfg-backup-$st/\$f || echo \"\$f DIFFERS\"; done; diff -r user user-backup-$st > /dev/null && echo user-restored $st || echo USER-DIFFERS; pidof $P || echo not-running"
    ;;
install)
    # np3.sh install <zip>: a driver package through the launcher (prints the installed folders)
    $ADB push "$1" /sdcard/Download/np3-driver.zip > /dev/null
    $ADB shell am force-stop $P; sleep 1
    $ADB shell am start -n $P/dev.unchartedg2.game.Launcher --es driver_zip /sdcard/Download/np3-driver.zip > /dev/null; sleep 4
    $ADB shell am force-stop $P
    $ADB shell "ls /sdcard/Android/data/$P/files/drivers/ 2>/dev/null; run-as $P ls files/drivers 2>/dev/null" ;;
esac
