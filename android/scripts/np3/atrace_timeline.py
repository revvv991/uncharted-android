#!/usr/bin/env python3
"""An atrace text dump (sched + gfx): per 50 ms bucket, how often the KGSL threads and the port's
GPU-side threads were woken, and who woke them. Usage: atrace_timeline.py <atrace.txt>"""
import collections
import re
import sys

watch = {'gmu_f2h': 'gmu', 'kgsl_hwsched': 'hwsched', 'kgsl-events': 'events',
         'bb:VkRecorder': 'VkRec', 'bb:DrawRec': 'DrawRec', 'shadPS4:GpuSche': 'GpuSche',
         'bb:Landing': 'Landing', 'shadPS4:GpuComm': 'GpuComm', 'shadPS4:Present': 'PresTh',
         'bb:Present': 'bbPres', 'SDLThread': 'SDL', 'FlipHandler': 'Flip', 'kgsl_devfreq_no': 'devfreq'}
line_re = re.compile(r'^\s*(\S.*?)-(\d+)\s+\(\s*-?\d+\)\s+\[(\d+)\]\s+\S+\s+(\d+\.\d+): (\w+): (.*)$')
buckets = collections.defaultdict(lambda: collections.Counter())
wakers = collections.defaultdict(collections.Counter)
t0 = None
marks = collections.Counter()
for line in open(sys.argv[1], encoding='utf-8', errors='replace'):
    m = line_re.match(line)
    if not m:
        continue
    comm, pid, cpu, ts, ev, rest = m.groups()
    ts = float(ts)
    if t0 is None:
        t0 = ts
    b = int((ts - t0) / 0.05)
    if ev == 'sched_waking':
        w = re.search(r'comm=(\S+)', rest)
        if w and w.group(1) in watch:
            buckets[b][watch[w.group(1)]] += 1
            wakers[watch[w.group(1)]][comm] += 1
    elif ev == 'tracing_mark_write':
        marks[(comm, rest[:60])] += 1
print('bucket(50ms): ' + ' '.join(f'{v:>7s}' for v in ['gmu', 'hwsched', 'events', 'VkRec', 'DrawRec', 'GpuSche', 'Landing', 'GpuComm', 'PresTh', 'Flip', 'SDL']))
for b in range(0, max(buckets) + 1):
    c = buckets[b]
    print(f'{b * 50:6d} ms     ' + ' '.join(f'{c[v]:7d}' for v in ['gmu', 'hwsched', 'events', 'VkRec', 'DrawRec', 'GpuSche', 'Landing', 'GpuComm', 'PresTh', 'Flip', 'SDL']))
print('\nwho wakes whom (top 4):')
for k, c in wakers.items():
    print(f'  {k:8s}: ' + ', '.join(f'{w} x{n}' for w, n in c.most_common(4)))
print('\ntracing_mark_write (top 25):')
for (comm, text), n in marks.most_common(25):
    print(f'  {n:5d} {comm:20s} {text}')
