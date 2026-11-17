#!/usr/bin/env bash
# oat_sym.sh <oat file (a /mnt/c path)> <offset, hex> [more offsets]: the Java methods of an Android
# oat file at code offsets, from its compressed symbol table (.gnu_debugdata: an xz'd small ELF with
# a symbol table). For a fault in the game's process inside Android's runtime (the port's crash
# report names the library and the offset: `Host fault (signal 11) in .../boot-framework.oat+0x...`;
# 2026-11-16: `SensorEventQueue.dispatchSensorEvent` with an interface table entry 0, a damaged
# object). Pull the oat first (`adb pull /system/framework/arm64/boot-framework.oat` into the
# scratchpad); `llvm-objdump -D --triple=aarch64 --start-address=0x.. --stop-address=0x.. -j .text`
# shows the code around the offset. Run inside WSL. The file is data: nothing of it is executed.
f=$1; shift
B=$(dirname "$(ls ~/android/android-ndk-*/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-objdump | tail -n 1)")
w=$(mktemp -d)
cp "$f" "$w/in.oat"
"$B/llvm-objcopy" --dump-section .gnu_debugdata="$w/mini.xz" "$w/in.oat" "$w/unused.oat" 2>&1 | head -3
xz -dc "$w/mini.xz" > "$w/mini.elf" 2> "$w/xz.err" || head -2 "$w/xz.err"
"$B/llvm-nm" -n -S "$w/mini.elf" > "$w/syms.txt" 2> "$w/nm.err"
echo "symbols: $(wc -l < "$w/syms.txt") $(head -c 200 "$w/nm.err")"
python3 -I - "$w/syms.txt" "$@" << 'EOF'
import sys
syms = []
for line in open(sys.argv[1], errors="replace"):
    p = line.rstrip("\n").split(" ", 3)
    try:
        a = int(p[0], 16)
    except ValueError:
        continue
    if len(p) == 4 and len(p[1]) == 16:
        syms.append((a & ~1, int(p[1], 16), p[3]))
    else:
        syms.append((a & ~1, 0, line.rstrip("\n").split(" ", 2)[-1]))
syms.sort()
for t in sys.argv[2:]:
    t = int(t, 16)
    best = None
    for s in syms:
        if s[0] <= t:
            best = s
        else:
            break
    print("0x%x: %s" % (t, ("%s (starts 0x%x, size 0x%x, +0x%x)" % (best[2], best[0], best[1], t - best[0])) if best else "?"))
EOF
rm -rf "$w"
