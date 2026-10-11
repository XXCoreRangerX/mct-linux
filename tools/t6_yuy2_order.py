"""YUYV byte order: Y0 U Y1 V against Y0 V Y1 U on the same picture."""
import os
import sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.argv = ["x"]
src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "t6_yuy2_hunt.py")).read().split("t = t6dev.T6()")[0]
exec(src)
t = t6dev.T6()
t.set_mode(t.find_mode(W, H, 60)); t.cout(0x31, 0, 0); t.output(True); time.sleep(1.5)
for n, order in ((1, ["Y0", "U", "Y1", "V"]), (3, ["Y0", "V", "Y1", "U"])):
    print(n, order, flush=True)
    send(t, FB[n % 3], packed(image(n), order), 2 * W, 0, 0)
    time.sleep(8)
t.close()
