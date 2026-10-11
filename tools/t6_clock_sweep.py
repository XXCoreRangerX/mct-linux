#!/usr/bin/env python3
"""Mode sweeps on the T6 with a Gigabyte MO34WQC2 (3440x1440). Each step
sets a mode and keeps sending a picture (dark blue, coloured columns from
red at the left to magenta at the right edge, a strip of 1 px red/blue
lines, a white block moving across, and N white squares at the top left
for step N) until you answer y or n on the terminal; a summary at the end.

  t6_clock_sweep.py [clock]   does the monitor show it? (NV12)
    1  3440x1440@55, monitor timing        293.2 MHz
    2  3440x1440@57, monitor timing        303.9 MHz
    3  3440x1440@60, reduced blanking      312.8 MHz
    4  2560x1440@80, reduced blanking      316.0 MHz
  t6_clock_sweep.py more
    1, 2  3440x1440@62 / @64, reduced blanking, NV12: picture?
    3-5   3440x1440@52 / 55 / 57 in YUV 4:4:4: clean? (no lines, no
          flicker, block moves fine)
  t6_clock_sweep.py color     only the three 4:4:4 steps

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


SETS = {
    "clock": [
        ("3440x1440@55, monitor timing", (3440, 1440, 55, 3600, 3488, 32, 1481, 1443, 10), 6),
        ("3440x1440@57, monitor timing", (3440, 1440, 57, 3600, 3488, 32, 1481, 1443, 10), 6),
        ("3440x1440@60, reduced blanking", rb(3440, 1440, 60), 6),
        ("2560x1440@80, reduced blanking", rb(2560, 1440, 80), 6),
    ],
    "more": [
        ("3440x1440@62, reduced blanking", rb(3440, 1440, 62), 6),
        ("3440x1440@64, reduced blanking", rb(3440, 1440, 64), 6),
        ("3440x1440@52, 4:4:4", (3440, 1440, 52, 3600, 3488, 32, 1481, 1443, 10), 11),
        ("3440x1440@55, 4:4:4", (3440, 1440, 55, 3600, 3488, 32, 1481, 1443, 10), 11),
        ("3440x1440@57, 4:4:4", (3440, 1440, 57, 3600, 3488, 32, 1481, 1443, 10), 11),
    ],
}
SETS["color"] = SETS["more"][2:]
SET = sys.argv[1] if len(sys.argv) > 1 else "clock"
QUESTION = {6: "picture?", 11: "clean? (no lines/flicker, block moves fine)"}


def planes(w, h, step, bx):
    """Y, U, V at full resolution"""
    y = np.full((h, w), 40, np.uint8)
    u = np.full((h, w), 170, np.uint8)
    v = np.full((h, w), 110, np.uint8)
    cols = ((81, 90, 240), (210, 16, 146), (145, 54, 34), (170, 166, 16), (106, 202, 222))
    for i, (yy, uu, vv) in enumerate(cols):
        x = (i * (w - 200) // 4) // 2 * 2
        y[h // 6:h // 2, x:x + 200] = yy
        u[h // 6:h // 2, x:x + 200] = uu
        v[h // 6:h // 2, x:x + 200] = vv
    y[:, ::200] = 235
    y[h // 2 + 40:h // 2 + 140:2] = 81                # 1 px red / blue lines
    u[h // 2 + 40:h // 2 + 140:2], v[h // 2 + 40:h // 2 + 140:2] = 90, 240
    y[h // 2 + 41:h // 2 + 140:2] = 41
    u[h // 2 + 41:h // 2 + 140:2], v[h // 2 + 41:h // 2 + 140:2] = 240, 110
    y[h * 2 // 3:h - 60, bx:bx + 300] = 235           # moving block
    u[h * 2 // 3:h - 60, bx:bx + 300] = 128
    v[h * 2 // 3:h - 60, bx:bx + 300] = 128
    for k in range(step):                             # step number as squares
        y[40:120, 60 + k * 120:140 + k * 120] = 235
        u[40:120, 60 + k * 120:140 + k * 120] = 128
        v[40:120, 60 + k * 120:140 + k * 120] = 128
    return y, u, v


def encode(w, h, fmt, step, bx):
    y, u, v = planes(w, h, step, bx)
    if fmt == 6:
        uv = np.empty((h // 2, w // 2, 2), np.uint8)
        uv[..., 0] = u[::2, ::2]
        uv[..., 1] = v[::2, ::2]
        return y.tobytes() + uv.tobytes(), w, (0, w * h, 0)
    p = (w + 31) // 32 * 32
    out = b""
    for c in (y, u, v):
        o = np.zeros((h, p), np.uint8); o[:, :w] = c
        out += o.tobytes()
    return out, p, (0, p * h, 2 * p * h)


def frame(t, fb, fmt, enc):
    payload, pitch, offs = enc
    t.seq += 1
    p0 = fb + 0x30
    hdr = struct.pack("<4I2H3I I3I", 3, len(payload), t.seq, fmt, pitch, pitch,
                      p0 + offs[0], p0 + offs[1], p0 + offs[2], fmt, 0, 0, 0)
    data = hdr + payload
    for off in range(0, len(data), t6dev.FRAG):
        frag = data[off:off + t6dev.FRAG]
        t.dev.write(2, struct.pack("<8I", 0, len(data), fb, len(frag), off,
                                   1 if off + t6dev.FRAG < len(data) else 0, 0, 0), 5000)
        t.dev.write(2, frag, 5000)


def run_until_answer(t, fmt, frames):
    """send the frames in a loop (the block moves) until a y/n line arrives"""
    import select
    k = 0
    while True:
        frame(t, FB[k % 3], fmt, frames[k % len(frames)])
        k += 1
        r, _, _ = select.select([sys.stdin], [], [], 0 if len(frames) > 1 else 1.0)
        if r:
            a = sys.stdin.readline().strip().lower()
            if a in ("y", "n"):
                return a


t = t6dev.T6()
results = []
try:
    for i, (name, args, fmt) in enumerate(SETS[SET], 1):
        khz, m = entry(*args)
        w, h = args[0], args[1]
        print(f"\nSTEP {i} ({i} square{'s' if i > 1 else ''}): {name}, {khz / 1000:.1f} MHz",
              flush=True)
        n = 12
        pos = [j * (w - 300) // (n - 1) // 2 * 2 for j in range(n)]
        frames = [encode(w, h, fmt, i, x) for x in pos + pos[-2:0:-1]]
        t.cout(0x12, t.out, 0, m); t.cout(0x31, 0, 0); t.output(True)
        time.sleep(1)
        print(f"  {QUESTION[fmt]} y/n + Enter: ", end="", flush=True)
        results.append((i, name, khz, run_until_answer(t, fmt, frames)))
    print("\nsummary:")
    for i, name, khz, a in results:
        print(f"  {i}  {name:34} {khz / 1000:6.1f} MHz  {'yes' if a == 'y' else 'no'}")
finally:
    t.close()
