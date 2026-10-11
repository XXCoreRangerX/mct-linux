import os
import sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "t6_yuy2_hunt.py")).read().split("t = t6dev.T6()")[0]
sys.argv = ["x"]
exec(src)
img = np.zeros((H, W, 3), np.uint8)
cols = [(0, 0, 255), (0, 255, 0), (255, 0, 0), (255, 255, 255)]
for b in range(12):
    img[b * 90:(b + 1) * 90] = cols[b % 4]
    img[b * 90 + 30:b * 90 + 60, 40:40 + 60 * (b + 1):60] = 0     # b+1 black ticks per band
t = t6dev.T6()
t.set_mode(t.find_mode(W, H, 60)); t.cout(0x31, 0, 0); t.output(True); time.sleep(1.5)
for k in range(3):                 # same image into all three buffers, shown from the last
    send(t, FB[k], packed(img, ["Y0", "U", "Y1", "V"]), 2 * W, 0, 0)
print("bands shown", flush=True)
t.close()
