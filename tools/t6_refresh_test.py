"""Custom refresh test: build a mode entry with the PLL fields MCT's formula gives."""
import sys, struct, time, math
import numpy as np
sys.path.insert(0, "tools")
import t6dev
W, H = 1920, 1080
BASE = 40.0          # MHz; b0 index 0 = 1 -> "Super Lite" platform

def pll(f_mhz):
    for i, x in enumerate((4, 8, 16, 32)):
        if f_mhz * x >= 800:
            v = f_mhz * x / BASE
            n = int(v)
            p = int((v - n) * 1000)
            return p, 1000, n, (3, 1, 2, 0)[i]
    raise ValueError

def entry(clock_khz, hz, htot, hs, hsl, vtot, vs, vsl, hpol=1, vpol=1):
    p, q, n, osel = pll(clock_khz / 1000)
    return struct.pack("<IBB8HHHBBBBBB", clock_khz, hz, 0, htot, W, hs, hsl, vtot, H, vs, vsl,
                       p, q, n, osel, hpol, vpol, 0, 0)

t = t6dev.T6()
ref = t.find_mode(1920, 1080, 60)["raw"]
mine60 = entry(148500, 60, 2200, 2008, 44, 1125, 1084, 5)
print("table 1080p60 :", ref.hex())
print("computed      :", mine60.hex(), "MATCH" if mine60[:28] == ref[:28] else "DIFF")
hz = int(sys.argv[1]) if len(sys.argv) > 1 else 120
def rb2(hz):
    """CVT reduced blanking v2 style: 80 px horizontal blanking, >= 460 us vertical."""
    htot = W + 80
    vtot = H + 20
    while (vtot - H) * (1e6 / hz / vtot) < 460:
        vtot += 1
    clk = round(htot * vtot * hz / 1000)
    return entry(clk, hz, htot, W + 8, 32, vtot, vtot - 6 - 8, 8, 1, 0), clk, vtot
if hz == 120:
    m = entry(297000, 120, 2200, 2008, 44, 1125, 1084, 5)        # CTA VIC 63
elif hz == 60:
    m = ref
else:
    m, clk, vtot = rb2(hz)
    print(f"{hz} Hz reduced blanking: 2000 x {vtot}, {clk} kHz", flush=True)
t.cout(0x12, t.out, 0, m); t.cout(0x31, 0, 0); t.output(True)
print(f"mode set: {hz} Hz", flush=True)
time.sleep(2)
BUFS = (0x0, 0xFFFEC0, 0x1FFFD80)
N = 24
def nv12(i):
    x0 = (i * (W - 300)) // (N - 1)
    y = np.full((H, W), 41, np.uint8); y[200:880, x0:x0 + 300] = 235
    uv = np.empty((H // 2, W // 2, 2), np.uint8); uv[...] = (240, 110)
    uv[100:440, x0 // 2:(x0 + 300) // 2] = (128, 128)
    return y.tobytes() + uv.tobytes()
frames = [nv12(i) for i in range(N)] + [nv12(i) for i in range(N - 2, 0, -1)]
n, t0 = 0, time.time()
while time.time() - t0 < 8:
    fb = BUFS[n % 3]; p0 = fb + 0x30; pay = frames[n % len(frames)]
    t.seq += 1
    data = struct.pack("<4I2H3I I3I", 3, len(pay), t.seq, 6, W, W, p0, p0 + W * H, p0, 6, 0, 0, 0) + pay
    for off in range(0, len(data), t6dev.FRAG):
        frag = data[off:off + t6dev.FRAG]
        t.dev.write(2, struct.pack("<8I", 0, len(data), fb, len(frag), off,
                                   1 if off + t6dev.FRAG < len(data) else 0, 0, 0), 5000)
        t.dev.write(2, frag, 5000)
    t.acks(1)
    n += 1
print(f"{n} frames in 8 s = {n / 8:.1f} fps", flush=True)
t.close()
