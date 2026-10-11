#!/usr/bin/env python3
"""Are the T6's frame-done events tied to the monitor's refresh? About 10 s,
nothing to watch, prints numbers. Run through tools/t6_run.sh.
"""
import os, sys, struct, time, math
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import t6dev

W, H = 1920, 1080
FB = (0x0, 0xFFFEC0, 0x1FFFD80)
HZ = 60
RGB32, NV12, YUV444 = 8, 6, 11


def picture(w, h):
    img = np.zeros((h, w, 3), np.uint8)
    img[...] = (70, 50, 40)                                   # B, G, R
    img[:h // 12] = (0, 0, 230)                               # red bar at the top
    img[-h // 12:] = (230, 60, 0)                             # blue bar at the bottom
    s = min(w, h) // 5
    img[h // 12 + 20:h // 12 + 20 + s, 20:20 + s] = (0, 200, 0)    # green square top left
    cx, top, bot = w // 2, h // 4, 3 * h // 4                 # arrow pointing up
    for y in range(top, top + s):
        half = (y - top) * s // (2 * s) + 1
        img[y, cx - half:cx + half] = 255
    img[top + s:bot, cx - s // 8:cx + s // 8] = 255
    return img


def pad(a, pitch):
    out = np.zeros((a.shape[0], pitch), np.uint8)
    out[:, :a.shape[1]] = a
    return out


def yuv(img):
    b, g, r = (img[..., i].astype(np.float32) for i in range(3))
    y = 0.299 * r + 0.587 * g + 0.114 * b
    u = 128 - 0.168736 * r - 0.331264 * g + 0.5 * b
    v = 128 + 0.5 * r - 0.418688 * g - 0.081312 * b
    return [np.clip(np.rint(c), 0, 255).astype(np.uint8) for c in (y, u, v)]


def encode(img, fmt):
    """-> payload, pitch, plane offsets relative to the image start"""
    h, w = img.shape[:2]
    if fmt == RGB32:
        out = np.zeros((h, w, 4), np.uint8); out[..., :3] = img
        return out.tobytes(), w * 4, (0, 0, 0)
    y, u, v = yuv(img)
    if fmt == NV12:
        uv = np.empty((h // 2, w // 2, 2), np.uint8)
        uv[..., 0] = u.reshape(h // 2, 2, w // 2, 2).mean((1, 3))
        uv[..., 1] = v.reshape(h // 2, 2, w // 2, 2).mean((1, 3))
        return y.tobytes() + uv.tobytes(), w, (0, w * h, 0)
    p = (w + 31) // 32 * 32
    return b"".join(pad(c, p).tobytes() for c in (y, u, v)), p, (0, p * h, 2 * p * h)


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


def events(t, timeout=1):
    got = []
    while True:
        try:
            ev = bytes(t.dev.read(t6dev.EP_ACK, 64, timeout))
            got.append((time.monotonic(), ev))
        except Exception:
            return got


def set_mode(t):
    t.set_mode(t.find_mode(W, H, HZ)); t.cout(0x31, 0, 0); t.output(True)
    time.sleep(1.5)


def part1(t):
    print("frame-done timing (nothing to watch)", flush=True)
    set_mode(t)
    img = picture(W, H)
    pay, pitch, offs = encode(img, NV12)
    events(t, 50)
    # back to back: is the done rate capped at the refresh rate?
    n, t0, done = 0, time.monotonic(), []
    while time.monotonic() - t0 < 4:
        frame(t, FB[n % 3], pay, pitch, NV12, offs)
        done += events(t, 1)
        n += 1
    done += events(t, 100)
    nd = sum(1 for _, e in done if e[0x13] & 4)
    print(f"  back to back: {n} frames sent, {nd} frame done in 4 s ({nd / 4:.1f}/s, refresh {HZ})")
    # spaced: phase of the done events against a {HZ} Hz grid
    lat, ph = [], []
    for i in range(60):
        frame(t, FB[i % 3], pay, pitch, NV12, offs)
        ts = time.monotonic()
        ev = [e for e in events(t, 40) if e[1][0x13] & 4]
        if ev:
            lat.append((ev[0][0] - ts) * 1000)
            ph.append((ev[0][0] * HZ) % 1.0)
        time.sleep(0.037)
    if ph:
        c = sum(math.cos(2 * math.pi * p) for p in ph) / len(ph)
        s = sum(math.sin(2 * math.pi * p) for p in ph) / len(ph)
        lat.sort()
        print(f"  spaced: {len(ph)} events, done after last write: min {lat[0]:.2f} / "
              f"median {lat[len(lat) // 2]:.2f} / max {lat[-1]:.2f} ms")
        print(f"  phase concentration R = {math.hypot(c, s):.2f} (near 1: locked to refresh, near 0: not)")
        print(f"  sample event: {done[0][1][:24].hex() if done else '-'}", flush=True)


t = t6dev.T6()
try:
    part1(t)
finally:
    t.close()
