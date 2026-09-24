#!/usr/bin/env python3
"""Throughput of full-frame 0x13 updates for different payload transfer sizes.
Each variant shows the same photo with a different tint so the result can be
checked on the panel. Usage: bench.py photo.png [frames]"""
import sys, time
import t2dev, t2rle
from PIL import Image

W, H = t2dev.W, t2dev.H


def tinted(rgb, tint):
    out = bytearray(rgb)
    for c in range(3):
        if tint[c] != 1.0:
            out[c::3] = bytes(int(v * tint[c]) for v in rgb[c::3])
    return bytes(out)


def rot180(rgb):
    return b"".join(reversed([rgb[i:i + 3] for i in range(0, len(rgb), 3)]))


def header(addr, paylen):
    a = addr << 8
    return (b"\x13" + a.to_bytes(4, "little") * 2 + b"\x00"
            + bytes.fromhex("0004580200045802004000400000003900")
            + (W * H * 3).to_bytes(3, "little") + paylen.to_bytes(3, "little")
            + bytes.fromhex("300528"))


def run(t, pay, chunk, frames):
    addrs = (t2dev.FB_A, t2dev.FB_B)
    t0 = time.time()
    for n in range(frames):
        addr = addrs[n % 2]
        t.out(2, header(addr, len(pay)))
        for k in range(0, len(pay), chunk):
            t.out(2, pay[k:k + chunk], 30000)
        t.flip(addr)
    dt = time.time() - t0
    return frames / dt, len(pay) * frames / dt / 1e6


if __name__ == "__main__":
    photo = Image.open(sys.argv[1]).convert("RGB").resize((W, H)).tobytes()
    frames = int(sys.argv[2]) if len(sys.argv) > 2 else 20
    t = t2dev.T2()
    variants = [("red tint", 19968, (1.0, 0.45, 0.45)), ("green tint", 65536, (0.45, 1.0, 0.45)),
                ("blue tint", 262144, (0.45, 0.45, 1.0)), ("no tint", 1 << 24, (1.0, 1.0, 1.0))]
    for name, chunk, tint in variants:
        pay = t2rle.encode(rot180(tinted(photo, tint)), W, H)
        fps, mbs = run(t, pay, chunk, frames)
        print(f"{name:10s} chunk {min(chunk, len(pay)):8d}  payload {len(pay)/1e3:7.1f} kB  "
              f"{fps:5.1f} fps  {mbs:5.1f} MB/s", flush=True)
        time.sleep(4)
    import os
    noise = t2rle.encode(os.urandom(W * H * 3), W, H)
    fps, mbs = run(t, noise, 1 << 24, 10)
    print(f"noise (worst case, whole-frame transfer): {len(noise)/1e3:.0f} kB  {fps:.1f} fps  {mbs:.1f} MB/s")
    t.show(photo)
    t.close()
