import os
import sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "t6_yuy2_hunt.py")).read().split("t = t6dev.T6()")[0]
sys.argv = ["x"]
exec(src)
def block(i):
    img = np.zeros((H, W, 3), np.uint8); img[...] = (90, 40, 40)
    x0 = (i * (W - 300)) // 23
    img[200:880, x0:x0 + 300] = 255
    return img
frames = [packed(block(i), ["Y0", "U", "Y1", "V"]) for i in range(24)]
frames += frames[-2:0:-1]
t = t6dev.T6()
t.set_mode(t.find_mode(W, H, 60)); t.cout(0x31, 0, 0); t.output(True); time.sleep(1.5)
n, t0 = 0, time.time()
while time.time() - t0 < 6:
    send(t, FB[n % 3], frames[n % len(frames)], 2 * W, 0, 0)
    t.acks(1)
    n += 1
print(f"YUYV motion: {n / 6:.0f} fps", flush=True)
t.close()
