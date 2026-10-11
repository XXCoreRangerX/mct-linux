#!/usr/bin/env python3
"""Pixel clock sweep on the T6: which of these modes does the monitor show?
Each step sets the mode and keeps sending an NV12 picture (dark blue,
coloured columns from red at the left to magenta at the right edge, and N
white squares at the top left for step N) until you answer y (picture) or
n (black) on the terminal. A summary comes at the end.

  1  3440x1440@55, monitor timing        293.2 MHz
  2  3440x1440@57, monitor timing        303.9 MHz
  3  3440x1440@60, reduced blanking      312.8 MHz
  4  2560x1440@80, reduced blanking      ~316 MHz (clock as high, rows shorter)

Ctrl-C ends it (t6_run.sh rebinds the driver). Run through tools/t6_run.sh.
"""
import os, sys, struct, time
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import t6dev

FB = (0x0, 0xFFFEC0, 0x1FFFD80)


def pll(khz):
    """integer math like the driver, 40 MHz base ("Super Lite")"""
    for i, x in enumerate((4, 8, 16, 32)):
        if khz * x >= 800000:
            vco = khz * x
            n = vco // 40000
            return (vco - n * 40000) * 1000 // 40000, 1000, n, (3, 1, 2, 0)[i]
    raise ValueError


def entry(w, h, hz, htot, hs, hsl, vtot, vs, vsl, hpol=1, vpol=0):
    khz = round(htot * vtot * hz / 1000)
    p, q, n, osel = pll(khz)
    return khz, struct.pack("<IBB8HHHBBBBBB", khz, hz, 0, htot, w, hs, hsl, vtot, h, vs, vsl,
                            p, q, n, osel, hpol, vpol, 0, 0)


def rb(w, h, hz):
    """the driver's reduced blanking (t6_rb_mode)"""
    vt = h + 20
    while (vt - h) * 1000000 < 460 * hz * vt:
        vt += 1
    return (w, h, hz, w + 80, w + 8, 32, vt, vt - 14, 8)


STEPS = [
    ("3440x1440@55, monitor timing", (3440, 1440, 55, 3600, 3488, 32, 1481, 1443, 10)),
    ("3440x1440@57, monitor timing", (3440, 1440, 57, 3600, 3488, 32, 1481, 1443, 10)),
    ("3440x1440@60, reduced blanking", (3440, 1440, 60, 3520, 3448, 32, 1481, 1443, 10)),
    ("2560x1440@80, reduced blanking", rb(2560, 1440, 80)),
]


def nv12(w, h, step):
    y = np.full((h, w), 40, np.uint8)
    uv = np.empty((h // 2, w // 2, 2), np.uint8); uv[...] = (170, 110)
    cols = ((81, 90, 240), (210, 16, 146), (145, 54, 34), (170, 166, 16), (106, 202, 222))
    for i, (yy, u, v) in enumerate(cols):
        x = (i * (w - 200) // 4) // 2 * 2
        y[h // 6:h * 2 // 3, x:x + 200] = yy
        uv[h // 12:h // 3, x // 2:x // 2 + 100] = (u, v)
    y[:, ::200] = 235
    for k in range(step):                     # step number as white squares
        y[40:120, 60 + k * 120:140 + k * 120] = 235
        uv[20:60, 30 + k * 60:70 + k * 60] = 128
    return y.tobytes() + uv.tobytes()


def frame(t, fb, w, h, payload):
    t.seq += 1
    p0 = fb + 0x30
    hdr = struct.pack("<4I2H3I I3I", 3, len(payload), t.seq, 6, w, w, p0, p0 + w * h, p0, 6, 0, 0, 0)
    data = hdr + payload
    for off in range(0, len(data), t6dev.FRAG):
        frag = data[off:off + t6dev.FRAG]
        t.dev.write(2, struct.pack("<8I", 0, len(data), fb, len(frag), off,
                                   1 if off + t6dev.FRAG < len(data) else 0, 0, 0), 5000)
        t.dev.write(2, frag, 5000)


def wait_answer(t, w, h, pic):
    """resend the picture every second until a y/n line arrives"""
    import select
    k = 0
    while True:
        frame(t, FB[k % 3], w, h, pic)
        k += 1
        r, _, _ = select.select([sys.stdin], [], [], 1.0)
        if r:
            a = sys.stdin.readline().strip().lower()
            if a in ("y", "n"):
                return a


t = t6dev.T6()
results = []
try:
    for i, (name, args) in enumerate(STEPS, 1):
        khz, m = entry(*args)
        w, h = args[0], args[1]
        print(f"\nSTEP {i} ({i} square{'s' if i > 1 else ''}): {name}, {khz / 1000:.1f} MHz", flush=True)
        t.cout(0x12, t.out, 0, m); t.cout(0x31, 0, 0); t.output(True)
        time.sleep(1)
        print("  picture? y/n + Enter: ", end="", flush=True)
        results.append((i, name, khz, wait_answer(t, w, h, nv12(w, h, i))))
    print("\nsummary:")
    for i, name, khz, a in results:
        print(f"  {i}  {name:34} {khz / 1000:6.1f} MHz  {'picture' if a == 'y' else 'black'}")
finally:
    t.close()
