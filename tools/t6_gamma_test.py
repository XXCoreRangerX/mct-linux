#!/usr/bin/env python3
"""Does this T6 apply gamma tables (requests 0x1d / 0x1c)? The vendor driver
only uses them from feature level 8 (0xb0 index 4); the 5600 reports 6.
Shows the test card, then: all tables inverted, one table inverted, off."""
import os
import sys, struct, time
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import t6dev

W, H = 1920, 1080
FB = 0x0
t = t6dev.T6()
print("feature level (b0 index 4):", t.cin(0xb0, 0, 4, 4).hex(), flush=True)
t.set_mode(t.find_mode(W, H, 60)); t.cout(0x31, 0, 0); t.output(True); time.sleep(1.5)

bgr = np.frombuffer(t6dev.test_card(W, H, 3), np.uint8).reshape(H, W, 3).astype(np.int32)
b, g, r = bgr[..., 0], bgr[..., 1], bgr[..., 2]
y = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16
s = lambda p: p.reshape(H // 2, 2, W // 2, 2).sum(axis=(1, 3))
B, G, R = s(b), s(g), s(r)
u = ((-38 * R - 74 * G + 112 * B + 512) >> 10) + 128
v = ((112 * R - 94 * G - 18 * B + 512) >> 10) + 128
pay = y.astype(np.uint8).tobytes() + np.stack([u, v], -1).astype(np.uint8).tobytes()
t.seq += 1; p0 = FB + 0x30
data = struct.pack("<4I2H3I I3I", 3, len(pay), t.seq, 6, W, W, p0, p0 + W * H, p0, 6, 0, 0, 0) + pay
for off in range(0, len(data), t6dev.FRAG):
    frag = data[off:off + t6dev.FRAG]
    t.dev.write(2, struct.pack("<8I", 0, len(data), FB, len(frag), off,
                               1 if off + t6dev.FRAG < len(data) else 0, 0, 0), 5000)
    t.dev.write(2, frag, 5000)

ident = bytes(range(256)); inv = bytes(255 - i for i in range(256))
out = t.out << 8
def tables(t100, t200, t400, on=True):
    for idx, tab in ((0x100, t100), (0x200, t200), (0x400, t400)):
        t.cout(0x1d, out, idx, tab)
    t.cout(0x1c, out | (2 if on else 0), 0)

print("1: normal test card", flush=True); time.sleep(4)
print("2: all gamma tables inverted", flush=True)
tables(inv, inv, inv); time.sleep(4)
print("3: only table 0x400 inverted", flush=True)
tables(ident, ident, inv); time.sleep(4)
print("4: gamma off", flush=True)
tables(ident, ident, ident, on=False); time.sleep(2)
t.close()
