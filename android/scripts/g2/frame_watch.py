#!/usr/bin/env python3
"""frame_watch.py <log> [secs] [--mp4 <recording>]: U1's render frames from BB_U1_FRAME_WATCH=1 (`Frame
watch` lines: a new frame number seen by the 0.5 ms poller, the end-of-pipe ticks of frame-3 - the
one FrameBegin recycles and waits for - and of frame-1) over the last <secs> seconds (90).

Prints the game's frame intervals (its own cadence: the time it moves the world by, if it measures
its frame from FrameBegin to FrameBegin), how many frames began before frame-3's tick was there
(FrameBegin then waits for the GPU side), and with --mp4 the correlation of the frame intervals
with the picture's steps on the screen (motion_vs_trace.py's measure, matched by time)."""
import re
import statistics
import sys

PAT = re.compile(r"Frame watch: ([\d.]+) ([ab]) frame (\d+) \(\+(-?\d+)\), eop tick of frame-3 (0x[0-9a-f]+|0), "
                 r"frame-1 (0x[0-9a-f]+|0)")


def main():
    path = sys.argv[1]
    secs = float(sys.argv[2]) if len(sys.argv) > 2 and not sys.argv[2].startswith("--") else 90.0
    rows = {"a": [], "b": []}
    for line in open(path, encoding="utf-8", errors="replace"):
        m = PAT.search(line)
        if m:
            rows[m.group(2)].append((float(m.group(1)), int(m.group(3)), int(m.group(4)), int(m.group(5), 16),
                                     int(m.group(6), 16)))
    for which, r in rows.items():
        if len(r) < 10:
            print(f"pointer {which}: {len(r)} frames")
            continue
        end = r[-1][0]
        r = [x for x in r if x[0] >= end - secs * 1000]
        dt = [b[0] - a[0] for a, b in zip(r, r[1:]) if b[2] == 1]
        skipped = sum(1 for x in r[1:] if x[2] != 1)
        no_tick = sum(1 for x in r if x[3] == 0)
        print(f"pointer {which}: {len(r)} frames over the last {secs:.0f} s; frame-number steps other than 1: {skipped}")
        q = sorted(dt)
        pick = lambda p: q[min(len(q) - 1, int(p / 100 * len(q)))]
        print(f"  frame intervals: median {statistics.median(dt):.1f} ms, stdev {statistics.pstdev(dt):.1f}, "
              f"p5 {pick(5):.1f}, p95 {pick(95):.1f}, within 2 ms of 33.3: "
              f"{100 * sum(1 for x in dt if abs(x - 33.33) <= 2) / len(dt):.0f}%")
        print(f"  frames begun before frame-3's end-of-pipe tick: {no_tick} of {len(r)}")
        ticks = [x[3] for x in r if x[3]]
        if ticks:
            print(f"  tick values (frame-3), first few: {[hex(t) for t in ticks[:4]]}")
        if "--list" in sys.argv:
            for a, b in list(zip(r, r[1:]))[:60]:
                print(f"    {b[0]:12.3f}  frame {b[1]}  +{b[0] - a[0]:5.1f} ms  tick3 {b[3]:#x}  tick1 {b[4]:#x}")


main()
