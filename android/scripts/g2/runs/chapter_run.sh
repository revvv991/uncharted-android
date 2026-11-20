#!/usr/bin/env bash
# chapter_run.sh <tag> <u1|u2|u3> <chapter> <secs> [KEY=VALUE ...]: one run of a chapter started
# from the game's Chapter Select - the game started through the launcher as continue_run.sh does,
# the main menu's Chapters, the chapter picked in the carousel, Start chapter, a movie that plays
# after the load skipped from the pause menu, then <secs> seconds of the camera turning (INPUT,
# rx=200): screenshots, the frame rates, the GPU faults and hangs, the log's crash, stall and
# unknown-layout lines; then the owner's files, saves and settings put back. Refuses while the
# game runs. One process a chapter: a blind walk through the pause menu back to the main menu
# loses its place (2026-11-15). For every chapter of a game: chapter_sweep.sh.
#   SAVES    a folder with a save of <game> that has the chapter unlocked (a finished game's has
#            them all; SAVEDATA0n0, SAVEDATA0nP and their .sce_sys, as for continue_run.sh). Without
#            it the owner's own save and its chapters. A reporter's saves stay outside the repository.
#   START    the game the OWNER played last (owner_game by default): see continue_run.sh
#   LIMIT    the frame limit (30; 0 = off), SIZE (1280x720), TITLE (45 s to the game's title screen)
#   LOAD     seconds from Start chapter to the first look (30: U2's chapter 2 plays after 7)
#   SKIP_MOVIES=0  leave a movie that plays after the load alone (it is skipped by default)
#   WAIT_MOVIES=1  wait such a movie out instead, no key pressed, up to MOVIE_MAX seconds (420):
#            for the chapters whose skip lands in a loading screen or loses the first scene
#   SHOTS    a screenshot every SHOTS seconds of the play into <tag>-tNNN.png (instead of -mid.png):
#            for a picture that changes during the run
#   DOWNS    the main menu's steps down from Continue to Chapters (u1: 3, u2 and u3: 1)
# Outputs in $OUT/chapters: <tag>-menu.png (the chapter picked, before the start), <tag>-game.png
# (the play's first second), <tag>-mid.png, <tag>-end.png, <tag>.log, <tag>.dmesg, the live record
# <tag>.klive and <tag>.temps (g2.sh's kwatch_start: the kernel log and the temperatures as the run
# went), and a line in summary.txt: "<tag> <game> ch<N>: <how it went> | <FPS of every 5 s...> |
# mean X min Y over N | draws/s D | files: <the first level files opened> | GPU faults N (BR, BV,
# PROT), page faults F, hangs H | hottest: cpu, gpu, board, pmic, battery | flags: <log lines>".
# Before the run the device cools (g2.sh's cool_wait: COOL_CPU, COOL_BOARD, COOL_MAX); in it the
# guard stops the game when a sample is too hot (kwatch_guard: HOT_CPU, HOT_PMIC, HOT_BATTERY;
# HOT_STOP=0 off) and the line says STOPPED BY THE GUARD.
# The chapter's name is in <tag>-menu.png; the level files name its place. With
# BB_SKIN_BATCH_STATS=1 among the variables the line also says how much of the skinning went
# through the batch, its any-layout shaders and unbatched.
# When adb stops answering in the run (the G2 reset in one on 2026-11-15) the line says THE DEVICE
# WENT AWAY, the run ends with RUN-LOST and without RUN-DONE (a sweep stops there), the live record
# keeps the kernel's and the game's last lines (<tag>.klive, <tag>.glive, <tag>.temps), and the
# owner's files wait in the backup pair it names: restore_owner.sh when the device is back.
set -uo pipefail
. "$(dirname -- "${BASH_SOURCE[0]}")/g2.sh"
[ $# -ge 4 ] || { echo "usage: chapter_run.sh <tag> <u1|u2|u3> <chapter> <secs> [KEY=VALUE ...]"; exit 1; }
tag=$1 game=$2 chapter=$3 secs=$4; shift 4; extra="$*"
case "$game" in u1) n=1; downs=3;; u2) n=2; downs=1;; u3) n=3; downs=1;; *) echo "game: u1, u2 or u3"; exit 1;; esac
downs=${DOWNS:-$downs}
RUN=$OUT/chapters; mkdir -p "$RUN"
LIMIT=${LIMIT:-30}
L=$G/bb-probe.log

