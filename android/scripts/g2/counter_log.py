#!/usr/bin/env python3
"""counter_log.py <ring file> [--all]: U2's job counter events written by BB_COUNTER_LOG (probe.c
install_counter_log) when a counter went below 0 by a few: the event that froze the ring, then
every event of that counter object (frees with their caller, decrements by a finished job or
barrier with three slots of the worker's frame - rbp-0x88, -0x80, -0x70 -, notifies with their
caller), oldest first. --all prints the whole ring."""
import struct
import sys

HEADER = 64
EVENT = struct.Struct('<8Q')
KINDS = {1: 'done', 2: 'notify', 4: 'free'}


def signed(v):
    return v - (1 << 64) if v >= 1 << 63 else v


def main():
    data = open(sys.argv[1], 'rb').read()
    nxt, frozen = struct.unpack_from('<QQ', data, 0)
    count = (len(data) - HEADER) // EVENT.size
    events = []
    first = max(0, nxt - count)
    for i in range(first, nxt):
        e = EVENT.unpack_from(data, HEADER + (i % count) * EVENT.size)
        if e[7] in KINDS:
            events.append((i, e))
    print(f'{nxt} events, {len(events)} in the ring, frozen {frozen}')
    if not events:
        return
    t0 = events[-1][1][5]
    trigger = next((ev for ev in reversed(events)
                    if ev[1][7] in (1, 2) and -16 < signed(ev[1][1]) < 0), None)
    show = events if '--all' in sys.argv or not trigger else \
        [ev for ev in events if ev[1][0] == trigger[1][0]]
    if trigger:
        print(f'trigger: event {trigger[0]}, counter {trigger[1][0]:#x} = {signed(trigger[1][1])}')
    for i, (counter, value, a, b, c, tsc, rsp, kind) in show:
        dt = (signed(tsc - t0)) / 19.2e3  # the G2's counter: 19.2 MHz -> ms
        what = KINDS[kind]
        if kind == 1:
            detail = f'frame -0x88 {a:#x} -0x80 {b:#x} -0x70 {c:#x}'
        else:
            detail = f'caller {a:#x}'
        print(f'{i:8d} {dt:10.3f} ms  {what:6s} {counter:#x} -> {signed(value):6d}  {detail}  rsp {rsp:#x}')


if __name__ == '__main__':
    main()
