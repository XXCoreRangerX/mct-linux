#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""List HID class control requests (GET/SET_REPORT etc.) from a tee.py usbredir log.
Usage: redir_hid.py capture.bin [--all]"""
import struct, sys

REQ = {0x01: "GET_REPORT", 0x02: "GET_IDLE", 0x03: "GET_PROTOCOL", 0x09: "SET_REPORT",
       0x0a: "SET_IDLE", 0x0b: "SET_PROTOCOL", 0x06: "GET_DESCRIPTOR"}
RTYPE = {1: "input", 2: "output", 3: "feature"}


def records(path):
    d = open(path, "rb").read(); off = 0
    bufs = {0: bytearray(), 1: bytearray()}; stamps = {0: [], 1: []}
    while off + 13 <= len(d):
        us, dr, ln = struct.unpack("<QBI", d[off:off + 13])
        stamps[dr].append((len(bufs[dr]), us)); bufs[dr] += d[off + 13:off + 13 + ln]; off += 13 + ln
    return bufs, stamps


def packets(buf, stamps):
    off = 0; si = 0
    while off + 12 <= len(buf):
        t, ln = struct.unpack("<II", buf[off:off + 8]); ids = 4 if t == 0 else 8
        tot = 8 + ids + ln
        if off + tot > len(buf):
            break
        while si + 1 < len(stamps) and stamps[si + 1][0] <= off:
            si += 1
        yield stamps[si][1], t, buf[off + 8 + ids:off + tot]
        off += tot


if __name__ == "__main__":
    bufs, stamps = records(sys.argv[1])
    show_all = "--all" in sys.argv
    t0 = None
    for direction in (0, 1):
        for us, t, p in packets(bufs[direction], stamps[direction]):
            if t != 100 or len(p) < 10:         # control_packet
                continue
            ep, req, rtype, status, value, index, length = struct.unpack("<BBBBHHH", p[:10])
            data = p[10:]
            if not show_all and (rtype & 0x60) != 0x20:   # class requests only
                continue
            t0 = t0 or us
            name = REQ.get(req, hex(req))
            kind = f"{RTYPE.get(value >> 8, '?')} {value & 0xff:#04x}" if req in (1, 9) else f"value {value:#06x}"
            who = "host->dev" if direction == 0 else "dev->host"
            if (rtype & 0x80) and direction == 0:
                continue                         # IN request without data yet; the reply carries it
            print(f"{(us - t0) / 1e6:8.3f}s {who} {name:11s} {kind:14s} len {length:3d} {data.hex(' ')}")
