#!/usr/bin/env python3
"""timestats.py <tag.timestats> [layer substring]: the game layer's SurfaceFlinger TimeStats
(continue_run.sh SF_STATS=1): its frame counts (total, dropped, late acquire, bad desired present)
and the histograms' non-empty buckets (present to present: how long each frame stayed on the
screen; desired to present; acquire to present ...)."""
import re
import sys

path = sys.argv[1]
want = sys.argv[2] if len(sys.argv) > 2 else "unchartedandroid"
lines = open(path, encoding="utf-8", errors="replace").read().split("\n")
blocks, cur = [], None
for line in lines:
    if line.startswith("layerName = "):
        cur = [line]
        blocks.append(cur)
    elif cur is not None:
        if not line.strip() and len(cur) > 3 and cur[-1].strip() == "":
            cur = None
            continue
        cur.append(line)
found = [b for b in blocks if want in b[0]]
if not found:
    names = [b[0] for b in blocks]
    sys.exit("no layer with '%s'; layers: %s" % (want, "; ".join(n[12:80] for n in names[:20])))
for b in found:
    print(b[0][:140])
    i = 1
    while i < len(b):
        line = b[i].strip()
        if line.endswith("histogram is as below:") and i + 1 < len(b):
            buckets = re.findall(r"(\d+)ms=(\d+)", b[i + 1])
            nz = [(int(k), int(v)) for k, v in buckets if v != "0"]
            total = sum(v for _, v in nz)
            print("  %s (%d): %s" % (line.replace(" histogram is as below:", ""), total,
                                     " ".join("%dms=%d" % kv for kv in nz)))
            i += 2
            continue
        if "=" in line and not line.startswith("0ms="):
            print("  " + line)
        i += 1
