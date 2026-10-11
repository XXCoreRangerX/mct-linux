#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""YUV 4:4:4 (surface format 11) vs NV12 on the T6.

Shows a detail image (1-pixel coloured lines, red strokes on blue) as NV12,
then as YUV 4:4:4, then NV12 again, 5 s each; then, with "motion", a moving
block in 4:4:4 at full speed to check for tearing.
Planes per the vendor driver: Y, U, V, full size, stride align32(width).
"""
import os, sys, struct, time
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import t6dev

W, H = 1920, 1080
FB = (0x0, 0xFFFEC0, 0x1FFFD80)
S = (W + 31) // 32 * 32


def detail():
    img = np.zeros((H, W, 3), np.uint8)
    img[...] = (120, 40, 20)                          # B, G, R: dark blue
    for i, c in enumerate(((0, 0, 255), (0, 255, 0), (255, 0, 0), (0, 255, 255))):
        x0 = 100 + i * 200
        for x in range(x0, x0 + 120, 2):              # 1-pixel lines, 1 pixel apart
            img[100:500, x] = c
    for y in range(600, 1000, 6):                     # red "text" strokes on blue
        img[y:y + 2, 100:900] = (0, 0, 255)
        img[y:y + 4, 1000:1004 + (y % 50) * 10] = (0, 0, 255)
    return img


def yuv(img):
    b, g, r = (img[..., i].astype(np.int32) for i in range(3))
    y = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16
    u = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128
    v = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128
    return y.astype(np.uint8), u.astype(np.uint8), v.astype(np.uint8)


def nv12(img):
    y, u, v = yuv(img)
    sub = lambda p: p.reshape(H // 2, 2, W // 2, 2).astype(np.int32).mean(axis=(1, 3)).round().astype(np.uint8)
    return y.tobytes() + np.stack([sub(u), sub(v)], -1).tobytes()


def yuv444(img):
    planes = []
    for p in yuv(img):
        pad = np.zeros((H, S), np.uint8)
        pad[:, :W] = p
        planes.append(pad.tobytes())
    return b"".join(planes)


def send(t, fb, payload, fmt):
    t.seq += 1
    p0 = fb + 0x30
    if fmt == 6:
        hdr = struct.pack("<4I2H3I I3I", 3, len(payload), t.seq, 6, W, W, p0, p0 + W * H, p0, 6, 0, 0, 0)
    else:
        hdr = struct.pack("<4I2H3I I3I", 3, len(payload), t.seq, 11, S, S, p0, p0 + S * H, p0 + 2 * S * H,
                          11, 0, 0, 0)
    data = hdr + payload
    for off in range(0, len(data), t6dev.FRAG):
        frag = data[off:off + t6dev.FRAG]
        t.dev.write(2, struct.pack("<8I", 0, len(data), fb, len(frag), off,
                                   1 if off + t6dev.FRAG < len(data) else 0, 0, 0), 5000)
        t.dev.write(2, frag, 5000)


t = t6dev.T6()
t.set_mode(t.find_mode(W, H, 60)); t.cout(0x31, 0, 0); t.output(True); time.sleep(1.5)
img = detail()
if "motion" not in sys.argv:
    print("1: NV12", flush=True); send(t, FB[0], nv12(img), 6); time.sleep(5)
    print("2: YUV 4:4:4", flush=True); send(t, FB[1], yuv444(img), 11); time.sleep(5)
    print("3: NV12", flush=True); send(t, FB[2], nv12(img), 6); time.sleep(3)
else:
    frames = []
    for i in range(24):
        f = np.zeros((H, W, 3), np.uint8); f[...] = (90, 40, 40)
        x0 = (i * (W - 300)) // 23
        f[200:880, x0:x0 + 300] = 255
        frames.append(yuv444(f))
    frames += frames[-2:0:-1]
    n, t0 = 0, time.time()
    while time.time() - t0 < 6:
        send(t, FB[n % 3], frames[n % len(frames)], 11)
        t.acks(1)
        n += 1
    print(f"4:4:4 motion: {n} frames in 6 s = {n / 6:.1f} fps", flush=True)
t.close()
