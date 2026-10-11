#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Verbatim replay of redir2.bin (EP3/EP4/EP2 incl. 0x13 RLE + 0x15 flips),
then test: 0x11 fill into back buffer + 0x15 flip to it.
Usage: replay.py [ops_limit]   (default: through first frame cycle, op idx of 2nd 0x11 clear)
"""
import struct, sys, time
import os
import usb.core, usb.util

CAP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "captures", "redir2.bin")
FB = {0x000C000: 0x10, 0x01CE000: 0x20}   # byte addr -> fb65 select


def load():
    d = open(CAP, "rb").read()
    off = 0; bufs = {0: b"", 1: b""}
    while off + 13 <= len(d):
        u, dr, ln = struct.unpack("<QBI", d[off:off + 13])
        bufs[dr] += d[off + 13:off + 13 + ln]; off += 13 + ln

    def pk(buf):
        off = 0; out = []
        while off + 12 <= len(buf):
            t, ln = struct.unpack("<II", buf[off:off + 8]); ids = 4 if t == 0 else 8
            tot = 8 + ids + ln
            if off + tot > len(buf): break
            out.append((t, buf[off + 8 + ids:off + tot])); off += tot
        return out

    def bulk(p):
        ln = struct.unpack("<H", p[2:4])[0] | (struct.unpack("<H", p[8:10])[0] << 16)
        return p[0], p[10:10 + ln]

    in_lens = [len(bulk(p)[1]) or struct.unpack("<H", p[2:4])[0]
               for t, p in pk(bufs[1]) if t in (101, 104) and len(p) >= 10 and p[0] == 0x81]
    ops = []
    for t, p in pk(bufs[0]):
        if t not in (101, 104) or len(p) < 10: continue
        ep, data = bulk(p)
        if data and ep in (2, 3, 4):
            ops.append((ep, data))
    return ops, in_lens


def flip_cmd(addr):
    a = addr >> 2
    return bytes([0x15, 0x07, 0xfb, 0x65, FB[addr], 0xfc, 0xb4, 0x11,
                  0xfc, 0xb1, a & 0xff, 0xfc, 0xb2, (a >> 8) & 0xff,
                  0xfc, 0xb3, (a >> 16) & 0xff, 0xfc, 0xb4, 0x15, 0xfc, 0xb4, 0x11])


def fill_cmd(addr, rgb):
    hdr = b"\x11" + addr.to_bytes(4, "little") + bytes.fromhex("0004580200045802004000400000201c")
    return hdr + bytes(rgb) * (1024 * 600)


def main():
    ops, in_lens = load()
    assert flip_cmd(0x1CE000).hex() == "1507fb6520fcb411fcb100fcb238fcb307fcb415fcb411"
    assert flip_cmd(0x00C000).hex() == "1507fb6510fcb411fcb100fcb230fcb300fcb415fcb411"
    # stop after first frame cycle: just before the 2nd-wave 0x11 clear (EP2 op #241)
    n2 = 0; limit = len(ops)
    for i, (ep, data) in enumerate(ops):
        if ep == 2:
            n2 += 1
            if n2 == 241: limit = i; break
    if len(sys.argv) > 1: limit = int(sys.argv[1])
    print(f"{len(ops)} ops, replaying {limit}; IN lens {len(in_lens)}")
    dev = usb.core.find(idVendor=0x0711, idProduct=0x520d)
    assert dev is not None
    usb.util.claim_interface(dev, 0)
    it = iter(in_lens)
    time.sleep(10)  # 8051 boot after power-cycle
    try:
        for ep, data in ops[:limit]:
            dev.write(ep, data, 30000)
            if ep == 3 and data[0] in (0x05, 0x91, 0x1D):
                rl = next(it, 512)
                for k in range(6):
                    try:
                        dev.read(0x81, max(rl, 1), 3000); break
                    except usb.core.USBError:
                        time.sleep(1)
                else:
                    raise RuntimeError("read failed")
            if ep != 2 or len(data) < 64:
                time.sleep(0.05)
        print("replay done (Windows' black frame should be on screen). 8 s pause")
        time.sleep(8)
        from t2rle import frame_cmd
        for addr, rgb, name in ((0x1CE000, (255, 0, 0), "ch0=FF ->B"),
                                (0x00C000, (0, 255, 0), "ch1=FF ->A"),
                                (0x1CE000, (0, 0, 255), "ch2=FF ->B"),
                                (0x00C000, (255, 255, 255), "WHITE ->A")):
            fc = frame_cmd(addr, bytes(rgb) * (1024 * 600))
            t0 = time.time()
            dev.write(2, fc[:36], 5000)               # header: own transfer
            for k in range(36, len(fc), 19968):       # payload: Windows chunking
                dev.write(2, fc[k:k + 19968], 30000)
            dev.write(2, flip_cmd(addr), 5000)
            print(time.strftime("%H:%M:%S"), "frame+flip", name, "%.2fs" % (time.time() - t0), flush=True)
            time.sleep(8)
        print("done")
    finally:
        usb.util.release_interface(dev, 0)
        print("released")


if __name__ == "__main__":
    main()
