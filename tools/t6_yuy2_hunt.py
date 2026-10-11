#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Which layout does surface format 0 (4:2:2) expect? Shows 8 variants,
4 s each; variant N has N white squares at the top left."""
import os, sys, struct, time
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import t6dev

W, H = 1920, 1080
FB = (0x0, 0xFFFEC0, 0x1FFFD80)


def image(n):
    img = np.zeros((H, W, 3), np.uint8)
    bars = [(0, 0, 255), (0, 255, 0), (255, 0, 0), (255, 255, 255)]   # B, G, R
    bh = H // 5
    for i, c in enumerate(bars):
        img[i * bh:(i + 1) * bh] = c
    img[4 * bh:] = np.linspace(0, 255, W, dtype=np.uint8)[None, :, None]  # grey ramp
    for x in range(1000, 1400, 4):                                       # fine stripes
        img[:, x:x + 2] = 0
    for i in range(n):                                                    # N squares
        img[20:80, 20 + i * 80:80 + i * 80] = 255
        img[20:80, 20 + i * 80 + 60:80 + i * 80] = 0
    return img


def yuv422(img):
    b, g, r = (img[..., i].astype(np.int32) for i in range(3))
    y = (77 * r + 150 * g + 29 * b + 128) >> 8
    u = np.clip(((-43 * r - 85 * g + 128 * b + 128) >> 8) + 128, 0, 255)
    v = np.clip(((128 * r - 107 * g - 21 * b + 128) >> 8) + 128, 0, 255)
    u2 = (u[:, 0::2] + u[:, 1::2] + 1) // 2
    v2 = (v[:, 0::2] + v[:, 1::2] + 1) // 2
    return y.astype(np.uint8), u2.astype(np.uint8), v2.astype(np.uint8)


def packed(img, order):
    y, u, v = yuv422(img)
    comp = {"Y0": y[:, 0::2], "Y1": y[:, 1::2], "U": u, "V": v}
    out = np.stack([comp[c] for c in order], -1)
    return out.tobytes()


def nv16(img, swap=False):
    y, u, v = yuv422(img)
    uv = np.stack([v, u] if swap else [u, v], -1)
    return y.tobytes() + uv.tobytes()


def send(t, fb, payload, pitch0, pitch1, p1off):
    t.seq += 1
    p0 = fb + 0x30
    hdr = struct.pack("<4I2H3I I3I", 3, len(payload), t.seq, 0, pitch0, pitch1, p0,
                      p0 + p1off, 0, 0, 0, 0, 0)
    data = hdr + payload
    for off in range(0, len(data), t6dev.FRAG):
        frag = data[off:off + t6dev.FRAG]
        t.dev.write(2, struct.pack("<8I", 0, len(data), fb, len(frag), off,
                                   1 if off + t6dev.FRAG < len(data) else 0, 0, 0), 5000)
        t.dev.write(2, frag, 5000)


variants = [
    ("packed YUYV, pitch 2W", lambda i: packed(i, ["Y0", "U", "Y1", "V"]), 2 * W, 0, 0),
    ("packed UYVY, pitch 2W", lambda i: packed(i, ["U", "Y0", "V", "Y1"]), 2 * W, 0, 0),
    ("packed YVYU, pitch 2W", lambda i: packed(i, ["Y0", "V", "Y1", "U"]), 2 * W, 0, 0),
    ("packed VYUY, pitch 2W", lambda i: packed(i, ["V", "Y0", "U", "Y1"]), 2 * W, 0, 0),
    ("packed YUYV, both pitches 2W", lambda i: packed(i, ["Y0", "U", "Y1", "V"]), 2 * W, 2 * W, 0),
    ("NV16 (Y, then UV full height), pitch W", lambda i: nv16(i), W, W, W * H),
    ("NV61 (Y, then VU full height), pitch W", lambda i: nv16(i, True), W, W, W * H),
    ("packed YUYV, pitch 2W, plane 1 after the image", lambda i: packed(i, ["Y0", "U", "Y1", "V"]), 2 * W, 2 * W, 2 * W * H),
]

t = t6dev.T6()
t.set_mode(t.find_mode(W, H, 60)); t.cout(0x31, 0, 0); t.output(True); time.sleep(1.5)
for n, (name, make, p0, p1, off) in enumerate(variants, 1):
    print(f"{n}: {name}", flush=True)
    payload = make(image(n))
    if off and len(payload) < off + W * H:          # room for plane 1 if it points past the data
        payload += bytes(off + 2 * W * H - len(payload))
    send(t, FB[n % 3], payload, p0, p1, off)
    time.sleep(6)
t.close()
