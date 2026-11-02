#!/usr/bin/env python3
"""cap_diff.py <frame A> <frame B>: two frame-analyzer captures side by side - the passes and
dispatches per shader (vs/ps or cs hash) with their draw counts and target sizes, and what differs
(passes only in one of them, different counts). For the U3 question: does the focus read-back
change what the game renders (depth of field passes) or only add the copies?"""
import collections
import re
import sys


def passes(path):
    """-> list of (kind, shaders, draws, targets)"""
    out = []
    kind = shaders = None
    draws = 0
    targets = []
    for line in open(path, encoding='utf-8', errors='replace'):
        m = re.match(r'#(\d+) (PASS|COMPUTE) (?:draws|dispatches) (\d+)(?: \(indices \d+\))?, (?:vs/ps|cs) (.*)', line)
        if m:
            if kind:
                out.append((kind, shaders, draws, tuple(targets)))
            # the shaders of a pass as a set: their order in the list differs between captures
            kind, draws = m.group(2), int(m.group(3))
            shaders = ' '.join(sorted(set(h[-8:] for h in m.group(4).split())))
            targets = []
            continue
        m = re.match(r'  (color\d|depth)\s+(?:0x[0-9a-f]+ )?(\S+) (\d+x\d+)', line)
        if m and kind:
            targets.append('%s %s %s' % (m.group(1), m.group(2), m.group(3)))
    if kind:
        out.append((kind, shaders, draws, tuple(targets)))
    return out


def summary(ps):
    c = collections.Counter()
    draws = collections.Counter()
    for kind, shaders, d, targets in ps:
        key = (kind, shaders, targets)
        c[key] += 1
        draws[key] += d
    return c, draws


a, b = sys.argv[1], sys.argv[2]
pa, pb = passes(a), passes(b)
ca, da = summary(pa)
cb, db = summary(pb)
print('A: %d passes (%d draws/dispatches); B: %d passes (%d)' % (len(pa), sum(da.values()), len(pb), sum(db.values())))
keys = set(ca) | set(cb)
diff = [(k, ca.get(k, 0), cb.get(k, 0), da.get(k, 0), db.get(k, 0)) for k in keys if ca.get(k, 0) != cb.get(k, 0) or da.get(k, 0) != db.get(k, 0)]
diff.sort(key=lambda x: (-(abs(x[1] - x[2]) + abs(x[3] - x[4])), x[0]))
print('--- differing (kind shaders | passes A/B | draws A/B | targets):')
for (kind, shaders, targets), na, nb, dA, dB in diff[:60]:
    hashes = shaders.split()
    shown = ' '.join(hashes[:4]) + (' +%d' % (len(hashes) - 4) if len(hashes) > 4 else '')
    print('%s %s | %d/%d | %d/%d | %s' % (kind, shown, na, nb, dA, dB, '; '.join(targets)[:90]))
print('--- the same in both: %d pass kinds' % (len(keys) - len(diff)))
