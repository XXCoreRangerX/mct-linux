# SPDX-License-Identifier: GPL-2.0-only
"""Session 7 rectangle write into a hidden NV12 frame buffer, then an empty
flip to it: a green box should appear on blue. 1080p60."""
import os
import sys, struct, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import t6dev
W, H = 1920, 1080
FB = (0x0, 0xFFFEC0, 0x1FFFD80)
COL = {"blue": (41, 240, 110), "green": (145, 54, 34)}
def planes(c, w, rows):
    y, cb, cr = COL[c]
    return bytes([y]) * (w * rows) + bytes([cb, cr]) * (w * rows // 4)
def frame(t, fb, payload):
    t.seq += 1; p0 = fb + 0x30
    data = struct.pack("<4I2H3I I3I", 3, len(payload), t.seq, 6, W, W, p0, p0 + W * H, p0, 6, 0, 0, 0) + payload
    for off in range(0, len(data), t6dev.FRAG):
        frag = data[off:off + t6dev.FRAG]
        t.dev.write(2, struct.pack("<8I", 0, len(data), fb, len(frag), off, 1 if off + t6dev.FRAG < len(data) else 0, 0, 0), 5000)
        t.dev.write(2, frag, 5000)
t = t6dev.T6()
t.set_mode(t.find_mode(1920, 1080, 60)); t.cout(0x31, 0, 0); t.output(True); time.sleep(1.5)
frame(t, FB[0], planes("blue", W, H)); time.sleep(0.5)
frame(t, FB[1], planes("blue", W, H)); time.sleep(2)     # buffer 1 on screen
x0, y0, w, h = 600, 300, 300, 400                          # green rect into hidden buffer 0
base = FB[0] + 0x30
data = planes("green", w, h)
t.dev.write(2, struct.pack("<4I4H2I", 7, len(data), base + y0 * W + x0, base + W * H + (y0 // 2) * W + x0, W, W, w, h, 0, 0), 5000)
t.dev.write(2, data, 5000)
time.sleep(2.5)
t.seq += 1; p0 = FB[0] + 0x30                               # empty flip to buffer 0
t.dev.write(2, struct.pack("<8I", 0, 48, FB[0], 48, 0, 0, 0, 0), 5000)
t.dev.write(2, struct.pack("<4I2H3I I3I", 3, 0, t.seq, 6, W, W, p0, p0 + W * H, p0, 6, 0, 0, 0), 5000)
print("done", flush=True)
t.close()
