# SPDX-License-Identifier: GPL-2.0-only
"""Load a tee capture -> ordered list of (usec, ep, data) OUT ops (EP2/3/4)."""
import struct


def load(path):
    d = open(path, "rb").read()
    off = 0; bufs = {0: b"", 1: b""}
    while off + 13 <= len(d):
        u, dr, ln = struct.unpack("<QBI", d[off:off + 13]); bufs[dr] += d[off + 13:off + 13 + ln]; off += 13 + ln
    ops = []; off = 0; buf = bufs[0]
    while off + 12 <= len(buf):
        t, ln = struct.unpack("<II", buf[off:off + 8]); ids = 4 if t == 0 else 8
        tot = 8 + ids + ln
        if off + tot > len(buf): break
        p = buf[off + 8 + ids:off + tot]; off += tot
        if t in (101, 104) and len(p) >= 10:
            n = struct.unpack("<H", p[2:4])[0] | (struct.unpack("<H", p[8:10])[0] << 16)
            data = p[10:10 + n]
            if data and p[0] in (2, 3, 4):
                ops.append((p[0], data))
    return ops


def frames(ops):
    """Yield (header, payload) for EP2 0x13 frames (payload = following EP2 chunks)."""
    i = 0
    while i < len(ops):
        ep, d = ops[i]
        if ep == 2 and d[0] == 0x13 and len(d) == 36:
            cl = int.from_bytes(d[30:33], "little"); pay = b""; j = i + 1
            while len(pay) < cl and j < len(ops):
                if ops[j][0] == 2: pay += ops[j][1]
                j += 1
            yield d, pay
        i += 1
