"""failure_tally.py <runs dir> <tag>: the runs <tag>-<arm><n>[r].log of a series of two builds
(runs/build_pairs.sh: arms A and B; the local series of 2026-11-17: O and N) counted. Per run: the
frame rate lines, the failure - a fault line, a STOP (an assertion of the GPU library ends the game
too), or a hang (`STALL: no frame` in the log: the game lived on and its run line says nothing) -,
the compute queue notes, the late GPU data lines and the GPU faults of its summary line. Per arm:
the counts, then the two-sided Fisher exact test of the failures and of the runs with GPU faults.
Reads only. On U3's roofs before port 0196 the base rate was one failure in 13 runs of 90 s: 20
runs an arm tell only a large difference - a counter that catches the mechanism tells more (the
port-change skill)."""
import math
import os
import re
import sys

root, tag = sys.argv[1], sys.argv[2]
RUN = re.compile(r"^%s-([A-Z]+)(\d+)(r?)\.log$" % re.escape(tag))
FAULT = re.compile(rb"^((Host |Guest )?[Ff]ault \([^\n]{0,60}|STOP: [^\n]{0,70})", re.M)
STALL = re.compile(rb"^STALL: no frame", re.M)
FPS = re.compile(rb"^Frame rate: ([0-9.]+) FPS", re.M)
CQ = re.compile(rb"^Compute queue \d+: ", re.M)
LATE = re.compile(rb"^Late GPU data: a", re.M)
GPUF = re.compile(r"^(\S+) \(.*?GPU faults (\d+) \(BR (\d+), BV (\d+), PROT (\d+)\)")


def fisher(a, b, c, d):
    """Two-sided Fisher exact test of a 2x2 table: (a, b) one arm's yes and no, (c, d) the other's."""
    n1, n2, k = a + b, c + d, a + c
    if n1 == 0 or n2 == 0:
        return 1.0

    def p(x):
        return math.comb(n1, x) * math.comb(n2, k - x) / math.comb(n1 + n2, k)

    p0 = p(a)
    return min(1.0, sum(p(x) for x in range(max(0, k - n2), min(k, n1) + 1) if p(x) <= p0 * (1 + 1e-9)))


gpu = {}
try:
    with open(os.path.join(root, "summary.txt"), encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = GPUF.match(line)
            if m:
                gpu[m.group(1)] = int(m.group(2))
except OSError:
    pass

runs = []
for name in sorted(os.listdir(root)):
    m = RUN.match(name)
    if not m:
        continue
    with open(os.path.join(root, name), "rb") as fh:
        log = fh.read()
    fault = FAULT.search(log)
    failure = ""
    if fault:
        failure = fault.group(0).decode(errors="replace")
    elif STALL.search(log):
        failure = "a hang (STALL)"
    fps = [float(x) for x in FPS.findall(log)]
    runs.append({"arm": m.group(1), "n": int(m.group(2)), "retry": m.group(3), "fps": fps,
                 "failure": failure, "cq": len(CQ.findall(log)), "late": len(LATE.findall(log)),
                 "gpu": gpu.get(name[:-4], 0)})

if not runs:
    sys.exit("no runs %s-<arm><n>.log in %s" % (tag, root))
print("run        lines  last15  GPU faults  compute notes  late GPU data  failure")
for r in sorted(runs, key=lambda r: (r["n"], r["arm"], r["retry"])):
    last = r["fps"][-15:]
    print("%-10s %5d  %6.1f  %10d  %13d  %13d  %s" % (
        "%s%d%s" % (r["arm"], r["n"], r["retry"]), len(r["fps"]), sum(last) / len(last) if last else 0.0,
        r["gpu"], r["cq"], r["late"], r["failure"]))
arms = sorted({r["arm"] for r in runs})
stats = {}
for arm in arms:
    mine = [r for r in runs if r["arm"] == arm]
    stats[arm] = (len(mine), sum(1 for r in mine if r["failure"]), sum(1 for r in mine if r["gpu"]))
    print("%s: %d runs, %d failed, %d with GPU faults, %d with compute notes, %d late GPU data lines" % (
        arm, len(mine), stats[arm][1], stats[arm][2], sum(1 for r in mine if r["cq"]),
        sum(r["late"] for r in mine)))
if len(arms) == 2:
    (n1, f1, g1), (n2, f2, g2) = stats[arms[0]], stats[arms[1]]
    print("Fisher exact, two-sided: failures %s %d/%d against %s %d/%d, p = %.3f; runs with GPU faults "
          "p = %.3f" % (arms[0], f1, n1, arms[1], f2, n2, fisher(f1, n1 - f1, f2, n2 - f2),
                        fisher(g1, n1 - g1, g2, n2 - g2)))