if ! device_here; then echo "$tag: no device (adb) - skipped"; exit 1; fi
run_lock "$tag" || exit 1               # one run at a time; its EXIT trap stops the live record
START=${START:-$(owner_game)}       # the owner's last game: where the menu stands
if game_running; then echo "$tag: the game is running - skipped"; exit 1; fi
if ! battery_ok; then echo "$tag: the battery is below ${BATTERY_MIN:-20}% with no charger (AC) - skipped"; exit 1; fi
if ! screen_awake; then echo "$tag: the screen does not wake - skipped"; exit 1; fi
cooled=$(cool_wait)                     # a warm device starts its play that much nearer the limits
# The wait can take minutes: the owner may have started the game meanwhile.
if game_running; then echo "$tag: the game was started during the cool-down - skipped"; exit 1; fi
stamp=$(date +%Y%m%d-%H%M%S)
back_up "$stamp" || exit 1
owner_env=$(owner_extra_env)
restore() { put_back "$stamp" "$(printf '%s' "$owner_env" | sed ':a;N;$!ba;s/\n/\\\\n/g')"; }
# lost: adb answers no more - nothing can be read or put back; say where the last lines are.
lost() {
    kwatch_stop
    echo "$tag $game ch$chapter: THE DEVICE WENT AWAY (adb lost by $(date +%H:%M:%S): a reset?) | last sample: $(tail -n 1 "$RUN/$tag.temps" 2> /dev/null) | hottest: $(kwatch_hottest "$RUN/$tag") | the kernel's and the game's last lines: $tag.klive, $tag.glive | the owner's files NOT put back: restore_owner.sh $stamp" | tee -a "$RUN/summary.txt"
    echo RUN-LOST; exit 2
}
if [ -n "${SAVES:-}" ]; then
    title=$($ADB shell "ls $G/user/savedata/1/" | tr -d '\r' | head -1)
    for d in SAVEDATA0${n}0 SAVEDATA0${n}0.sce_sys SAVEDATA0${n}P SAVEDATA0${n}P.sce_sys; do
        if [ -z "$title" ] || ! $ADB push "$(cygpath -w "$SAVES/$d")" "$G/user/savedata/1/$title/" > /dev/null; then
            echo "$tag $game ch$chapter: the save's $d did not go to the device - skipped"; restore; exit 1
        fi
    done
fi

env="BB_PAD_FILE=$G/pad.txt\\\\nBB_FRAME_STATS=1"
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
if [ "$($ADB shell "grep -c '' $L" | tr -d '\r')" -lt 10 ] || ! $ADB shell dumpsys power | grep -q 'mWakefulness=Awake'; then
    echo "$tag $game ch$chapter: no game after 50 s (the screen off?) - skipped"; restore; exit 1
fi
press cross                             # the collection's title
sleep 10                                # the menu stands on the owner's last game: START
pos() { case "$1" in u1) echo 0;; u2) echo 1;; u3) echo 2;; esac; }
case $(( ($(pos "$game") - $(pos "${START:-u1}") + 3) % 3 )) in
    1) press lx=255 0.15; sleep 6;;     # one step right
    2) press lx=0 0.15; sleep 6;;       # one step left (the ring of three wraps)
esac
press cross
sleep ${TITLE:-45}; press cross         # the game's title screen
sleep 10                                # its main menu, Continue chosen
i=0; while [ $i -lt "$downs" ]; do press down 0.25; sleep 1; i=$((i + 1)); done
press cross; sleep 3                    # Chapters: the chapter row chosen, chapter 1 shown
press cross; sleep 2                    # the carousel (it does not wrap)
i=1; while [ $i -lt "$chapter" ]; do press right 0.25; sleep 0.8; i=$((i + 1)); done
sleep 1; press cross; sleep 2           # the chapter taken: back on the chapter's screen
i=0; while [ $i -lt 3 ]; do press down 0.25; sleep 0.8; i=$((i + 1)); done   # Start chapter
$ADB exec-out screencap -p > "$RUN/$tag-menu.png"
l0=$($ADB shell "grep -c '' $L" | tr -d '\r')
press cross                             # Start chapter
sleep "${LOAD:-30}"

# "<Videodec2 decode lines> <1 while the game lives>": a movie that plays adds a line every 5 s.
state() {
    $ADB shell "echo \$(grep -a -c 'Videodec2: [0-9]* decodes' $L) \$(pidof $P > /dev/null && echo 1 || echo 0)" | tr -d '\r'
}
movies=0; died=0; movie_left=0; waited=0
while :; do
    read -r d1 alive <<< "$(state)"; [ "${alive:-0}" = 1 ] || { died=1; break; }
    sleep 6
    read -r d2 alive <<< "$(state)"; [ "${alive:-0}" = 1 ] || { died=1; break; }
    [ "${d2:-0}" -gt "${d1:-0}" ] || break
    if [ "${WAIT_MOVIES:-0}" = 1 ]; then
        waited=$((waited + 6)); movies=1
        [ $waited -lt "${MOVIE_MAX:-420}" ] && continue
        movie_left=1; break
    fi
    if [ "${SKIP_MOVIES:-1}" != 1 ] || [ $movies -ge 4 ]; then movie_left=1; break; fi
    press options; sleep 3              # the pause menu of a movie: Skip cinematic is its third line
    press down 0.25; sleep 1.5; press down 0.25; sleep 1.5; press cross
    movies=$((movies + 1)); sleep "${AFTER_MOVIE:-15}"
done

