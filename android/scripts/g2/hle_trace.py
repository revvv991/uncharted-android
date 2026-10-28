#!/usr/bin/env python3
# hle_trace.py: the port's host function trace (BB_HLE_TRACE, guest_cpu_fex.cpp) read on the PC.
#   hle_trace.py <file> [--top N] [--calls NAME] [--thread NAME [--frames N]] [--tail N]
# <file> is the trace (records of 64 bytes: function id, thread, the guest's call site, three
# integer arguments, entered and left in ns, the integer result) with <file>.names and
# <file>.threads beside it.
# Prints the frame times from the flips (sceVideoOutSubmitFlip and the SubmitAndFlip calls), then
# per thread where its time in host functions goes (by function and call site, ms per second);
# --calls: every call of one function (arguments, result, duration, call site); --thread: one
# thread's calls of 0.1 ms and longer over the first frames, relative to the flips; --tail: each
# thread's last calls before the trace ends (a flight recorder trace, BB_HLE_TRACE_RING: a hang).
import argparse
import collections
import statistics
import struct

RECORD = struct.Struct('<IIQ3QQQQ')
FLIPS = ('sceVideoOutSubmitFlip', 'sceGnmSubmitAndFlipCommandBuffers', 'sceGnmSubmitAndFlipCommandBuffersForWorkload')


def load(path):
    names = {}
    with open(path + '.names', encoding='utf-8') as f:
        for line in f:
            i, _, name = line.rstrip('\n').partition(' ')
            names[int(i)] = name
    threads = {}
    with open(path + '.threads', encoding='utf-8') as f:
        for line in f:
            tid, _, name = line.rstrip('\n').partition(' ')
            threads[int(tid)] = name
    with open(path, 'rb') as f:
        data = f.read()
    records = []
    for at in range(0, len(data) - RECORD.size + 1, RECORD.size):
        fid, tid, caller, a0, a1, a2, enter, leave, result = RECORD.unpack_from(data, at)
        if enter and leave >= enter:
            records.append((enter, leave, fid, tid, caller, (a0, a1, a2), result))
    records.sort()
    return names, threads, records


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('trace')
    ap.add_argument('--top', type=int, default=6, help='entries per thread')
    ap.add_argument('--calls', help='list the calls of this function')
    ap.add_argument('--thread', help='the calls of the threads with this name over the first frames')
    ap.add_argument('--frames', type=int, default=4)
    ap.add_argument('--tail', type=int, help="each thread's last N calls before the trace ends")
    a = ap.parse_args()
    names, threads, records = load(a.trace)
    if not records:
        raise SystemExit('no records')
    t0 = records[0][0]
    span = (max(r[1] for r in records) - t0) / 1e9
    print('%d calls in %.2f s, %d threads' % (len(records), span, len({r[3] for r in records})))

    flips = [r for r in records if names.get(r[2]) in FLIPS]
    times = [r[0] for r in flips]
    if len(times) > 2:
        gaps = [(b - a) / 1e6 for a, b in zip(times, times[1:])]
        print('flips: %d (%s), interval median %.2f ms, mean %.2f, min %.2f, max %.2f' % (
            len(flips), ', '.join(sorted({names[r[2]] for r in flips})), statistics.median(gaps),
            statistics.mean(gaps), min(gaps), max(gaps)))
        print('  flip arguments seen:', sorted({(names[r[2]],) + r[5] for r in flips})[:6])

    if a.calls:
        for enter, leave, fid, tid, caller, args, result in records:
            if names.get(fid) == a.calls:
                print('%9.3f ms  %-16s %8.3f ms  from %#x  args %s -> %#x' % (
                    (enter - t0) / 1e6, threads.get(tid, tid), (leave - enter) / 1e6, caller,
                    ' '.join('%#x' % x for x in args), result))
        return

    if a.tail:
        end = max(r[1] for r in records)
        last = collections.defaultdict(lambda: collections.deque(maxlen=a.tail))
        for r in records:
            last[r[3]].append(r)
        for tid in sorted(last, key=lambda t: (threads.get(t, '?'), t)):
            print('thread %d %s' % (tid, threads.get(tid, '?')))
            for enter, leave, fid, _, caller, args, result in last[tid]:
                print('  %9.3f ms before the end  %-34s %8.3f ms  from %#x  args %s -> %#x' % (
                    (end - enter) / 1e6, names.get(fid, fid), (leave - enter) / 1e6, caller,
                    ' '.join('%#x' % x for x in args), result))
        return

    if a.thread:
        tids = {t for t, n in threads.items() if n == a.thread}
        end = times[a.frames] if len(times) > a.frames else records[-1][1]
        marks = iter(times)
        mark = next(marks, None)
        for enter, leave, fid, tid, caller, args, _ in records:
            if enter > end:
                break
            while mark is not None and mark <= enter:
                print('---- flip at %.3f ms' % ((mark - t0) / 1e6))
                mark = next(marks, None)
            if tid in tids and leave - enter >= 100000:
                print('%9.3f ms  %-34s %8.3f ms  from %#x  args %s' % (
                    (enter - t0) / 1e6, names.get(fid, fid), (leave - enter) / 1e6, caller,
                    ' '.join('%#x' % x for x in args)))
        return

    per = collections.defaultdict(lambda: collections.defaultdict(lambda: [0, 0]))
    for enter, leave, fid, tid, caller, args, _ in records:
        e = per[tid][(fid, caller)]
        e[0] += 1
        e[1] += leave - enter
    for tid in sorted(per, key=lambda t: (threads.get(t, '?'), t)):
        entries = sorted(per[tid].items(), key=lambda kv: -kv[1][1])
        total = sum(v[1] for v in per[tid].values()) / 1e6 / span
        print('thread %d %-16s in host functions %6.1f ms/s, %d calls/s' % (
            tid, threads.get(tid, '?'), total, sum(v[0] for v in per[tid].values()) / span))
        for (fid, caller), (count, ns) in entries[:a.top]:
            print('    %7.1f ms/s  %6.1f calls/s  %7.3f ms each  %-34s from %#x' % (
                ns / 1e6 / span, count / span, ns / 1e6 / count, names.get(fid, fid), caller))


if __name__ == '__main__':
    main()
