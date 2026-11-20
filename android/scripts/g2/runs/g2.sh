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

# owner_game: the game the owner played last (u1, u2 or u3) - the newest of the three games' save
# files on the device, their times kept by the restores. The collection's menu stands on that
# game after every start, and the routes count their steps from it: the run scripts take it as
# START unless START is given. Call it while the owner's saves are in place (before a run pushes
# its own). A game the owner started without reaching a checkpoint is not seen: hence wrong_game.
owner_game() {
    local d=$G/user/savedata/1/CUSA02344 n
    n=$($ADB shell "ls -t $d/SAVEDATA010/USR-DATA $d/SAVEDATA020/USR-DATA $d/SAVEDATA030/USR-DATA 2> /dev/null | head -n 1" |
        tr -d '\r' | sed -n -E 's/.*SAVEDATA0([123])0.*/u\1/p')
    echo "${n:-u1}"
}
# wrong_game <log> <game>: a note for a run's line when its log belongs to another game than the
# one asked for (the first of a game's data files it opened: /app0/u<n>data). On 2026-11-16 a run
# for U3 started without START walked U1's menus for its whole time and reported 30 FPS.
wrong_game() {
    local ran
    ran=$(grep -a -m 1 -o '/app0/u[123]data' "$1" 2> /dev/null | sed -E 's/.*(u[123])data/\1/')
    [ -n "$ran" ] && [ "$ran" != "$2" ] && echo "WRONG GAME: $ran ran, not $2 (START=${START:-?}: the owner's last game?)"
    return 0
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

# battery_ok: the charge is at least BATTERY_MIN percent (20), or a charger (AC) feeds the device.
# The PC's USB does not count: the status says Charging, yet the game takes ~2 A out of the
# battery (2026-11-18: 47% -> 9% over an A/B of eight runs).
battery_ok() {
    local cap ac
    cap=$($ADB shell cat /sys/class/power_supply/battery/capacity | tr -d '\r')
    ac=$($ADB shell dumpsys battery | grep -c 'AC powered: true')
    [ "${cap:-0}" -ge "${BATTERY_MIN:-20}" ] || [ "${ac:-0}" -ge 1 ]
}

# device_here: adb still answers (after a reset or a dropped link every later adb call comes back
# empty, and a run would report a dead game and "put back" nothing).
device_here() { [ "$($ADB get-state 2> /dev/null | tr -d '\r')" = device ]; }

# run_lock <tag>: one run at a time on the device. A second run script that starts while one is
# in progress refuses: on 2026-11-16 two chains ran a run each for some minutes by mistake, and
# the second backed the first one's save and variables up as "the owner's" and put them back at
# its end (found by the final check, restored from the last good pair). The lock is a directory
# under $OUT with the holder's process id; a holder that is gone is ignored. Sets the script's
# EXIT trap: the lock removed, the live record stopped.
run_locked=0
run_cleanup() {
    kwatch_stop
    [ $run_locked = 1 ] && rm -rf "$OUT/.run-lock"
    run_locked=0
}
run_lock() {
    local dir=$OUT/.run-lock pid
    mkdir -p "$OUT"
    if ! mkdir "$dir" 2> /dev/null; then
        pid=$(cat "$dir/pid" 2> /dev/null)
        if [ -n "$pid" ] && kill -0 "$pid" 2> /dev/null; then
            echo "$1: another run is in progress (process $pid) - skipped"; return 1
        fi
        rm -rf "$dir"; mkdir "$dir" 2> /dev/null || return 1
    fi
    echo $$ > "$dir/pid"
    run_locked=1
    trap run_cleanup EXIT
}

# kwatch_start <prefix> / kwatch_stop: a run's kernel log, temperatures and game log streamed to
# the PC while it goes - <prefix>.klive (the kernel log's new lines every second, without the AHB
# bus error flood of register 0x8e07; it opens with the last 64 KiB from before), <prefix>.temps
# (every 5 s: the hottest CPU, GPU, board and PMIC zones and the DDR's in millidegrees, the
# battery's temperature in tenths of a degree, its charge, current and voltage) and <prefix>.glive
# (bb-probe.log as the game writes it). The G2 has reset in runs and recorded nothing each time
# (boot reason "reboot", no last kernel log, no thermal history, the game log's last 31 KiB zeros:
# findings 2026-11-15), so what the PC holds by then is all there is. A panic's own lines may not
# make it out; what led to it does. The shell user cannot follow the kernel log here (/dev/kmsg is
# root's, `dmesg -w` falls back to one dump, logd keeps no kernel buffer on a user build): hence
# the poll of `dmesg -s`, ~15 ms of one core a second. Read-only on the device; the streams end
# with adb. KWATCH=0 in the environment turns them off. With them goes the guard (kwatch_guard
# below): the game is stopped when a sample is too hot.
KWATCH_KERNEL=$(cat <<'EOF'
l=0
while :; do
x=$(dmesg -s 65536 | awk -v l="$l" '/^\[/ { t = $0; sub(/^\[ */, "", t); sub(/\].*/, "", t); if (t + 0 > l + 0 && $0 !~ /CP: AHB bus error|gen7_err_callback|avc: .*scontext=u:r:shell:s0|audit: (rate limit exceeded|audit_lost)/) print }')
if [ -n "$x" ]; then print -r -- "$x"; l=$(print -r -- "$x" | tail -n 1 | sed -E 's/^\[ *([0-9.]+)\].*/\1/'); fi
sleep 1
done
EOF
)
# The game's log from where it stands (f=<file> in front): the launcher starts it anew at the
# game's start, and a `tail -F` would wait for the new one to outgrow the old.
KWATCH_GAMELOG=$(cat <<'EOF'
o=$(stat -c %s $f 2>/dev/null) || o=0
while :; do
s=$(stat -c %s $f 2>/dev/null) || s=0
[ "$s" -lt "$o" ] && o=0
if [ "$s" -gt "$o" ]; then tail -c +$((o + 1)) $f | head -c $((s - o)); o=$s; fi
sleep 1
done
EOF
)
# One sample: "<time> cpu C gpu G board S pmic P ddr D battery B charge Q current I voltage U hot <zone>"
# (the hottest of the cpu*, gpuss*, sys-therm* and pm*_tz zones, millidegrees; the battery in tenths
# of a degree, percent, microamperes - negative while it discharges - and microvolts; the name of
# the hottest CPU zone).
KWATCH_SAMPLE='c=0; n=-; g=0; s=0; p=0; d=0
for z in /sys/class/thermal/thermal_zone*; do
{ read t < $z/type && read v < $z/temp; } 2>/dev/null || continue
case $t in
cpu*) [ $v -gt $c ] && { c=$v; n=$t; };;
gpuss*) [ $v -gt $g ] && g=$v;;
sys-therm*) [ $v -gt $s ] && s=$v;;
pm*_tz) [ $v -gt $p ] && p=$v;;
ddr) d=$v;;
esac
done
read b < /sys/class/power_supply/battery/temp; read q < /sys/class/power_supply/battery/capacity
read i < /sys/class/power_supply/battery/current_now; read u < /sys/class/power_supply/battery/voltage_now
echo "$(date +%H:%M:%S) cpu $c gpu $g board $s pmic $p ddr $d battery $b charge $q current $i voltage $u hot $n"'
thermal_sample() { $ADB shell "$KWATCH_SAMPLE" 2> /dev/null | tr -d '\r'; }

