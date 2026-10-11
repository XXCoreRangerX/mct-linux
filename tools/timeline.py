# SPDX-License-Identifier: GPL-2.0-only
"""Print EP3/EP4 commands + EP2 0x11/0x13/0x15 headers of a tee capture. Usage: timeline.py [capture]"""
import os, struct, sys

CAP = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "captures", "redir2.bin")
d = open(CAP, "rb").read()
off = 0
bufs = {0: b"", 1: b""}
while off + 13 <= len(d):
    usec, direction, ln = struct.unpack("<Q B I", d[off:off + 13])
    bufs[direction] += d[off + 13:off + 13 + ln]
    off += 13 + ln


def packets(buf):
    off = 0
    out = []
    while True:
        if off + 12 > len(buf):
            break
        typ, ln = struct.unpack("<II", buf[off:off + 8])
        ids = 4 if typ == 0 else 8
        if off + 8 + ids > len(buf):
            break
        total = 8 + ids + ln
        if off + total > len(buf):
            break
        out.append((typ, buf[off + 8 + ids:off + total]))
        off += total
    return out


for typ, p in packets(bufs[0]):
    if typ not in (101, 104) or len(p) < 10:
        continue
    ep = p[0]
    ln = struct.unpack("<H", p[2:4])[0] | (struct.unpack("<H", p[8:10])[0] << 16)
    data = p[10:10 + ln]
    if not data:
        continue
    if ep == 0x03:
        print("EP3 cmd=0x%02x len=%d %s" % (data[0], len(data), data[:16].hex(" ")))
    elif ep == 0x02 and data[0] in (0x11, 0x13, 0x15) and len(data) <= 64:
        print("EP2 cmd=0x%02x len=%d %s" % (data[0], len(data), data[:36].hex(" ")))
    elif ep == 0x04:
        print("EP4 len=%d %s" % (len(data), data.hex(" ")))
