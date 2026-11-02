#!/usr/bin/env bash
# owner_session.sh start|status|stop: the owner's own play session on the G2 (they start the game
# themselves, with the variables you asked them to put into More environment), watched over adb:
# start - the dmesg time and the TU_/BB_ lines of env.txt; status/stop - the GPU faults of the
# game's processes since start (dmesg, by kind), the driver's notes and the frame rate lines of
# bb-probe.log; stop also pulls the log. Touches nothing of the owner's state.
# Pair it with a Monitor that polls the fault count every 20 s so a fault is reported at once.
set -uo pipefail
. "$(dirname -- "${BASH_SOURCE[0]}")/g2.sh"
SES=$OUT/owner; mkdir -p "$SES"
case "${1:-}" in
start)
    $ADB shell "dmesg | tail -1" | sed -E 's/^\[ *([0-9.]+)\].*/\1/' > "$SES/t0"
    echo "dmesg time at start: $(cat "$SES/t0")"
    echo "env.txt now (the launcher rewrites it on Play):"
    $ADB shell "grep -E '^(TU_|BB_)' $G/env.txt" | tr -d '\r' | grep -v -E "$LAUNCHER_KEYS" | sed 's/^/  /' || echo "  (no extra lines)"
    ;;
status|stop)
    t0=$(cat "$SES/t0" 2>/dev/null || echo 0)
    pid=$($ADB shell pidof $P | tr -d '\r'); echo "game pid: ${pid:-none}"
    $ADB shell "grep -E '^TU_' $G/env.txt" | tr -d '\r' | sed 's/^/env.txt: /'
    $ADB shell dmesg > "$SES/dmesg.txt"
    awk -v t0="$t0" -F'[][]' '{t=$2+0} /gmu: CP/ { k = ($0 ~ /BV opcode/) ? "BV" : ($0 ~ /Protected/) ? "PROT" : "BR" }
        t > t0 && /cmdline/ && /unchartedandroid/ { c[k]++; n++ } END { printf "GPU faults since start: %d (BR %d, BV %d, PROT %d)\n", n, c["BR"], c["BV"], c["PROT"] }' "$SES/dmesg.txt"
    awk -v t0="$t0" -F'[][]' '{t=$2+0} t > t0 && /gmu: CP/ {printf "%.0f ", t - t0}' "$SES/dmesg.txt" | sed 's/^/fault times (s after start): /'; echo
    $ADB shell "grep -a -E '^GPU: Turnip|^tu: |^tu/kgsl|GPU fault|recover|device lost' $G/bb-probe.log" | tr -d '\r' | sort | uniq -c | sort -rn | head -8 | cut -c1-170
    $ADB shell "grep -a 'Frame rate' $G/bb-probe.log" | awk '{for(i=1;i<=NF;i++) if($i=="FPS,") {s+=$(i-1); n++; if ($(i-1)+0 < lo || n==1) lo=$(i-1)}} END {if (n) printf "frame rate: mean %.1f over %d lines, lowest %s\n", s/n, n, lo}'
    if [ "$1" = stop ]; then
        $ADB shell "cp $G/bb-probe.log /sdcard/Download/owner.log" && $ADB pull /sdcard/Download/owner.log "$(cygpath -w "$SES/owner-$(date +%H%M).log")" > /dev/null && $ADB shell rm -f /sdcard/Download/owner.log && echo "log saved to $SES"
    fi
    ;;
*) echo "usage: owner_session.sh start|status|stop"; exit 1;;
esac
