#!/usr/bin/env python3
"""spv_accesses.py <file.dis> [more ...]: the buffer accesses of a translated shader, from its
SPIR-V disassembly (spirv-dis with its friendly names, the default): for every load from and
store to a buffer block, the binding, the value's type and the index expression traced back
through the multiplies, adds and shifts to constants. It shows what a permutation of a guest
shader was compiled for - the stride of each V# (index * stride in dwords) and its format
(two packed dwords for a half4, three floats for a float3 ...). Two permutations compared line
by line (diff) show where their layouts differ: issue #35's skinning shaders were read this way.

The permutations a device has compiled are in its shader cache,
files/home/.local/share/shadPS4/cache/<title>/<hash>_<permutation>.spv (adb pull; BB_DUMP_SHADERS=1
also writes the ones a run compiles into .../shadPS4/shader/dumps). They are the game's shaders:
keep them and their disassembly outside the repository.
  spirv-dis <hash>_1.spv -o 1.dis; spirv-dis <hash>_2.spv -o 2.dis
  python3 spv_accesses.py 1.dis > 1.txt; python3 spv_accesses.py 2.dis > 2.txt; diff 1.txt 2.txt"""
import re
import sys

OPS = {'OpIMul': '*', 'OpIAdd': '+', 'OpShiftRightLogical': '>>', 'OpShiftLeftLogical': '<<', 'OpISub': '-',
       'OpUDiv': '/', 'OpBitwiseAnd': '&'}


def summarize(path):
    lines = open(path, encoding='utf-8', errors='replace').read().split('\n')
    defs = {}
    for line in lines:
        m = re.match(r'\s*(%\S+) = (\S+)\s*(.*)', line)
        if m:
            defs[m.group(1)] = (m.group(2), m.group(3).split())
    consts = {k: a[1] for k, (op, a) in defs.items() if op == 'OpConstant' and len(a) >= 2}

    def expr(value, depth=0):
        if value in consts:
            return consts[value]
        if depth > 5 or value not in defs:
            return value
        op, a = defs[value]
        if op in OPS and len(a) >= 3:
            return '(%s %s %s)' % (expr(a[1], depth + 1), OPS[op], expr(a[2], depth + 1))
        if op == 'OpLoad' and len(a) >= 2:
            return 'load(%s)' % a[1]
        return value

    binding = {}
    for line in lines:
        m = re.match(r'\s*OpDecorate (%\S+) Binding (\d+)', line)
        if m:
            binding[m.group(1)] = int(m.group(2))
    chains, seen = {}, {}
    for line in lines:
        m = re.match(r'\s*(%\S+) = OpAccessChain (%\S+) (%\S+) (.*)', line)
        if m and m.group(3) in binding:
            index = m.group(4).split()
            chains[m.group(1)] = (binding[m.group(3)], expr(index[-1]) if index else '')
            continue
        m = re.match(r'\s*(%\S+) = OpLoad (%\S+) (%\S+)', line)
        if m and m.group(3) in chains:
            key = ('load',) + chains[m.group(3)] + (m.group(2),)
        else:
            m = re.match(r'\s*OpStore (%\S+) (%\S+)', line)
            if not (m and m.group(1) in chains):
                continue
            # The stored value's type is its definition's first operand.
            kind = defs.get(m.group(2), ('', ['?']))[1][:1]
            key = ('store',) + chains[m.group(1)] + (kind[0] if kind else '?',)
        # Result ids differ between permutations: only the shape of the index is kept.
        key = key[:2] + (re.sub(r'%\w+', '%x', key[2]),) + key[3:]
        seen[key] = seen.get(key, 0) + 1
    print('==', path.replace('\\', '/').rsplit('/', 1)[-1])
    for (kind, b, index, value_type), count in sorted(seen.items(), key=lambda kv: (kv[0][1], kv[0][0], kv[0][2])):
        print('  binding %2d %-5s x%-3d %-14s index %s' % (b, kind, count, value_type[:14], index[:120]))


if len(sys.argv) < 2:
    sys.exit(__doc__)
for p in sys.argv[1:]:
    summarize(p)
