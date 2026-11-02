#!/usr/bin/env python3
"""apk_diff.py <a.apk> <b.apk>: two builds compared entry by entry - the entries only in one, the
identical ones counted, and for each differing ELF library the byte ranges that differ with the
section they fall in. A release APK built from the public copy against the test build the owner
played: only AndroidManifest.xml (versionCode, versionName), the signature files under META-INF/
and the libraries' 20-byte .note.gnu.build-id may differ (the build directory is in the unstripped
debug info the build ID hashes). Our own APKs only; run it with `python -I`."""
import struct
import sys
import zipfile


def sections(data):
    """(start, end, name) of the ELF64 sections with bytes in the file."""
    shoff, = struct.unpack_from('<Q', data, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 0x3a)
    hdrs = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * shentsize) for i in range(shnum)]
    names = hdrs[shstrndx][4]
    return [(h[4], h[4] + h[5], data[names + h[0]:data.index(b'\0', names + h[0])].decode())
            for h in hdrs if h[1] != 8]  # SHT_NOBITS has no bytes


def ranges(a, b, gap=16):
    """The differing byte ranges of two equally long buffers (ranges closer than gap merged)."""
    out, start, end = [], None, 0
    for i in range(len(a)):
        if a[i] != b[i]:
            if start is None:
                start = i
            end = i
        elif start is not None and i - end > gap:
            out.append((start, end + 1))
            start = None
    if start is not None:
        out.append((start, end + 1))
    return out


za, zb = (zipfile.ZipFile(p) for p in sys.argv[1:3])
ia = {i.filename: i for i in za.infolist()}
ib = {i.filename: i for i in zb.infolist()}
common = sorted(set(ia) & set(ib))
print('entries %d / %d, identical %d' % (len(ia), len(ib), sum(ia[n].CRC == ib[n].CRC for n in common)))
for n in sorted(set(ia) - set(ib)):
    print('only in the first:', n)
for n in sorted(set(ib) - set(ia)):
    print('only in the second:', n)
for n in common:
    if ia[n].CRC == ib[n].CRC:
        continue
    print('differs: %s (%d -> %d bytes)' % (n, ia[n].file_size, ib[n].file_size))
    a, b = za.read(n), zb.read(n)
    if a[:4] != b'\x7fELF' or len(a) != len(b):
        continue
    secs = sections(a)
    for s, e in ranges(a, b)[:12]:
        sec = [name for lo, hi, name in secs if lo <= s < hi]
        print('    %#x-%#x (%d bytes) in %s' % (s, e, e - s, sec[0] if sec else '?'))