# cool_wait: before a run, until the hottest CPU zone is at COOL_CPU (60 C) or less, the board at
# COOL_BOARD (42 C) or less and the hottest PMIC zone at COOL_PMIC (60 C) or less, COOL_MAX seconds
# at most (300); says what it waited for. A run from a warm device starts its play that much nearer
# the limits: U1's chapter 9 took a CPU zone from 59 to 99 C in ten seconds of play and held 94-101
# C (2026-11-15, the device cool before it); on a wall charger U3's play took the PMIC 74 -> 90 C,
# the guard's stop (2026-11-17: see charge_wait).
cool_wait() {
    local t0=$SECONDS line c s p
    while :; do
        line=$(thermal_sample < /dev/null)
        c=$(echo "$line" | awk '{ print $3 }'); s=$(echo "$line" | awk '{ print $7 }'); p=$(echo "$line" | awk '{ print $9 }')
        [ -n "$c" ] || return 1
        [ "$c" -le "${COOL_CPU:-60000}" ] && [ "${s:-0}" -le "${COOL_BOARD:-42000}" ] && [ "${p:-0}" -le "${COOL_PMIC:-60000}" ] && break
        if [ $((SECONDS - t0)) -ge "${COOL_MAX:-300}" ]; then echo "still warm after ${COOL_MAX:-300} s: cpu $c board $s pmic $p"; return 1; fi
        sleep 10
    done
    [ $((SECONDS - t0)) -lt 10 ] || echo "cooled for $((SECONDS - t0)) s: cpu $c board $s pmic $p"
}

# charge_wait [max secs]: until the battery takes CHARGE_MAX_MA (1500 mA) or less and the hottest
# PMIC zone is at CHARGE_PMIC (55 C) or less, max secs at most (900); says what it waited for. A
# wall charger feeds the battery 3-4 A below ~90% and heats the PMIC: U3's play then took it 74 ->
# 90 C and the guard stopped the run (2026-11-17), while full or off the charger the same run peaks
# at 67-70 C. build_pairs.sh waits so before each run (an hour at most before the first: the
# battery fills).
charge_wait() {
    local t0=$SECONDS max=${1:-900} line p i q
    while :; do
        line=$(thermal_sample < /dev/null)
        p=$(echo "$line" | awk '{ print $9 }'); i=$(echo "$line" | awk '{ print $17 }'); q=$(echo "$line" | awk '{ print $15 }')
        [ -n "$p" ] || return 0
        [ "$p" -le "${CHARGE_PMIC:-55000}" ] && [ "${i:-0}" -le "$(( ${CHARGE_MAX_MA:-1500} * 1000 ))" ] && break
        if [ $((SECONDS - t0)) -ge "$max" ]; then
            echo "still charging after $max s: pmic $((p / 1000)) C, current $((i / 1000)) mA, charge $q% - the run goes"
            return 1
        fi
        sleep 30
    done
    [ $((SECONDS - t0)) -lt 30 ] || echo "waited $((SECONDS - t0)) s for the charge: pmic $((p / 1000)) C, current $((i / 1000)) mA, charge $q%"
}

