#!/usr/bin/env python3
"""The DUMP sections of a bb-probe.log (BB_DUMP_REQUEST): per dump and thread, the name, the guest's
host call (if any) and the host frames as library+offset. Writes offsets.txt (lib offset) for
llvm-symbolizer and prints the per-thread summary. Usage: dumps.py <log> [name filter regex]"""
import re
import sys

log = open(sys.argv[1], encoding='utf-8', errors='replace').read().split('\n')
want = re.compile(sys.argv[2]) if len(sys.argv) > 2 else None
maps = []  # (start, end, file_offset, path)
dumps = []
cur = None
thread = None
for line in log:
    if line.startswith('DUMP: thread stacks'):
        cur = []
        dumps.append(cur)
        thread = None
        continue
    if cur is None:
        continue
    if line.startswith('DUMP: fibers') or line.startswith('DUMP: end'):
        cur = None
        continue
    m = re.match(r'  map ([0-9a-f]+)-([0-9a-f]+) r-xp ([0-9a-f]+) \S+ \S+ +(\S*)', line)
    if m:
        maps.append((int(m.group(1), 16), int(m.group(2), 16), int(m.group(3), 16), m.group(4)))
        continue
    m = re.match(r'  tid=([0-9a-f]+) rdi=', line)
    if m:
        thread = {'tid': int(m.group(1), 16), 'name': '?', 'frames': [], 'guest': ''}
        cur.append(thread)
        continue
    if thread is None:
        continue
    m = re.match(r'  name=(.*)', line)
    if m:
        thread['name'] = m.group(1)
        continue
    m = re.match(r'  tid=[0-9a-f]+ rip=([0-9a-f]+)', line)
    if m:
        thread['frames'].append(int(m.group(1), 16))
        continue
    m = re.match(r'  guest in host function (.*)', line)
    if m:
        thread['guest'] = m.group(1)
        continue
    m = re.match(r'  guest stack: (.*)', line)
    if m:
        thread['gstack'] = m.group(1)[:120]

def place(rip):
    for start, end, off, path in maps:
        if start <= rip < end:
            name = path.rsplit('/', 1)[-1] or '?'
            return name, rip - start + off
    return '?', rip

offsets = set()
for i, d in enumerate(dumps, 1):
    print(f'===== dump {i}: {len(d)} threads')
    for t in d:
        if want and not want.search(t['name']):
            continue
        frames = []
        for rip in t['frames'][:12]:
            lib, off = place(rip)
            if lib in ('libbbgpu.so', 'libmain.so', 'libvulkan_freedreno.so', 'libc.so', 'libSDL3.so'):
                frames.append(f'{lib}+0x{off:x}')
                if lib in ('libbbgpu.so', 'libmain.so', 'libvulkan_freedreno.so'):
                    offsets.add((lib, off))
            else:
                frames.append(lib)
        print(f"-- {t['name']} (tid {t['tid']})" + (f"  GUEST: {t['guest']}" if t['guest'] else ''))
        if t.get('gstack'):
            print(f"   guest stack: {t['gstack']}")
        print('   ' + ' < '.join(frames))
with open('offsets.txt', 'w') as f:
    for lib, off in sorted(offsets):
        f.write(f'{lib} 0x{off:x}\n')
print(f'{len(offsets)} offsets written to offsets.txt')
