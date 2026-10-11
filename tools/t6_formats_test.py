#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Packed surface formats on the T6: RGB32 (8), BGR24 (9), YUYV 4:2:2 (0).
For each: the colour detail image for 4 s, then a moving block for 4 s.
1080p60; full frames into three buffers with the vendor recipe."""
import os, sys, struct, time
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import t6dev

W, H = 1920, 1080
FB = (0x0, 0xFFFEC0, 0x1FFFD80)


def detail():
    img = np.zeros((H, W, 3), np.uint8)
    img[...] = (120, 40, 20)                          # B, G, R
    for i, c in enumerate(((0, 0, 255), (0, 255, 0), (255, 0, 0), (0, 255, 255))):
        for x in range(100 + i * 200, 220 + i * 200, 2):
            img[100:500, x] = c
    for y in range(600, 1000, 6):
        img[y:y + 2, 100:900] = (0, 0, 255)
    img[100:500, 1100:1800] = np.linspace(0, 255, 700, dtype=np.uint8)[None, :, None]  # grey ramp
    return img


def block(i):
    img = np.zeros((H, W, 3), np.uint8); img[...] = (90, 40, 40)
    x0 = (i * (W - 300)) // 23
    img[200:880, x0:x0 + 300] = 255
    return img


def pack(img, fmt):
    if fmt == 8:                                      # B, G, R, X
        out = np.zeros((H, W, 4), np.uint8); out[..., :3] = img
        return out.tobytes(), W * 4
    if fmt == 9:                                      # B, G, R, rows aligned to 32 px
        pitch = (W + 31) // 32 * 96
        out = np.zeros((H, pitch), np.uint8); out[:, :W * 3] = img.reshape(H, W * 3)
        return out.tobytes(), pitch
    b, g, r = (img[..., i].astype(np.int32) for i in range(3))   # YUYV, BT.601 limited
    y = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16
    u = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128
    v = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128
    out = np.empty((H, W // 2, 4), np.uint8)
    out[..., 0] = y[:, 0::2]
    out[..., 1] = ((u[:, 0::2] + u[:, 1::2] + 1) // 2)
    out[..., 2] = y[:, 1::2]
    out[..., 3] = ((v[:, 0::2] + v[:, 1::2] + 1) // 2)
    return out.tobytes(), W * 2


def send(t, fb, payload, pitch, fmt):
    t.seq += 1
    p0 = fb + 0x30
    hdr = struct.pack("<4I2H3I I3I", 3, len(payload), t.seq, fmt, pitch, 0, p0, p0, 0, fmt, 0, 0, 0)
    data = hdr + payload
    for off in range(0, len(data), t6dev.FRAG):
        frag = data[off:off + t6dev.FRAG]
        t.dev.write(2, struct.pack("<8I", 0, len(data), fb, len(frag), off,
                                   1 if off + t6dev.FRAG < len(data) else 0, 0, 0), 5000)
        t.dev.write(2, frag, 5000)


t = t6dev.T6()
t.set_mode(t.find_mode(W, H, 60)); t.cout(0x31, 0, 0); t.output(True); time.sleep(1.5)
moving = [block(i) for i in range(24)]
moving += moving[-2:0:-1]
for name, fmt in (("RGB32", 8), ("BGR24", 9), ("YUYV 4:2:2", 0)):
    print(f"{name}: detail image", flush=True)
    p, pitch = pack(detail(), fmt)
    send(t, FB[0], p, pitch, fmt)
    time.sleep(4)
    frames = [pack(f, fmt) for f in moving]
    n, t0 = 0, time.time()
    while time.time() - t0 < 4:
        p, pitch = frames[n % len(frames)]
        send(t, FB[n % 3], p, pitch, fmt)
        t.acks(1)
        n += 1
    print(f"{name}: motion {n / 4:.0f} fps", flush=True)
t.close()
