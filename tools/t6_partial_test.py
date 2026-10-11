# SPDX-License-Identifier: GPL-2.0-only
"""Partial updates on the T6: full frames into two buffers, an empty flip, then
a session 7 band written into the hidden buffer and flipped to. 1080p60, NV12."""
import os
import sys, struct, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import t6dev
W, H = 1920, 1080
FB = (0x0, 0xFFFEC0, 0x1FFFD80)
COL = {"blue": (41, 240, 110), "red": (81, 90, 240), "green": (145, 54, 34)}
FRAG = t6dev.FRAG
def planes(c, rows):
    y, cb, cr = COL[c]
    return bytes([y]) * (W * rows) + bytes([cb, cr]) * (W * rows // 4)
def session(t, data, dest, stype=0, extra=None):
    total = len(data)
    if extra is not None:                         # one-shot session with custom dwords
        t.dev.write(2, struct.pack("<8I", stype, total, *extra), 5000)
        t.dev.write(2, data, 10000)
        return
    for off in range(0, total, FRAG):
        frag = data[off:off + FRAG]
        t.dev.write(2, struct.pack("<8I", stype, total, dest, len(frag), off,
                                   1 if off + FRAG < total else 0, 0, 0), 5000)
        t.dev.write(2, frag, 5000)
def flip(t, fb, payload=b""):
    t.seq += 1
    p0 = fb + 0x30
    hdr = struct.pack("<4I2H3I I3I", 3, len(payload), t.seq, 6, W, W, p0, p0 + W * H, p0, 6, 0, 0, 0)
    session(t, hdr + payload, fb)
t = t6dev.T6()
t.set_mode(t.find_mode(1920, 1080, 60)); t.cout(0x31, 0, 0); t.output(True); time.sleep(2)
print("1 blue (full frame into buffer 0)", flush=True)
flip(t, FB[0], planes("blue", H)); time.sleep(2.5)
print("2 red (full frame into buffer 1)", flush=True)
flip(t, FB[1], planes("red", H)); time.sleep(2.5)
print("3 empty flip back to buffer 0", flush=True)
flip(t, FB[0]); time.sleep(2.5)
print("4 session-7 green band into buffer 1 (hidden)", flush=True)
y0, rows = 400, 280
data = planes("green", rows)
yaddr = FB[1] + 0x30 + y0 * W
uvaddr = FB[1] + 0x30 + W * H + (y0 // 2) * W
session(t, data, 0, stype=7, extra=(yaddr, uvaddr, W | (W << 16), W | (rows << 16), 0, 0))
time.sleep(2.5)
print("5 empty flip to buffer 1", flush=True)
flip(t, FB[1]); time.sleep(2)
print("acks:", [a[12:20].hex() for a in t.acks(300)], flush=True)
t.close()
