"""Partial updates in YUV 4:4:4 (surface format 11): rectangle writes into
the three planes of a hidden buffer, then a flip. 1080p60."""
import os, sys, struct, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import t6dev
W, H = 1920, 1080
S = W
FB = (0x0, 0xFFFEC0, 0x1FFFD80)
BLUE, GREEN = (41, 240, 110), (145, 54, 34)
def plane444(c, w, h):
    return b"".join(bytes([v]) * (w * h) for v in c)
def session(t, data, dest):
    for off in range(0, len(data), t6dev.FRAG):
        frag = data[off:off + t6dev.FRAG]
        t.dev.write(2, struct.pack("<8I", 0, len(data), dest, len(frag), off,
                                   1 if off + t6dev.FRAG < len(data) else 0, 0, 0), 5000)
        t.dev.write(2, frag, 5000)
def flip(t, fb, payload=b""):
    t.seq += 1; p0 = fb + 0x30
    hdr = struct.pack("<4I2H3I I3I", 3, len(payload), t.seq, 11, S, S, p0, p0 + S * H, p0 + 2 * S * H, 11, 0, 0, 0)
    session(t, hdr + payload, fb)
def rect7(t, a1, a2, w, rows, d1, d2):
    """session 7: rows of w bytes from d1 to a1, rows/2 rows from d2 to a2 (pitch S)"""
    data = d1 + d2
    t.dev.write(2, struct.pack("<4I4H2I", 7, len(data), a1, a2, S, S, w, rows, 0, 0), 5000)
    t.dev.write(2, data, 5000)
t = t6dev.T6()
t.set_mode(t.find_mode(W, H, 60)); t.cout(0x31, 0, 0); t.output(True); time.sleep(1.5)
flip(t, FB[0], plane444(BLUE, S, H)); time.sleep(0.3)
flip(t, FB[1], plane444(BLUE, S, H)); time.sleep(2)        # buffer 1 on screen
x0, y0, w, n = 600, 300, 320, 400                             # green rect into hidden buffer 0
P = FB[0] + 0x30
Yp, Up, Vp = P, P + S * H, P + 2 * S * H
gy, gu, gv = GREEN
at = lambda plane, y: plane + y * S + x0
rect7(t, at(Yp, y0), at(Up, y0), w, n, bytes([gy]) * (w * n), bytes([gu]) * (w * n // 2))
rect7(t, at(Vp, y0), at(Up, y0 + n // 2), w, n, bytes([gv]) * (w * n), bytes([gu]) * (w * n // 2))
time.sleep(2.5)
flip(t, FB[0]); time.sleep(1)
print("done", flush=True)
t.close()
