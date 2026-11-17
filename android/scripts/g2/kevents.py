#!/usr/bin/env python3
"""The kernel's GPU events of a series of runs, from the dmesg each run saved at its end: for
every run the window between the previous run's last kernel line and its own, and in it the CP
opcode errors (with the process they name), the CP AHB bus errors (lines and suppressed
callbacks), the GPU page faults (with their addresses' count) and the GPU hangs. The run scripts
count CP faults by the game's process id only: a fault in the collection menu's process of the
same run and every page fault are missed there (2026-11-15).
Usage: kevents.py <run.dmesg> [<run.dmesg> ...]   (in the order the runs were made)"""
import re
import sys

TS = re.compile(r'^\[\s*([0-9]+\.[0-9]+)\]')


def lines(path):
    out = []
    for raw in open(path, encoding='utf-8', errors='replace'):
        m = TS.match(raw)
        if m:
            out.append((float(m.group(1)), raw.rstrip('\n')))
    return out


prev_end = None
total = [0, 0, 0, 0]
runs, seconds = 0, 0.0
for path in sys.argv[1:]:
    ls = lines(path)
    if not ls:
        print('%-22s no kernel lines' % path.rsplit('/', 1)[-1])
        continue
    end = ls[-1][0]
    start = prev_end if prev_end is not None else end - 420.0
    cp, ahb, supp, pf, hang, pids, where = 0, 0, 0, 0, 0, set(), []
    for t, s in ls:
        if not (start < t <= end):
            continue
        if 'CP opcode error interrupt' in s:
            cp += 1
            where.append('%.0f s before the end' % (end - t))
        elif 'CP: AHB bus error' in s:
            ahb += 1
        elif 'callbacks suppressed' in s and 'gen7_err_callback' in s:
            m = re.search(r'(\d+) callbacks suppressed', s)
            supp += int(m.group(1)) if m else 0
        elif 'GPU PAGE FAULT' in s:
            pf += 1
            where.append('page faults %.0f s before the end' % (end - t)) if pf == 1 else None
        elif 'GPU hang detected' in s:
            hang += 1
        m = re.search(r'kgsl-3d0: (\S+\[\d+\]): cmdline', s) or re.search(r'pid= ?(\d+) name=nchartedandroid', s)
        if m:
            pids.add(m.group(1))
    name = path.replace('\\', '/').rsplit('/', 1)[-1].replace('.dmesg', '')
    runs += 1
    if cp or pf or hang:
        print('%-14s %3.0f s: CP opcode errors %d, page faults %d, hangs %d; pids %s; %s' % (
            name, end - start, cp, pf, hang, ','.join(sorted(pids)) or '-', '; '.join(where)))
    total = [total[0] + cp, total[1] + ahb + supp, total[2] + pf, total[3] + hang]
    seconds += end - start
    prev_end = end
print('%d runs, %.0f s: CP opcode errors %d, page faults %d, hangs %d; AHB bus errors %d (%.0f a second: the known '
      'background of register 0x8e07, see the findings of 2026-10-20)' % (
          runs, seconds, total[0], total[2], total[3], total[1], total[1] / max(seconds, 1.0)))
