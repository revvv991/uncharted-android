#!/usr/bin/env python3
"""frame_trace.py <log> [secs]: the `Frame trace` lines of BB_PRESENT_TRACE=1 over the last <secs>
seconds of the log (90 by default: a run's turning).

Per frame: the guest's flip call (and its wait there for the submission lock and the GPU side's
idle), the request queued for the present thread, taken (and the slot it was due at), handed to
vkQueuePresentKHR, its desired present time and the vsync it went out on.

Prints the stages' latencies; the frames on the screen for 3+ refreshes by cause:
  - taken late, the guest's frame long: from the return of its previous flip to this flip call
    longer than the frame period;
  - taken late, the guest held in its previous flip: that flip's wait (the lock, the GPU side's
    idle) made it late;
  - taken late, the port's processing long: the guest's flip to the request queued 8 ms over the
    median (the GPU thread's work up to the flip, the frame's preparation);
  - in time, shown late: taken in time, its vsync missed (the GPU work after SurfaceFlinger's latch);
and the motion's evenness: the game moves its world by the time between its frames and the screen
shows them on vsyncs, so a change between consecutive frames of the delay from the guest's flip to
the screen is a jump (shorter by a refresh: the motion skips forward; longer: it holds)."""
import re
import sys

PAT = re.compile(r"Frame trace: id (\d+), arg (-?\d+), guest ([-\d.]+) waited ([-\d.]+), queued ([-\d.]+), "
                 r"taken ([-\d.]+) due ([-\d.]+), presented ([-\d.]+), desired ([-\d.]+), shown ([-\d.]+), "
                 r"refreshes (-?\d+), gap (\d+)")
REFRESH = 1000.0 / 60
PERIOD = 2 * REFRESH


def pct(values, p):
    values = sorted(values)
    return values[min(len(values) - 1, int(p / 100 * len(values)))] if values else float("nan")


def main():
    path = sys.argv[1]
    secs = float(sys.argv[2]) if len(sys.argv) > 2 else 90.0
    rows = []
    for line in open(path, encoding="utf-8", errors="replace"):
        m = PAT.search(line)
        if m:
            v = [float(x) for x in m.groups()]
            rows.append(dict(id=int(v[0]), arg=int(v[1]), guest=v[2], waited=v[3], queued=v[4], taken=v[5],
                             due=v[6], presented=v[7], desired=v[8], shown=v[9], n=int(v[10]), gap=int(v[11])))
    if not rows:
        sys.exit("no Frame trace lines")
    end = rows[-1]["shown"]
    rows = [r for r in rows if r["shown"] >= end - secs * 1000]
    print(f"{len(rows)} paced frames over the last {secs:.0f} s ({len(rows) / secs:.1f} a second); "
          f"presents not shown among them: {sum(r['gap'] for r in rows)}")

    def stage(name, values):
        if values:
            print(f"  {name:42s} median {pct(values, 50):6.1f}  p90 {pct(values, 90):6.1f}  "
                  f"p99 {pct(values, 99):6.1f}  max {max(values):6.1f} ms")

    g = [r for r in rows if r["guest"] > 0]
    print("stages:")
    stage("the guest's wait in its flip", [r["waited"] for r in g])
    stage("the guest's flip -> request queued", [r["queued"] - r["guest"] for r in g])
    stage("queued -> taken", [r["taken"] - r["queued"] for r in rows])
    stage("taken -> vkQueuePresentKHR", [r["presented"] - r["taken"] for r in rows])
    stage("vkQueuePresentKHR -> shown", [r["shown"] - r["presented"] for r in rows])
    stage("the guest's flip -> shown", [r["shown"] - r["guest"] for r in g])
    port_median = pct([r["queued"] - r["guest"] for r in g], 50)

    counts, long_rows = {}, []
    for i in range(1, len(rows)):
        r, prev = rows[i], rows[i - 1]
        if r["n"] < 3:
            continue
        if r["taken"] > r["due"] + 1.0:
            own = r["guest"] - (prev["guest"] + prev["waited"]) if r["guest"] > 0 and prev["guest"] > 0 else None
            if r["guest"] > 0 and r["queued"] - r["guest"] > port_median + 8:
                cause = "taken late: the port's processing long"
            elif own is not None and own > PERIOD:
                cause = "taken late: the guest's frame long"
            elif prev["waited"] > 4:
                cause = "taken late: the guest held in its previous flip"
            else:
                cause = "taken late: other"
        elif r["shown"] - (r["desired"] + REFRESH / 2) > REFRESH / 2:
            cause = "in time, shown late (GPU after the latch)"
        else:
            cause = "other"
        counts[cause] = counts.get(cause, 0) + 1
        long_rows.append((r, prev, cause))
    print(f"frames on the screen 3+ refreshes: {len(long_rows)} ({len(long_rows) / secs * 60:.0f} a minute)")
    for cause, c in sorted(counts.items(), key=lambda kv: -kv[1]):
        print(f"  {cause:46s} {c}")

    jumps = []
    for i in range(1, len(rows)):
        a, b = rows[i - 1], rows[i]
        if a["guest"] > 0 and b["guest"] > 0 and b["id"] == a["id"] + 1:
            jumps.append(((b["shown"] - b["guest"]) - (a["shown"] - a["guest"]), b))
    big = [j for j in jumps if abs(j[0]) >= REFRESH * 0.75]
    print(f"motion: the delay from the guest's flip to the screen changed by 12.5+ ms between consecutive "
          f"frames {len(big)} times ({len(big) / secs * 60:.0f} a minute): {sum(1 for d, _ in big if d < 0)} "
          f"shorter (a skip forward), {sum(1 for d, _ in big if d > 0)} longer (a hold)")
    print("the long frames (ms; own = the guest's frame from its previous flip's return):")
    for r, prev, cause in long_rows[:40]:
        own = r["guest"] - (prev["guest"] + prev["waited"]) if r["guest"] > 0 and prev["guest"] > 0 else float("nan")
        port = r["queued"] - r["guest"] if r["guest"] > 0 else float("nan")
        print(f"  id {r['id']:6d} {r['n']} refr: own {own:5.1f}, prev wait {prev['waited']:5.1f}, port {port:5.1f}, "
              f"taken {r['taken'] - r['due']:+6.1f} vs due, shown {r['shown'] - r['desired'] - REFRESH / 2:+6.1f} "
              f"vs meant; {cause}")


main()
