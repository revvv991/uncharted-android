#!/usr/bin/env python3
"""The split GPU profile (BB_GPU_PROFILE=2 or 3) of a run's log, summed over its 5 s blocks.

    gprof_split.py <log> [--blocks A-B] [--top N] [--by-shader] [--pass WxH]

Each label of the split profile is a run of draws with one pixel shader and one depth and blend
state inside one pass. The table: GPU ms a frame, its share, segments, draws, thousands of
indices and millions of fragment shader invocations a frame, ns a fragment, the label.
--blocks picks the 5 s blocks to sum (the list of blocks is printed first: choose the ones
where the scene is the one meant); --by-shader merges a pixel shader's labels in a pass;
--pass keeps the passes of that size. The times are true per label where the driver runs a
pass's draws in order (TU_DEBUG=sysmem)."""
import re
import sys

HEAD = re.compile(r'^GPU profile: ([\d.]+) ms/frame over (\d+) frames \(([\d.]+) ms/s\)')
ROW = re.compile(r'^\s+([\d.]+) ms/frame\s+([\d.]+)/frame  (.*?)'
                 r'(?: \| ([\d.]+) draws, ([\d.]+) k indices)?(?: \| ([\d.]+) M fragments)?$')
SHADER = re.compile(r'^(pass \S+ \[[^\]]*\] ps [0-9a-f]+)')


def blocks_of(path):
    blocks, rows = [], None
    for line in open(path, encoding='utf-8', errors='replace'):
        line = line.rstrip('\r\n')
        head = HEAD.match(line)
        if head:
            rows = []
            blocks.append({'ms': float(head[1]), 'frames': int(head[2]), 'rows': rows})
            continue
        if rows is None:
            continue
        if 'idle gaps by length' in line:   # the block's rows end here
            rows = None
            continue
        row = ROW.match(line)
        if not row:                          # another thread's line in between
            continue
        rows.append((row[3], float(row[1]), float(row[2]), float(row[4] or 0),
                     float(row[5] or 0), float(row[6] or 0)))
    return blocks


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    path, first, last, top, merge, size = args[0], 1, 10**9, 40, False, None
    i = 1
    while i < len(args):
        if args[i] == '--blocks':
            a, _, b = args[i + 1].partition('-')
            first, last = int(a), int(b or a)
            i += 2
        elif args[i] == '--top':
            top = int(args[i + 1])
            i += 2
        elif args[i] == '--pass':
            size = args[i + 1]
            i += 2
        elif args[i] == '--by-shader':
            merge = True
            i += 1
        else:
            sys.exit(__doc__)
    blocks = blocks_of(path)
    if not blocks:
        sys.exit('no GPU profile blocks in ' + path)
    print('blocks (5 s each): ' + ', '.join(
        '%d: %.0f ms x %d' % (n + 1, b['ms'], b['frames']) for n, b in enumerate(blocks)))
    chosen = [b for n, b in enumerate(blocks) if first <= n + 1 <= last]
    frames = sum(b['frames'] for b in chosen)
    if not frames:
        sys.exit('no frames in the chosen blocks')
    total = {}
    for b in chosen:
        for label, ms, segments, draws, indices, fragments in b['rows']:
            if size and not label.startswith('pass %s ' % size):
                continue
            if merge:
                shader = SHADER.match(label)
                label = shader[1] if shader else label
            t = total.setdefault(label, [0.0] * 5)
            for k, v in enumerate((ms, segments, draws, indices, fragments)):
                t[k] += v * b['frames']
    whole = sum(t[0] for t in total.values()) / frames
    print('%d frames in %d blocks, %.1f ms a frame in the labels listed by the log%s'
          % (frames, len(chosen), whole, ' (%s passes)' % size if size else ''))
    print('%8s %6s %7s %7s %9s %9s %7s  %s' % ('ms/frame', 'share', 'pieces', 'draws', 'k indices',
                                              'M fragm.', 'ns/fr.', 'label'))
    ranked = sorted(total.items(), key=lambda kv: -kv[1][0])
    for label, t in ranked[:top]:
        ms, segments, draws, indices, fragments = (v / frames for v in t)
        per = '%7.1f' % (ms * 1e6 / (fragments * 1e6)) if fragments > 0 else '      -'
        print('%8.3f %5.1f%% %7.1f %7.1f %9.1f %9.3f %s  %s'
              % (ms, 100 * ms / whole, segments, draws, indices, fragments, per, label))
    rest = ranked[top:]
    if rest:
        print('%8.3f %5.1f%%  in %d more labels'
              % (sum(t[0] for _, t in rest) / frames,
                 100 * sum(t[0] for _, t in rest) / frames / whole, len(rest)))


if __name__ == '__main__':
    main()