# kwatch_guard <prefix>: stops the game when a sample of <prefix>.temps has a CPU zone at HOT_CPU
# (108 C; the kernel's own throttling starts at 110, there is no critical trip on those zones and
# the G2 has reset without a record), a PMIC at HOT_PMIC (90 C; its passive trip is 95, critical
# 115) or the battery at HOT_BATTERY (48 C); the sample goes to <prefix>.hot. HOT_STOP=0: off.
too_hot() {   # too_hot <sample line>
    echo "$1" | awk -v c="${HOT_CPU:-108000}" -v p="${HOT_PMIC:-90000}" -v b="${HOT_BATTERY:-480}" \
        '{ hot = ($3 + 0 >= c || $9 + 0 >= p || $13 + 0 >= b) } END { exit hot ? 0 : 1 }'
}
kwatch_guard() {
    local line
    while sleep 5; do
        line=$(tail -n 1 "$1.temps" 2> /dev/null)
        too_hot "$line" || continue
        echo "$line" > "$1.hot"
        $ADB shell am force-stop $P
        break
    done
}
# hot_stop_note <prefix>: "cpu 109 pmic 61 battery 30 C at 22:06:13" when the guard stopped the run.
hot_stop_note() {
    [ -s "$1.hot" ] && awk '{ printf "cpu %.0f pmic %.0f battery %.0f C at %s", $3 / 1000, $9 / 1000, $13 / 10, $1 }' "$1.hot"
}
kwatch_pids=
kwatch_start() {
    [ "${KWATCH:-1}" = 1 ] || return 0
    rm -f "$1.hot"
    $ADB exec-out "$KWATCH_KERNEL" > "$1.klive" 2> /dev/null < /dev/null &
    kwatch_pids=$!
    $ADB exec-out "while :; do $KWATCH_SAMPLE; sleep 5; done" > "$1.temps" 2> /dev/null < /dev/null &
    kwatch_pids="$kwatch_pids $!"
    $ADB exec-out "f=${2:-$G/bb-probe.log}; $KWATCH_GAMELOG" > "$1.glive" 2> /dev/null < /dev/null &
    kwatch_pids="$kwatch_pids $!"
    if [ "${HOT_STOP:-1}" = 1 ]; then kwatch_guard "$1" > /dev/null 2>&1 < /dev/null & kwatch_pids="$kwatch_pids $!"; fi
}
kwatch_stop() {
    [ -n "$kwatch_pids" ] && kill $kwatch_pids 2> /dev/null
    kwatch_pids=
}
# kwatch_hottest <prefix>: "cpu 71 gpu 64 board 43 pmic 55 battery 37 C" - the highest of each.
kwatch_hottest() {
    awk '{ for (i = 2; i < NF; i += 2) if (!($i in m) || $(i + 1) + 0 > m[$i]) m[$i] = $(i + 1) + 0; n++ }
        END { if (n) printf "cpu %.0f gpu %.0f board %.0f pmic %.0f battery %.0f C", m["cpu"] / 1000, m["gpu"] / 1000, m["board"] / 1000, m["pmic"] / 1000, m["battery"] / 10; else printf "no samples" }' "$1.temps" 2> /dev/null
}

# kernel_mark: the kernel time of the log's last line, for kernel_events.
kernel_mark() { $ADB shell "dmesg | tail -1" | tr -d '\r' | sed -E 's/^\[ *([0-9.]+)\].*/\1/'; }

# kernel_events <dmesg file> <kernel time>: the GPU's events in the kernel log after that time -
# "GPU faults N (BR b, BV v, PROT p), page faults F, hangs H". By the time window, not by the
# game's process id: a fault in the collection menu's process of the same run is the run's too,
# and a page fault names no command line (2026-11-15: three of a sweep's four CP opcode errors
# and a burst of ten page faults went uncounted by the process id).
kernel_events() {
    awk -v t="${2:-0}" '{ ts = $0; sub(/^\[ */, "", ts); sub(/\].*/, "", ts) } ts + 0 <= t + 0 { next }
        /gmu: CP/ { k = ($0 ~ /BV opcode/) ? "BV" : ($0 ~ /Protected/) ? "PROT" : "BR"; c[k]++; n++ }
        /GPU PAGE FAULT/ { pf++ } /GPU hang detected/ { h++ }
        END { printf "GPU faults %d (BR %d, BV %d, PROT %d), page faults %d, hangs %d", n, c["BR"], c["BV"], c["PROT"], pf, h }' "$1"
}
