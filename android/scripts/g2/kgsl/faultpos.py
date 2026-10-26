"""Where the CP (BR and BV) stopped in a KGSL fault snapshot and which packet spans that point.
python faultpos.py <snap>..."""
import struct
import sys

NAMES = {0x43: "SET_DRAW_STATE", 0x5c: "CONTEXT_REG_BUNCH", 0x5d: "NON_CONTEXT_REG_BUNCH",
         0x3f: "INDIRECT_BUFFER", 0x34: "LOAD_STATE6_FRAG", 0x32: "LOAD_STATE6_GEOM", 0x36: "LOAD_STATE6",
         0x38: "DRAW_INDX_OFFSET", 0x17: "THREAD_CONTROL", 0x10: "NOP", 0x46: "EVENT_WRITE",
         0x26: "WAIT_FOR_IDLE", 0x33: "EXEC_CS", 0x2c: "BLIT", 0x65: "SET_MARKER", 0x47: "COND_REG_EXEC",
         0x3d: "MEM_WRITE", 0x63: "SET_MODE", 0x14: "WAIT_REG_MEM", 0x1b: "BV_BR_COUNT_OPS"}


def load(path):
    data = open(path, "rb").read()
    off = 12
    blobs, regs = [], {}
    while off + 8 <= len(data):
        m, sid, size = struct.unpack_from("<HHI", data, off)
        if m != 0xABCD or size == 0:
            break
        body = off + 8
        if sid == 0x0402:
            a, pt, n = struct.unpack_from("<QQQ", data, body)
            blobs.append((a, n, body + 24, True))
        elif sid == 0x0B02:
            t, a, pt, n = struct.unpack_from("<iQQQ", data, body)
            blobs.append((a, n, body + 28, False))
        elif sid == 0x0202:
            b = data[body: off + size]
            w = struct.unpack_from("<%dI" % (len(b) // 4), b)
            i = 0
            while i < len(w):
                e = w[i]
                if e & 0x80000000:
                    if i + 1 >= len(w):
                        break
                    regs[e & 0x7fffffff] = w[i + 1]
                    i += 2
                else:
                    if i + 1 >= len(w):
                        break
                    c = w[i + 1]
                    if c > 100000 or i + 2 + c > len(w):
                        break
                    for k in range(c):
                        regs[e + k] = w[i + 2 + k]
                    i += 2 + c
        if sid == 0xFFFF:
            break
        off += size
    return data, blobs, regs


def words(data, blobs, addr, n):
    for a, cnt, doff, ib in sorted(blobs, key=lambda b: not b[3]):
        if a <= addr and addr + n * 4 <= a + cnt * 4:
            return list(struct.unpack_from("<%dI" % n, data, doff + (addr - a)))
    return None


def ib_size(data, blobs, base):
    # An IB section has the IB's own size; a GPU object at the same address is the whole BO.
    sizes = [(not ib, cnt) for a, cnt, doff, ib in blobs if a == base]
    if not sizes:
        return None
    first = min(sizes)[0]
    return max(cnt for kind, cnt in sizes if kind == first)


def at(data, blobs, base, rem, label):
    if not base:
        print("  %s: no IB" % label)
        return
    size = ib_size(data, blobs, base)
    if size is None:
        print("  %s: IB 0x%x rem 0x%x (IB not in the snapshot)" % (label, base, rem))
        return
    pos = size - rem
    fetch = base + pos * 4
    print("  %s: IB 0x%x size 0x%x rem 0x%x -> fetch +0x%x = 0x%x (in 1K 0x%x, in 2K 0x%x)" % (
        label, base, size, rem, pos, fetch, fetch & 1023, fetch & 2047))
    w = words(data, blobs, base, size)
    i = 0
    while i < size:
        h = w[i]
        t = h >> 28
        ln = (h & 0x7f) if t == 4 else (h & 0x3fff) if t == 7 else 0
        if i <= pos < i + 1 + ln or (pos - 24 <= i < pos):
            a = base + i * 4
            name = NAMES.get((h >> 16) & 0x7f, "op 0x%02x" % ((h >> 16) & 0x7f)) if t == 7 else (
                "pkt4 0x%05x" % ((h >> 8) & 0x3ffff) if t == 4 else "data %08x" % h)
            cross = [str(b) for b in (1024, 2048, 4096) if (a // b) != ((a + (1 + ln) * 4 - 1) // b)]
            print("    +0x%04x %-22s len %3d%s%s" % (i, name, ln, ("  crosses " + ",".join(cross)) if cross else "",
                                                     "  <== fetch inside" if i < pos < i + 1 + ln else ""))
        if i > pos:
            break
        i += 1 + ln


def main():
    for path in sys.argv[1:]:
        data, blobs, regs = load(path)
        print("== %s: CP_PROTECT_STATUS 0x%x" % (path.split("/")[-1].split("\\")[-1], regs.get(0x824, 0)))
        for label, b_lo, b_hi, r1, i2_lo, i2_hi, r2 in (
                ("BR", 0x928, 0x929, 0x92a, 0x92b, 0x92c, 0x92d),
                ("BV", 0xa6d, 0xa6e, 0xa70, 0xa71, 0xa72, 0xa74)):
            ib1 = regs.get(b_lo, 0) | (regs.get(b_hi, 0) << 32)
            ib2 = regs.get(i2_lo, 0) | (regs.get(i2_hi, 0) << 32)
            at(data, blobs, ib1, regs.get(r1, 0), label + " IB1")
            if ib2:
                at(data, blobs, ib2, regs.get(r2, 0), label + " IB2")


if __name__ == "__main__":
    main()