[ "${WAIT_MOVIES:-0}" = 1 ] && [ $movies -gt 0 ] && [ $died = 0 ] && sleep 8    # the scene after the movie
f0=$($ADB shell "grep -c 'Frame rate' $L" | tr -d '\r')
p0=$($ADB shell "grep -c 'Draw pipe:' $L" | tr -d '\r')
if [ $died = 0 ]; then
    $ADB exec-out screencap -p > "$RUN/$tag-game.png"
    $ADB shell "echo '${INPUT-rx=200}' > $G/pad.txt"
    if [ -n "${SHOTS:-}" ]; then
        t=0
        while [ $((t + SHOTS)) -le "$secs" ]; do
            sleep $((SHOTS - 1)); t=$((t + SHOTS))
            $ADB exec-out screencap -p > "$RUN/$tag-t$(printf %03d $t).png"
        done
    else
        sleep $((secs / 2)); $ADB exec-out screencap -p > "$RUN/$tag-mid.png"
        sleep $((secs - secs / 2))
    fi
    $ADB shell "echo > $G/pad.txt"
    $ADB exec-out screencap -p > "$RUN/$tag-end.png"
    $ADB shell pidof $P > /dev/null || died=2
fi
device_here || lost
fps=$($ADB shell "grep 'Frame rate' $L" | tail -n +$((f0 + 1)) | awk '{for(i=1;i<=NF;i++) if($i=="FPS,") {v=$(i-1); printf "%s ", v; s+=v; n++; if (m=="" || v+0<m+0) m=v}} END {if (n) printf "| mean %.1f min %.1f over %d", s/n, m, n}')
draws=$($ADB shell "grep -a -o -E 'Draw pipe: [0-9]+ draws/s' $L" | tr -d '\r' | tail -n +$((p0 + 1)) | cut -d' ' -f3 | sort -n | awk '{v[NR]=$1} END {if (NR) print v[int((NR+1)/2)]; else print "?"}')
$ADB shell "dmesg" > "$RUN/$tag.dmesg"
kev=$(kernel_events "$RUN/$tag.dmesg" "$kmark")
kwatch_stop
hot=$(kwatch_hottest "$RUN/$tag")

$ADB shell am force-stop $P
$ADB shell "cp $L /sdcard/Download/run-$tag.log"; $ADB pull /sdcard/Download/run-$tag.log "$(cygpath -w "$RUN/$tag.log")" > /dev/null; $ADB shell rm -f /sdcard/Download/run-$tag.log
[ -s "$RUN/$tag.log" ] && rm -f "$RUN/$tag.glive"       # the whole log is here: its live copy is not needed
since() { tail -n +$((l0 + 1)) "$RUN/$tag.log" 2>/dev/null; }
flags=$(since | grep -a -E '^(Fault|Guest fault|Host fault)|ASSERT|STALL:|STOP:|Device lost|a layout the batch does not know' | cut -c1-110 | awk '!seen[$0]++' | head -3 | paste -sd'~' - | sed 's/~/ ~ /g')
files=$(since | grep -a -o -E 'open\(/app0/[^)]*\.bin\)' | sed -E 's|.*/||; s|\.bin\)||; s|-ingame[0-9]+$||' | awk '!seen[$0]++' | head -4 | paste -sd' ' -)
# With BB_SKIN_BATCH_STATS=1: the most skinning dispatches a second, by the any-layout shaders, unbatched.
skin=$(since | grep -a -E 'Skin batch: [0-9]+ dispatches/s' | sed -E 's/Skin batch: ([0-9]+) dispatches\/s \(([0-9]+)\/s by the any-layout shaders\).* ([0-9]+)\/s regular.*/\1 \2 \3/' | awk '{ if ($1 + 0 > a) a = $1; if ($2 + 0 > b) b = $2; if ($3 + 0 > c) c = $3; n++ } END { if (n) printf "skin batch up to %d/s (any-layout %d/s, regular %d/s) | ", a, b, c }')
case $died in
    1) how="THE GAME DIED before the play";;
    2) how="THE GAME DIED in the play";;
    *) how="played";;
esac
if [ "${WAIT_MOVIES:-0}" = 1 ]; then [ $movies -gt 0 ] && how="$how, a movie waited out (~$((waited + 6)) s)"
else [ $movies -gt 0 ] && how="$how, $movies movie(s) skipped"; fi
[ $movie_left = 1 ] && how="$how, A MOVIE PLAYS (not skipped)"
[ "$draws" != "?" ] && [ "$draws" -lt 2000 ] && how="$how, FEW DRAWS (a loading or black screen?)"
stopped=$(hot_stop_note "$RUN/$tag"); [ -n "$stopped" ] && how="STOPPED BY THE GUARD: TOO HOT ($stopped)"
wrong=$(wrong_game "$RUN/$tag.log" "$game"); [ -n "$wrong" ] && how="$wrong; $how"
echo "$tag $game ch$chapter: $how | $fps | draws/s $draws | files: ${files:-none} | ${skin}$kev | hottest: $hot${cooled:+ ($cooled)} | flags: ${flags:-none}" | tee -a "$RUN/summary.txt"
restore
echo RUN-DONE
