"""YUYV frames sent as session 7 rectangle writes instead of the flip's
payload, to see if the row limit depends on the write path."""
import os
import sys, struct, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "t6_yuy2_hunt.py")).read().split("t = t6dev.T6()")[0]
sys.argv = ["x"]
exec(src)
P = 2 * W                                     # bytes per row
STEP = t6dev.FRAG // (3 * P)                  # n: each write = 3n rows
def block(i):
    img = np.zeros((H, W, 3), np.uint8); img[...] = (90, 40, 40)
    x0 = (i * (W - 300)) // 23
    img[200:880, x0:x0 + 300] = 255
    return img
def write_frame(t, fb, data):
    base = fb + 0x30
    y = 0
    while y < H:
        n = min(STEP, -(-(H - y) // 3))
        ys = min(y, H - 3 * n)
        chunk = data[ys * P:(ys + 3 * n) * P]
        t.dev.write(2, struct.pack("<4I4H2I", 7, len(chunk), base + ys * P, base + (ys + 2 * n) * P,
                                   P, P, P, 2 * n, 0, 0), 5000)
        t.dev.write(2, chunk, 5000)
        y += 3 * n
def flip(t, fb):
    t.seq += 1; p0 = fb + 0x30
    t.dev.write(2, struct.pack("<8I", 0, 48, fb, 48, 0, 0, 0, 0), 5000)
    t.dev.write(2, struct.pack("<4I2H3I I3I", 3, 0, t.seq, 0, P, 0, p0, p0, 0, 0, 0, 0, 0), 5000)
frames = [packed(block(i), ["Y0", "U", "Y1", "V"]) for i in range(24)]
frames += frames[-2:0:-1]
t = t6dev.T6()
t.set_mode(t.find_mode(W, H, 60)); t.cout(0x31, 0, 0); t.output(True); time.sleep(1.5)
k, t0 = 0, time.time()
while time.time() - t0 < 8:
    write_frame(t, FB[k % 3], frames[k % len(frames)])
    flip(t, FB[k % 3])
    t.acks(1)
    k += 1
print(f"YUYV via rectangle writes: {k / 8:.0f} fps", flush=True)
t.close()
