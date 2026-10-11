#!/usr/bin/env python3
"""Large modes on the T6, with the Gigabyte MO34WQC2's own timings:
  2560   2560x1440@60, 241.5 MHz (default)
  3440   3440x1440@60, 319.75 MHz: this one froze the monitor, don't
  3440-50  3440x1440@50, 266.58 MHz (the monitor's DP timing): width or clock?
Does the chip scan out rows this long, and in which formats?

Picture: dark blue, five coloured columns spread over the full width (red at
the left edge, then yellow, green, cyan, magenta at the right edge), thin
white vertical lines every 200 px, a grey ramp along the bottom. Each format
shows the picture for 5 s, then a white block moving across the full width
for 5 s. Run through tools/t6_run.sh.
"""
import os, sys, struct, time
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import t6dev

MODES = {  # clock kHz, htotal, hsync start, len, vtotal, vsync start, len
    "2560": (2560, 241500, 2720, 2608, 32, 1481, 1443, 5, 60),
    "3440": (3440, 319750, 3600, 3488, 32, 1481, 1443, 10, 60),
    "3440-50": (3440, 266580, 3600, 3488, 32, 1481, 1443, 5, 50),
}
W, CLK, HTOT, HS, HSL, VTOT, VS, VSL, HZ = MODES[sys.argv[1] if len(sys.argv) > 1 else "2560"]
H = 1440
FB = (0x0, 0xFFFEC0, 0x1FFFD80)
BASE = 40.0                                   # MHz, "Super Lite"


def pll(f_mhz):
    """integer math like the driver: f in kHz, base 40000 kHz"""
    f = round(f_mhz * 1000)
    for i, x in enumerate((4, 8, 16, 32)):
        if f * x >= 800000:
            vco = f * x
            n = vco // 40000
            return (vco - n * 40000) * 1000 // 40000, 1000, n, (3, 1, 2, 0)[i]
    raise ValueError


def entry(clock_khz, hz, htot, hs, hsl, vtot, vs, vsl, hpol, vpol):
    p, q, n, osel = pll(clock_khz / 1000)
    return struct.pack("<IBB8HHHBBBBBB", clock_khz, hz, 0, htot, W, hs, hsl, vtot, H, vs, vsl,
                       p, q, n, osel, hpol, vpol, 0, 0)


def picture():
    img = np.zeros((H, W, 3), np.uint8)
    img[...] = (90, 30, 20)                                      # B, G, R
    cols = ((0, 0, 230), (0, 220, 230), (0, 200, 0), (220, 220, 0), (220, 0, 220))
    for i, c in enumerate(cols):
        x = i * (W - 200) // 4
        img[200:1000, x:x + 200] = c
    img[:, ::200] = 255
    img[1150:1350] = np.linspace(0, 255, W, dtype=np.uint8)[None, :, None]
    return img


def block(i, n):
    img = np.zeros((H, W, 3), np.uint8); img[...] = (90, 30, 20)
    x0 = (i * (W - 300)) // (n - 1)
    img[300:1100, x0:x0 + 300] = 255
    return img


def yuv(img):
    b, g, r = (img[..., i].astype(np.float32) for i in range(3))
    y = 0.299 * r + 0.587 * g + 0.114 * b
    u = 128 - 0.168736 * r - 0.331264 * g + 0.5 * b
    v = 128 + 0.5 * r - 0.418688 * g - 0.081312 * b
    return [np.clip(np.rint(c), 0, 255).astype(np.uint8) for c in (y, u, v)]


def encode(img, fmt):
    """-> payload, pitch, plane offsets"""
    if fmt == 9:                                                 # BGR24, rows of 32 px
        p = (W + 31) // 32 * 96
        out = np.zeros((H, p), np.uint8); out[:, :W * 3] = img.reshape(H, W * 3)
        return out.tobytes(), p, (0, 0, 0)
    y, u, v = yuv(img)
    if fmt == 6:                                                 # NV12
        uv = np.empty((H // 2, W // 2, 2), np.uint8)
        uv[..., 0] = u.reshape(H // 2, 2, W // 2, 2).mean((1, 3))
        uv[..., 1] = v.reshape(H // 2, 2, W // 2, 2).mean((1, 3))
        return y.tobytes() + uv.tobytes(), W, (0, W * H, 0)
    p = (W + 31) // 32 * 32                                      # YUV 4:4:4, 3 planes
    planes = []
    for c in (y, u, v):
        o = np.zeros((H, p), np.uint8); o[:, :W] = c
        planes.append(o.tobytes())
    return b"".join(planes), p, (0, p * H, 2 * p * H)


def frame(t, fb, payload, pitch, fmt, offs):
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


t = t6dev.T6()
try:
    m = entry(CLK, HZ, HTOT, HS, HSL, VTOT, VS, VSL, 1, 0)
    t.cout(0x12, t.out, 0, m); t.cout(0x31, 0, 0); t.output(True)
    print(f"mode set: {W}x{H}@{HZ}, {CLK / 1000} MHz", flush=True)
    time.sleep(3)
    pic = picture()
    n = 16
    moving = [block(i, n) for i in range(n)] + [block(i, n) for i in range(n - 2, 0, -1)]
    fmts = (("NV12", 6), ("YUV 4:4:4", 11), ("RGB24", 9))
    if W > 2560:
        fmts = fmts[:2]          # RGB24 needs ~1 GB/s of scanout here
    for step, (name, fmt) in enumerate(fmts, 1):
        print(f"\nSTEP {step}a: {name}, picture", flush=True)
        frame(t, FB[0], *encode(pic, fmt)[:2], fmt, encode(pic, fmt)[2])
        time.sleep(5)
        print(f"STEP {step}b: {name}, moving block", flush=True)
        frames = [encode(f, fmt) for f in moving]
        k, t0 = 0, time.time()
        while time.time() - t0 < 5:
            pay, pitch, offs = frames[k % len(frames)]
            frame(t, FB[k % 3], pay, pitch, fmt, offs)
            k += 1
        print(f"         {k / 5:.0f} fps", flush=True)
    print("\ndone", flush=True)
finally:
    t.close()
