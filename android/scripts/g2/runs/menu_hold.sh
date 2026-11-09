#!/usr/bin/env bash
# menu_hold.sh <tag> <secs> [KEY=VALUE ...]: the game started through the launcher as
# continue_run.sh does, the collection menu navigated to U3, its intro skipped, then U3's main
# menu held for <secs> without input (the Nothing Phone 3 fell to 2 FPS there on main,
# 2026-11-10); screenshots at 10 s and the end, the frame rates of the hold, the log; then the
# owner's files put back. LIMIT (30), SIZE, TITLE (45), START (u1) as in continue_run.sh.
set -uo pipefail
. "$(dirname -- "${BASH_SOURCE[0]}")/g2.sh"
tag=$1 secs=$2; shift 2; extra="$*"
RUN=$OUT/runs; mkdir -p "$RUN"
LIMIT=${LIMIT:-30}

if game_running; then echo "$tag: the game is running - skipped"; exit 1; fi
if ! battery_ok; then echo "$tag: the battery is low and not charging - skipped"; exit 1; fi
if ! screen_awake; then echo "$tag: the screen does not wake - skipped"; exit 1; fi
stamp=$(date +%Y%m%d-%H%M%S)
back_up "$stamp" || exit 1
owner_env=$(owner_extra_env)
echo "owner's extra environment: [${owner_env}]"

env="BB_PAD_FILE=$G/pad.txt"
for kv in $extra; do env="$env\\\\n$kv"; done
$ADB shell am force-stop $P; sleep 1
$ADB shell "echo > $G/pad.txt; input keyevent KEYCODE_WAKEUP; [ -f $G/bbport.ini.c-bak ] || cp $G/bbport.ini $G/bbport.ini.c-bak"
$ADB shell am start -n $P/dev.unchartedg2.game.Launcher --es resolution ${SIZE:-1280x720} --es fps_limit $LIMIT --es touch_pad 0 --es extra_env "$env" > /dev/null; sleep 6
$ADB shell "am force-stop $P; sed -i -e 's/^fps_limit=.*/fps_limit=$LIMIT/' -e 's/^upscaler=.*/upscaler=off/' -e 's/^frame_gen=.*/frame_gen=off/' $G/bbport.ini"
$ADB shell am start -n $P/dev.unchartedg2.game.Launcher --ez play true > /dev/null

sleep 50
if [ "$($ADB shell "grep -c '' $G/bb-probe.log" | tr -d '\r')" -lt 10 ] || ! $ADB shell dumpsys power | grep -q 'mWakefulness=Awake'; then
    echo "$tag ($extra): no game after 50 s (the screen off?) - skipped"
    put_back "$stamp" "$(printf '%s' "$owner_env" | sed ':a;N;$!ba;s/\n/\\\\n/g')"
    exit 1
fi
press cross                             # the collection's title
sleep 10                                # the menu stands on the owner's last game: START
pos() { case "$1" in u1) echo 0;; u2) echo 1;; u3) echo 2;; esac; }
case $(( (2 - $(pos "${START:-u1}") + 3) % 3 )) in
    1) press lx=255 0.15; sleep 6;;     # one step right
    2) press lx=0 0.15; sleep 6;;       # one step left (the ring of three wraps)
esac
press cross
sleep ${TITLE:-45}; press cross         # the intro skipped: U3's main menu
sleep 2
f0=$($ADB shell "grep -c 'Frame rate' $G/bb-probe.log" | tr -d '\r')
sleep 10; $ADB exec-out screencap -p > "$RUN/$tag-menu1.png"
sleep $((secs - 10)); $ADB exec-out screencap -p > "$RUN/$tag-end.png"
pid=$($ADB shell pidof $P | tr -d '\r')
fps=$($ADB shell "grep 'Frame rate' $G/bb-probe.log" | tail -n +$((f0 + 1)) | awk '{for(i=1;i<=NF;i++) if($i=="FPS,") {printf "%s ", $(i-1); s+=$(i-1); n++}} END {if (n) printf "| mean %.1f over %d", s/n, n}')
$ADB shell "dmesg" > "$RUN/$tag.dmesg"
faults=$(awk -v pid="$pid" '/gmu: CP/ { k = ($0 ~ /BV opcode/) ? "BV" : ($0 ~ /Protected/) ? "PROT" : "BR" } index($0, "[" pid "]: cmdline") { c[k]++; n++ } END { printf "%d (BR %d, BV %d, PROT %d)", n, c["BR"], c["BV"], c["PROT"] }' "$RUN/$tag.dmesg")

$ADB shell am force-stop $P
$ADB shell "cp $G/bb-probe.log /sdcard/Download/run-$tag.log"; $ADB pull /sdcard/Download/run-$tag.log "$(cygpath -w "$RUN/$tag.log")" > /dev/null; $ADB shell rm -f /sdcard/Download/run-$tag.log
echo "$tag (LIMIT=$LIMIT $extra): menu hold $fps; GPU faults $faults" | tee -a "$RUN/summary.txt"
put_back "$stamp" "$(printf '%s' "$owner_env" | sed ':a;N;$!ba;s/\n/\\\\n/g')"
echo RUN-DONE
