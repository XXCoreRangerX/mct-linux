"""The driver's partial update scheme, from userspace: diff against the copy of
the target buffer, session-7 band writes, then an empty flip. Max speed."""
import os
import sys, struct, time
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import t6dev
W, H = 1920, 1080
FB = (0x0, 0xFFFEC0, 0x1FFFD80)
FRAG = t6dev.FRAG
STEP = (FRAG - 32) // (3 * W)
def img(i, N=40):
    y0 = 40 + (i * (H - 400)) // (N - 1)
    yp = np.full((H, W), 41, np.uint8); yp[y0:y0 + 300, 800:1100] = 235
    uv = np.empty((H // 2, W // 2, 2), np.uint8); uv[...] = (240, 110)
    uv[y0 // 2:(y0 + 300) // 2, 400:550] = (128, 128)
    return np.concatenate([yp.reshape(-1), uv.reshape(-1)])
frames = [img(i) for i in range(40)] + [img(i) for i in range(38, 0, -1)]
hw = [None, None, None]
t = t6dev.T6()
t.set_mode(t.find_mode(1920, 1080, 60)); t.cout(0x31, 0, 0); t.output(True); time.sleep(1.5)
def bands(new, old):
    if old is None:
        return [(0, H // 2)]
    ny = new[:W * H].reshape(H // 2, 2 * W); oy = old[:W * H].reshape(H // 2, 2 * W)
    nu = new[W * H:].reshape(H // 2, W); ou = old[W * H:].reshape(H // 2, W)
    ch = np.nonzero((ny != oy).any(1) | (nu != ou).any(1))[0]
    out = []
    for p in ch:
        if out and p - out[-1][1] <= 8:
            out[-1][1] = p + 1
        else:
            out.append([p, p + 1])
    return out
def send_band(k, p0, p1, new):
    base = FB[k] + 0x30
    p = p0
    while p < p1:
        n = min(STEP, p1 - p)
        y = new[p * 2 * W:(p + n) * 2 * W].tobytes(); uv = new[W * H + p * W:W * H + (p + n) * W].tobytes()
        t.dev.write(2, struct.pack("<4I4H2I", 7, len(y) + len(uv), base + p * 2 * W, base + W * H + p * W, W, W, W, n * 2, 0, 0), 5000)
        t.dev.write(2, y + uv, 5000)
        p += n
    return 0
def flip(k):
    t.seq += 1
    b = FB[k]; p0 = b + 0x30
    t.dev.write(2, struct.pack("<8I", 0, 48, b, 48, 0, 0, 0, 0), 5000)
    t.dev.write(2, struct.pack("<4I2H3I I3I", 3, 0, t.seq, 6, W, W, p0, p0 + W * H, p0, 6, 0, 0, 0), 5000)
n, k, t0, rows = 0, 0, time.time(), 0
while time.time() - t0 < 8:
    new = frames[n % len(frames)]
    bl = bands(new, hw[k])
    for p0, p1 in bl:
        send_band(k, p0, p1, new); rows += 2 * (p1 - p0)
    flip(k)
    hw[k] = new.copy()
    k = (k + 1) % 3
    t.acks(1)
    n += 1
print(f"{n} frames in 8 s = {n / 8:.0f} fps, {rows / n:.0f} rows per frame", flush=True)
t.close()
