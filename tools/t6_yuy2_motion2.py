"""YUYV at 1080p: moving block with plane 1 offsets varied, to rule out a
second plane setting behind the row limit."""
import os
import sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "t6_yuy2_hunt.py")).read().split("t = t6dev.T6()")[0]
sys.argv = ["x"]
exec(src)
def block(i, n):
    img = np.zeros((H, W, 3), np.uint8); img[...] = (90, 40, 40)
    x0 = (i * (W - 300)) // 23
    img[200:880, x0:x0 + 300] = 255
    for k in range(n):
        img[20:80, 20 + k * 80:80 + k * 80] = 255
        img[20:80, 20 + k * 80 + 60:80 + k * 80] = 0
    return img
t = t6dev.T6()
t.set_mode(t.find_mode(W, H, 60)); t.cout(0x31, 0, 0); t.output(True); time.sleep(1.5)
for n, off in ((1, 0), (2, (H // 2) * 2 * W), (3, H * 2 * W)):
    frames = [packed(block(i, n), ["Y0", "U", "Y1", "V"]) for i in range(24)]
    frames += frames[-2:0:-1]
    if off >= H * 2 * W:
        frames = [f + f for f in frames]          # plane 1 after the image: a copy there
    print(n, "plane 1 at +", off, flush=True)
    k, t0 = 0, time.time()
    while time.time() - t0 < 6:
        send(t, FB[k % 3], frames[k % len(frames)], 2 * W, 2 * W, off)
        t.acks(1)
        k += 1
t.close()
