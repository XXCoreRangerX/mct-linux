import sys, struct, time
sys.path.insert(0, "tools")
import numpy as np
src = open("tools/t6_yuy2_hunt.py").read().split("t = t6dev.T6()")[0]
sys.argv = ["x"]
exec(src)
W, H = 1024, 768
def block(i):
    img = np.zeros((H, W, 3), np.uint8); img[...] = (90, 40, 40)
    x0 = (i * (W - 200)) // 23
    img[100:660, x0:x0 + 200] = 255
    return img
frames = [packed(block(i), ["Y0", "U", "Y1", "V"]) for i in range(24)]
frames += frames[-2:0:-1]
t = t6dev.T6()
t.set_mode(t.find_mode(W, H, 60)); t.cout(0x31, 0, 0); t.output(True); time.sleep(2.5)
k, t0 = 0, time.time()
while time.time() - t0 < 8:
    send(t, FB[k % 3], frames[k % len(frames)], 2 * W, 0, 0)
    t.acks(1)
    k += 1
print(f"1024x768 YUYV: {k / 8:.0f} fps", flush=True)
t.close()
