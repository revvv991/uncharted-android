"""GPU idle between submissions from BB_GPU_PROFILE blocks of a bb-probe.log.
python gprof_idle.py <log> [last N blocks, default 10] [top labels, default 8]
Per block: 'GPU profile: X ms/frame over N frames (M ms/s), ...' then 'ms/frame count/frame (label)'.
Prints the mean ms/s (= share of GPU time, per mille) of the idle label and of the top labels."""
import re
import sys
from collections import defaultdict

path = sys.argv[1]
last = int(sys.argv[2]) if len(sys.argv) > 2 else 10
top = int(sys.argv[3]) if len(sys.argv) > 3 else 8
head = re.compile(r"GPU profile: ([\d.]+) ms/frame over (\d+) frames \((\d+) ms/s\)")
row = re.compile(r"^\s+([\d.]+) ms/frame\s+([\d.]+)/frame\s+(.*)$")
blocks = []
cur = None
for line in open(path, encoding="utf-8", errors="replace"):
    m = head.search(line)
    if m:
        cur = {"ms_frame": float(m.group(1)), "frames": int(m.group(2)), "ms_s": int(m.group(3)), "rows": {}}
        blocks.append(cur)
        continue
    if cur is not None:
        r = row.match(line)
        if r:
            cur["rows"][r.group(3).strip()] = (float(r.group(1)), float(r.group(2)))
        elif line.strip():
            cur = None
full = [b for b in blocks if b["ms_s"] >= 900 and b["ms_frame"] > 0][-last:]
if not full:
    print("no full-coverage profile blocks (%d blocks in all)" % len(blocks))
    sys.exit(0)
share = defaultdict(float)
count = defaultdict(float)
for b in full:
    fps = b["ms_s"] / b["ms_frame"]  # profiler frames per second
    for label, (ms, n) in b["rows"].items():
        share[label] += ms * fps / len(full)
        count[label] += n * fps / len(full)
idle = [k for k in share if "between submissions" in k]
print("%d blocks; GPU idle between submissions: %s" % (
    len(full), ", ".join("%.0f ms/s (%.1f/s)" % (share[k], count[k]) for k in idle) or "no label"))
for label in sorted(share, key=share.get, reverse=True)[:top]:
    print("  %6.0f ms/s %7.1f/s  %s" % (share[label], count[label], label))
