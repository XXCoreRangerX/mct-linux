#!/usr/bin/env python3
"""Hardware cursor experiments (T2). Pixel format: bytes A,B,G,R (DRM RGBA8888), 64x64."""
import sys, t2dev

HDR = bytes.fromhex("1200000000004000004000000125")   # 0x12 cursor upload, verbatim from Windows


def img(fn):
    out = bytearray()
    for y in range(64):
        for x in range(64):
            a, r, g, b = fn(x, y)
            out += bytes((a, b, g, r))
    return bytes(out)


def upload(t, data):
    t.out(2, HDR); t.out(2, data)


def pos_full(t, x, y, en=0x07):
    t.out(4, bytes([0x05, 0x12, 0x34, 0x40, 0x35, 0x40, 0x38, x & 0xff, 0x39, x >> 8,
                    0x3a, y & 0xff, 0x3b, y >> 8, 0x44, 0, 0x45, 0, 0x36, en]))


def pos(t, x, y, en=0x07, ox=0, oy=0):
    t.out(4, bytes([0x05, 0x0e, 0x38, x & 0xff, 0x39, x >> 8, 0x3a, y & 0xff, 0x3b, y >> 8,
                    0x44, ox, 0x45, oy, 0x36, en]))


def reg36(t, v):
    t.out(4, bytes([0x05, 0x02, 0x36, v]))


def marker(x, y):
    if x < 12 and y < 12:
        return (0xfe, 255, 255, 0)        # yellow in the image's first rows/cols
    return (0xfe, 255, 0, 255)            # magenta


if __name__ == "__main__":
    t = t2dev.T2()
    cmd = sys.argv[1]
    if cmd == "t1":
        upload(t, img(marker)); pos_full(t, 512, 300)
    elif cmd == "pos":
        pos(t, *[int(v) for v in sys.argv[2:]])
    elif cmd == "r36":
        reg36(t, int(sys.argv[2], 16))
    t.close()
