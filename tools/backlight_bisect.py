#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the trimmed init (t2d/init.bin) from cold up to step N, pausing so the
backlight state can be observed. Usage: backlight_bisect.py N  (resumes from a state file)."""
import json, os, sys, time, t2dev

STATE = "/tmp/bl_state.json"
recs = t2dev.load_init()
upto = int(sys.argv[1])
done = json.load(open(STATE))["done"] if os.path.exists(STATE) and "--fresh" not in sys.argv else 0
t = t2dev.T2()
if done == 0:
    print("feb0=%02x (cold=00)" % t.reg(0xfeb0))
    time.sleep(6)
for i in range(done, upto):
    ep, fl, d, ln, rd = recs[i]
    t.run_init([recs[i]], smart=True)
json.dump({"done": upto}, open(STATE, "w"))
last = recs[upto - 1]
print(f"ran steps {done}..{upto - 1}; last: EP{last[0]} {(last[2] or b'').hex()[:40]}")
t.close()
