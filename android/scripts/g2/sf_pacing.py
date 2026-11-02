#!/usr/bin/env python3
"""sf_pacing.py <dump file> [frame limit] [--summary]: how evenly the game's frames reached the screen.

The input is runs/sf_pacing.sh's polls of `dumpsys SurfaceFlinger --latency <layer>`: per frame
the desired present time (the queue time unless the producer set a timestamp), the actual present
time (the display's vsync that showed it) and the frame ready time (its GPU fence), in ns. Frames
are merged across the overlapping polls; the intervals between frames on the screen are counted
in display refreshes. With a frame limit (30 at 60 Hz: 2 refreshes a frame) the share of frames
held for another number of refreshes is the judder. --summary: one line (continue_run.sh).
"""
import statistics
import sys

PENDING = (1 << 63) - 1


def load(path):
    period, frames, dropped = None, {}, set()
    for line in open(path, encoding="utf-8", errors="replace"):
        parts = line.split()
        if len(parts) == 1 and parts[0].isdigit():
            period = int(parts[0])
        elif len(parts) == 3 and all(p.isdigit() for p in parts):
            desired, actual, ready = map(int, parts)
            if desired == 0:
                continue
            if actual in (0, PENDING) or actual < 0:
                dropped.add(desired)
            else:
                frames[desired] = (actual, ready)
    dropped -= set(frames)
    return period, frames, dropped


def pct(values, p):
    values = sorted(values)
    return values[min(len(values) - 1, int(len(values) * p / 100))]


def main():
    args = [a for a in sys.argv[1:] if a != "--summary"]
    path = args[0]
    limit = float(args[1]) if len(args) > 1 else 0
    period, frames, dropped = load(path)
    if not period or len(frames) < 3:
        print("no frames")
        return
    shown = sorted(frames.items(), key=lambda kv: kv[1][0])
    actual = [a for _, (a, _) in shown]
    gaps = [b - a for a, b in zip(actual, actual[1:])]
    refreshes = [max(1, round(g / period)) for g in gaps]
    span = (actual[-1] - actual[0]) / 1e9
    hz = 1e9 / period
    if "--summary" in sys.argv:
        share = lambda f: 100 * sum(1 for r in refreshes if f(r)) / len(refreshes)
        line = (f"screen {len(actual) / span:.2f} FPS, refreshes 1/2/3/4+ {share(lambda r: r == 1):.1f}/"
                f"{share(lambda r: r == 2):.1f}/{share(lambda r: r == 3):.1f}/{share(lambda r: r >= 4):.1f}%")
        if limit:
            target = round(hz / limit)
            line += f", off the limit {share(lambda r: r != target):.1f}%"
        print(line)
        return
    print(f"{path}: {len(actual)} frames on the screen over {span:.1f} s ({len(actual) / span:.2f} FPS), "
          f"refresh {hz:.2f} Hz; {len(dropped)} queued frames never shown")
    hist = {}
    for r in refreshes:
        hist[min(r, 6)] = hist.get(min(r, 6), 0) + 1
    print("  refreshes per frame: " + ", ".join(
        f"{k if k < 6 else '6+'}: {v} ({100 * v / len(refreshes):.1f}%)" for k, v in sorted(hist.items())))
    changes = sum(1 for a, b in zip(refreshes, refreshes[1:]) if a != b)
    print(f"  on-screen frame time ms: median {statistics.median(gaps) / 1e6:.1f}, p90 {pct(gaps, 90) / 1e6:.1f}, "
          f"p99 {pct(gaps, 99) / 1e6:.1f}, max {max(gaps) / 1e6:.1f}; stddev {statistics.pstdev(gaps) / 1e6:.2f}; "
          f"cadence changes {changes} ({100 * changes / max(1, len(refreshes) - 1):.1f}% of frames)")
    if limit:
        target = round(hz / limit)
        off = [r for r in refreshes if r != target]
        short = sum(1 for r in refreshes if r < target)
        long_ = sum(1 for r in refreshes if r > target)
        print(f"  limit {limit:g} FPS = {target} refreshes a frame: {len(off)} frames off it "
              f"({100 * len(off) / len(refreshes):.1f}%): {short} shorter, {long_} longer")
        # A longer frame next to a shorter one is a phase slip (both frames came in time, one
        # missed its latch); a longer one alone is a frame that came late.
        paired, used = 0, set()
        for i, r in enumerate(refreshes):
            if r > target and i not in used:
                for j in (i - 1, i + 1):
                    if 0 <= j < len(refreshes) and refreshes[j] < target and j not in used:
                        paired += 1
                        used.update((i, j))
                        break
        print(f"  longer frames next to a shorter one (phase slips): {paired}; longer ones alone "
              f"(late frames): {long_ - paired}")
    queue_to_screen = [frames[d][0] - d for d in frames]
    ready_to_screen = [a - r for a, r in frames.values() if 0 < r < a]
    print(f"  queue to screen ms: median {statistics.median(queue_to_screen) / 1e6:.1f}, "
          f"p90 {pct(queue_to_screen, 90) / 1e6:.1f}; GPU ready to screen ms: median "
          f"{statistics.median(ready_to_screen) / 1e6:.1f}, p90 {pct(ready_to_screen, 90) / 1e6:.1f}")
    # The sequence of refreshes per frame, a line per ~2 seconds, to see runs of judder.
    line, start = [], actual[0]
    for t, r in zip(actual[1:], refreshes):
        line.append(str(r) if r < 10 else "+")
        if t - start >= 2e9:
            print(f"  {(start - actual[0]) / 1e9:6.1f} s  {''.join(line)}")
            line, start = [], t
    if line:
        print(f"  {(start - actual[0]) / 1e9:6.1f} s  {''.join(line)}")


if __name__ == "__main__":
    main()
