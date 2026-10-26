"""Decode collector .dds files (BC1, BC4, RGBA8) to PNG on a grey background, no dependencies.
Usage: python -I dds_view.py <out_dir> <file.dds>...   Prints the size and format of each."""
import re, struct, sys, zlib, os


def png(path, w, h, rgb):
    raw = b"".join(b"\0" + bytes(rgb[y * w * 3:(y + 1) * w * 3]) for y in range(h))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def c565(c):
    return ((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31


def bc1(d, w, h):
    px = [(0, 0, 0, 0)] * (w * h)
    o = 0
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            a, b, bits = struct.unpack_from("<HHI", d, o); o += 8
            ca, cb = c565(a), c565(b)
            if a > b:
                pal = [ca + (255,), cb + (255,), tuple((2 * x + y) // 3 for x, y in zip(ca, cb)) + (255,),
                       tuple((x + 2 * y) // 3 for x, y in zip(ca, cb)) + (255,)]
            else:
                pal = [ca + (255,), cb + (255,), tuple((x + y) // 2 for x, y in zip(ca, cb)) + (255,), (0, 0, 0, 0)]
            for i in range(16):
                x, y = bx + i % 4, by + i // 4
                if x < w and y < h:
                    px[y * w + x] = pal[(bits >> (2 * i)) & 3]
    return px


def bc4(d, w, h):
    px = [(0, 0, 0, 0)] * (w * h)
    o = 0
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            r0, r1 = d[o], d[o + 1]
            bits = int.from_bytes(d[o + 2:o + 8], "little"); o += 8
            if r0 > r1:
                pal = [r0, r1] + [((6 - i) * r0 + (1 + i) * r1) // 7 for i in range(6)]
            else:
                pal = [r0, r1] + [((4 - i) * r0 + (1 + i) * r1) // 5 for i in range(4)] + [0, 255]
            for i in range(16):
                x, y = bx + i % 4, by + i // 4
                if x < w and y < h:
                    v = pal[(bits >> (3 * i)) & 7]
                    px[y * w + x] = (v, v, v, 255)
    return px


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for path in sys.argv[2:]:
        m = re.search(r"([0-9a-f]{16})_(\d+)x(\d+)_(\w+)\.dds$", path)
        if not m:
            continue
        name, w, h, fmt = m.group(1), int(m.group(2)), int(m.group(3)), m.group(4)
        d = open(path, "rb").read()[148:]
        if fmt.startswith("Bc1"):
            px = bc1(d, w, h)
        elif fmt.startswith("Bc4"):
            px = bc4(d, w, h)
        elif fmt.startswith("R8G8B8A8"):
            px = [tuple(d[i:i + 4]) for i in range(0, w * h * 4, 4)]
        else:
            print(name, fmt, "skipped"); continue
        rgb = bytearray(w * h * 3)
        for i, (r, g, b, a) in enumerate(px):
            for k, c in enumerate((r, g, b)):
                rgb[i * 3 + k] = (c * a + 70 * (255 - a)) // 255
        png(os.path.join(out, f"{name}_{w}x{h}_{fmt}.png"), w, h, rgb)
        print(name, w, h, fmt)


main()
