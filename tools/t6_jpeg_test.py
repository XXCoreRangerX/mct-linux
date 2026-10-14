#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""The T6 JPEG path from userspace (docs/research/t6-jpeg.md, lab
captures/t6-jpeg/ and captures/t6-research/jpeg-path.md). Run through
tools/t6_run.sh (driver unbound, bound again at the end). LG at
1920x1080@60 only; no other timing is touched.

  J1  three test cards as full-frame JPEGs (4:2:0, Q95, command 3) into the
      three frame buffers: right picture? frame-done events?
  J2  decode time at 1080p: flat, test card and noise at Q75 and Q95
  J3  large JPEGs decoded into a hidden buffer with command 7 (no flip, no
      mode change): 1920x1080 up to 3840x2160, decode time each
  P   a partial update: a JPEG rectangle decoded into the hidden buffer
      (command 7, position in the plane addresses), then an empty flip
  K   clean check: a test card drawn the way mct_t6 draws (session 7
      rectangle writes, then an empty flip), RGB24 and NV12: after JPEG
      frames the driver's picture came out tinted with a garbage band
  R   only if K isn't clean: recovery attempts, each followed by K again:
      request 0x86 (reset JPEG engine, MCT's t6.h), the mode set again,
      a full raw frame the vendor way
  X   request 0x86 on its own, then a JPEG frame: does the decoder still work?

  tools/t6_run.sh tools/t6_jpeg_test.py [--steps J1,P,J3,K,R]   (default)

Afterwards replug the T6 if K stayed unclean: the decoder's state outlives
the driver rebind. Sends only set mode 0x12, software ready 0x31, output
0x03, bulk sessions 0 and 7, and in R the request 0x86. Log:
/tmp/t6_jpeg-<time>.log.

Found 2026-10-13: JPEG fragments must not have the "more" flag set, and a
bitstream ring that wraps back with FLAG_RESET_JPEG breaks the decoder and
the event endpoint, so three fixed slots are used in turn.
"""
import argparse, io, os, queue, statistics, struct, sys, threading, time
import numpy as np
import usb.core
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import t6dev

W, H = 1920, 1080
FB = (0x0, 0xFFFEC0, 0x1FFFD80)          # mct_t6's three frame buffers
SLOTS = (0x3000000, 0x3300000, 0x3600000)  # JPEG bitstream slots, used in turn
SLOT_SIZE = 0x300000
FMT_NV12, FMT_BGR24, FMT_JPEG = 6, 9, 0xD
FLAG_RESET_JPEG = 0x80
EV_DONE, EV_JPEG_ERR = 0x04, 0x08
REQ_RESET_JPEG = 0x86

log_path = time.strftime("/tmp/t6_jpeg-%Y%m%d-%H%M%S.log")
logf = open(log_path, "w")


def log(msg, console=True):
    line = f"{time.strftime('%H:%M:%S')} {msg}"
    logf.write(line + "\n")
    logf.flush()
    if console:
        print(msg, flush=True)


def ask(q):
    while True:
        a = input(f"  {q} [y/n] ").strip().lower()
        if a in ("y", "n"):
            log(f"ANSWER {q} -> {a}", console=False)
            return a == "y"


def a16(v):
    return (v + 15) & ~15


# ------------------------------------------------------------------ images

def card(w, h, n, bg=(30, 40, 110)):
    """Test card (RGB): background, white border, colour bars, grey ramp,
    n + 1 white blocks at the left, fine red lines at the bottom."""
    img = np.zeros((h, w, 3), np.uint8)
    img[...] = bg
    img[:6], img[-6:], img[:, :6], img[:, -6:] = 255, 255, 255, 255
    bars = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0), (0, 255, 255), (255, 0, 255)]
    bw = (w - 400) // len(bars)
    for i, c in enumerate(bars):
        img[h // 6:h // 2, 300 + i * bw:300 + (i + 1) * bw] = c
    img[h // 2 + 40:h // 2 + 160, 300:w - 100] = np.linspace(0, 255, w - 400, dtype=np.uint8)[None, :, None]
    for i in range(n + 1):
        img[h // 6 + i * 90:h // 6 + i * 90 + 60, 60:200] = 255
    for y in range(h * 3 // 4, h - 40, 8):
        img[y:y + 2, 300:w - 100] = (255, 0, 0)
    return img


def flat(w, h):
    img = np.zeros((h, w, 3), np.uint8)
    img[...] = (60, 60, 60)
    return img


def noise(w, h, seed=1):
    return np.random.default_rng(seed).integers(0, 256, (h, w, 3), np.uint8)


def jpeg(img, q):
    buf = io.BytesIO()
    Image.fromarray(img, "RGB").save(buf, "JPEG", quality=q, subsampling=2)   # 4:2:0
    return buf.getvalue()


def nv12(img):
    """RGB -> NV12 (BT.601, limited range): Y plane, then interleaved U, V."""
    r, g, b = (img[..., i].astype(np.int32) for i in range(3))
    y = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16
    u = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128
    v = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128
    u = u.reshape(img.shape[0] // 2, 2, img.shape[1] // 2, 2).mean(axis=(1, 3))
    v = v.reshape(img.shape[0] // 2, 2, img.shape[1] // 2, 2).mean(axis=(1, 3))
    uv = np.empty((img.shape[0] // 2, img.shape[1]), np.uint8)
    uv[:, 0::2] = u.round().clip(0, 255)
    uv[:, 1::2] = v.round().clip(0, 255)
    return y.clip(0, 255).astype(np.uint8), uv


# ------------------------------------------------------------------ events

class Events(threading.Thread):
    """Reads EP 0x83 continuously with long timeouts (short timed-out reads
    led to I/O errors in earlier runs); frame-done events into a queue."""

    def __init__(self, dev):
        super().__init__(daemon=True)
        self.dev, self.q, self.stop = dev, queue.Queue(), False
        self.count = self.errors = 0

    def run(self):
        while not self.stop:
            try:
                ev = bytes(self.dev.read(t6dev.EP_ACK, 64, 2000))
            except usb.core.USBTimeoutError:
                continue
            except (usb.core.USBError, ValueError) as e:
                if self.stop:
                    return
                self.errors += 1
                log(f"  event read error: {e}", console=False)
                try:
                    self.dev.clear_halt(t6dev.EP_ACK)
                except usb.core.USBError:
                    pass
                time.sleep(0.05)
                continue
            self.count += 1
            self.q.put((time.perf_counter(), ev))


# ------------------------------------------------------------------ device

class Jpeg:
    def __init__(self, t):
        self.t = t
        self.shown = None           # frame buffer on screen (last flip)
        self.slot = 0
        self.fence = 0
        self.first = True
        self.ev = Events(t.dev)
        self.ev.start()

    def _send(self, hdr, data, flags):
        payload = bytearray(hdr + data + bytes(1024))   # JPEG + 1024 zero bytes
        assert len(payload) <= SLOT_SIZE, "JPEG too large for a slot"
        payload[0x2F] = flags
        dest = SLOTS[self.slot]
        self.slot = (self.slot + 1) % len(SLOTS)
        self.t.send(bytes(payload), dest=dest)          # fragments without "more"
        return time.perf_counter()

    def flip(self, data, fb, w=W, h=H):
        """Command 3: decode into frame buffer fb and show it."""
        self.fence += 1
        p0 = fb + 0x30
        hdr = struct.pack("<4I2H3II8x3xB", 3, len(data) + 1024, self.fence, FMT_NV12,
                          a16(w), a16(w), p0, p0 + a16(w) * a16(h), 0, FMT_JPEG, 0)
        flags = FLAG_RESET_JPEG if self.first else 0
        self.first = False
        self.shown = fb
        return self.fence, self._send(hdr, data, flags)

    def decode(self, data, fb, w, h, x=0, y=0, pitch=None, buf_h=None):
        """Command 7: decode a w x h JPEG into frame buffer fb at (x, y) of a
        surface with the given pitch and height, no flip."""
        self.fence += 1
        pitch = pitch or a16(w)
        buf_h = buf_h or a16(h)
        base = fb + 0x30
        p0 = base + y * pitch + x
        p1 = base + pitch * buf_h + (y // 2) * pitch + x
        hdr = struct.pack("<4I2H2H3II7xB", 7, len(data) + 1024, self.fence, FMT_NV12,
                          w, h, pitch, pitch, p0, p1, 0, FMT_JPEG, 0)
        return self.fence, self._send(hdr, data, 0)

    def wait(self, sent, timeout=1.0):
        """(fence, send time) -> (seconds until an event reports it, error), or None."""
        fence, t_sent = sent
        end = time.perf_counter() + timeout
        while True:
            left = end - time.perf_counter()
            if left <= 0:
                return None
            try:
                t_ev, ev = self.ev.q.get(timeout=left)
            except queue.Empty:
                return None
            if len(ev) < 0x14:
                continue
            fid = struct.unpack_from("<I", ev, 0x0C)[0]
            log(f"  event {ev[:0x14].hex()} fence {fid}", console=False)
            if ev[0x13] & EV_JPEG_ERR:
                return t_ev - t_sent, True
            if ev[0x13] & EV_DONE and fid >= fence:      # events repeat the last done fence
                return max(t_ev - t_sent, 0), False

    def close(self):
        self.ev.stop = True


def empty_flip(t, fb, fmt, pitch, p1_off, planes):
    """mct_t6's t6_flip: show frame buffer fb, no image data."""
    t.seq += 1
    base = fb + 0x30
    v = struct.pack("<4I2H3II12x", 3, 0, t.seq, fmt, pitch, pitch if planes > 1 else 0,
                    base, base + p1_off, base if planes > 1 else 0, fmt)
    t.dev.write(2, struct.pack("<8I", 0, len(v), fb, len(v), 0, 0, 0, 0), 5000)
    t.dev.write(2, v, 5000)


def rect_write(t, y_addr, uv_addr, pitch, bw, rows2, data):
    """mct_t6's t6_write_rect: session 7, 2n rows at y_addr + n at uv_addr."""
    hdr = struct.pack("<4I4H8x", 7, len(data), y_addr, uv_addr, pitch, pitch, bw, rows2)
    t.dev.write(2, hdr, 5000)
    t.dev.write(2, data, 5000)


def draw_rgb24(t, fb, img):
    """A full picture into frame buffer fb as mct_t6 does with RGB24."""
    pitch = ((W + 31) // 32 * 32) * 3
    bgr = np.ascontiguousarray(img[..., ::-1]).reshape(H, W * 3)
    rows = np.zeros((H, pitch), np.uint8)
    rows[:, :W * 3] = bgr
    n = max(t6dev.FRAG // (3 * pitch), 1)
    base = fb + 0x30
    y = 0
    while y < H:
        n3 = min(3 * n, H - y)
        k = n3 // 3 or 1
        ys = min(y, H - 3 * k)
        rect_write(t, base + ys * pitch, base + (ys + 2 * k) * pitch, pitch, pitch, 2 * k,
                   rows[ys:ys + 3 * k].tobytes())
        y = ys + 3 * k
    empty_flip(t, fb, FMT_BGR24, pitch, 0, 1)


def draw_nv12(t, fb, img):
    """A full picture into frame buffer fb as mct_t6 does with NV12 (UV at w x h)."""
    y, uv = nv12(img)
    n = max(t6dev.FRAG // (3 * W), 1)
    base = fb + 0x30
    for p in range(0, H // 2, n):
        k = min(n, H // 2 - p)
        data = y[2 * p:2 * p + 2 * k].tobytes() + uv[p:p + k].tobytes()
        rect_write(t, base + 2 * p * W, base + W * H + p * W, W, W, 2 * k, data)
    empty_flip(t, fb, FMT_NV12, W, W * H, 2)


# ------------------------------------------------------------------ steps

def j1(t, j):
    log("\n=== J1: three test cards as JPEG (4:2:0, Q95) into the three buffers")
    got = []
    for n, fb in enumerate(FB):
        data = jpeg(card(W, H, n), 95)
        r = j.wait(j.flip(data, fb))
        got.append(r)
        log(f"  buffer {n}: {len(data) // 1024} KiB, frame-done "
            + ("none" if r is None else f"after {r[0] * 1000:.1f} ms" + (" (DECODER ERROR)" if r[1] else "")))
    ok = ask("The LG shows a test card with 3 white blocks at the left, colour bars, a grey "
             "ramp and fine red lines at the bottom, colours right and no stripes?")
    log(f"RESULT J1: {'PASS' if ok else 'FAIL'}; frame-done events: "
        f"{sum(1 for r in got if r)} of 3")
    return ok


def j2(t, j):
    log("\n=== J2: decode time at 1080p, one frame at a time, then back to back")
    contents = (("flat grey", flat(W, H)),
                ("test card on GREEN with 5 white blocks", card(W, H, 4, bg=(20, 110, 40))),
                ("noise (coloured static)", noise(W, H)))
    for name, img in contents:
        for q in (75, 95):
            data = jpeg(img, q)
            lat, misses = [], 0
            for k in range(30):
                r = j.wait(j.flip(data, FB[k % 3]))
                if r and not r[1]:
                    lat.append(r[0])
                    misses = 0
                else:
                    misses += 1
                    if misses == 3:
                        break
            if misses >= 3:
                log(f"  {name} Q{q} ({len(data) // 1024} KiB): 3 frames in a row without "
                    "frame-done, stopping here so the decoder isn't flooded")
                ask(f"Does the LG show {name}?")
                return
            med = statistics.median(lat)
            log(f"  {name} Q{q}: {len(data) // 1024} KiB, decode {med * 1000:.2f} ms median "
                f"({min(lat) * 1000:.2f}-{max(lat) * 1000:.2f}), {W * H / med / 1e6:.1f} Mpixel/s")
            t0, n, pending = time.perf_counter(), 60, []
            for k in range(n):
                pending.append(j.flip(data, FB[k % 3]))
                if len(pending) > 2 and j.wait(pending.pop(0)) is None:
                    log("  back to back: a frame went unacknowledged, stopping")
                    break
            for p in pending:
                j.wait(p)
            dt = time.perf_counter() - t0
            log(f"  {'':9s} back to back: {n / dt:.1f} fps ({n * len(data) / dt / 1e6:.1f} MB/s)")
        ok = ask(f"Does the LG show {name} now, without garbage?")
        log(f"RESULT J2 {name}: {'shown' if ok else 'NOT shown'}")


def j3(t, j):
    log("\n=== J3: large JPEGs decoded into a hidden buffer (command 7, no flip)")
    hidden = next(fb for fb in (FB[2], FB[1], FB[0]) if fb != j.shown)   # not on screen
    log(f"  decoding into buffer at {hidden:#x} (on screen: {j.shown:#x})", console=False)
    for w, h in ((1920, 1080), (2560, 1440), (3440, 1440), (3840, 2160)):
        if a16(w) * a16(h) * 3 // 2 + 0x30 > FB[1] - FB[0]:
            log(f"  {w}x{h}: does not fit a frame buffer, skipped")
            continue
        data = jpeg(card(w, h, 1), 95)
        lat = []
        for _ in range(10):
            r = j.wait(j.decode(data, hidden, w, h), timeout=2.0)
            if r is None or r[1]:
                log(f"  {w}x{h}: " + ("decoder error" if r else "no frame-done within 2 s"))
                break
            lat.append(r[0])
        if lat:
            med = statistics.median(lat)
            log(f"  {w}x{h}: {len(data) // 1024} KiB, decode {med * 1000:.2f} ms median "
                f"({min(lat) * 1000:.2f}-{max(lat) * 1000:.2f}), {w * h / med / 1e6:.1f} Mpixel/s")
    ok = ask("The picture on the LG stayed the same during J3 (it decodes into a hidden "
             "buffer)?")
    log(f"RESULT J3: {'hidden as expected' if ok else 'the picture changed'}; "
        f"{j.ev.errors} event read errors so far")


def p(t, j):
    log("\n=== P: partial update, a JPEG rectangle into the hidden buffer, then a flip")
    base_img = card(W, H, 1)                        # 2 blocks: the picture before
    r = j.wait(j.flip(jpeg(base_img, 95), FB[0]))
    log(f"  full picture into buffer 0 (shown): {'ok' if r else 'no event'}")
    r = j.wait(j.decode(jpeg(base_img, 95), FB[1], W, H, pitch=W, buf_h=a16(H)))
    log(f"  same picture into buffer 1 (hidden, command 7): {'ok' if r else 'no event'}")
    ok0 = ask("The LG shows the test card with 2 white blocks?")
    box = np.zeros((128, 256, 3), np.uint8)
    box[...] = (230, 30, 30)
    box[16:-16, 16:-16] = (255, 255, 255)
    box[48:80, 96:160] = (30, 30, 230)
    x, y = 832, 448                                  # multiples of 32
    r = j.wait(j.decode(jpeg(box, 95), FB[1], 256, 128, x=x, y=y, pitch=W, buf_h=a16(H)))
    log(f"  256x128 box at ({x},{y}) into buffer 1: {'ok' if r else 'no event'}")
    empty_flip(t, FB[1], FMT_NV12, W, W * a16(H), 2)
    j.shown = FB[1]
    ok = ask("After the flip: the same test card, plus a red-bordered white box with a blue "
             "bar in the middle of the screen, everything else unchanged and no garbage?")
    log(f"RESULT P: {'PASS' if ok and ok0 else 'FAIL'} (picture before: {'y' if ok0 else 'n'})")


def k(t, j, label="K"):
    log(f"\n=== {label}: clean check, drawn the way mct_t6 draws")
    draw_rgb24(t, FB[0], card(W, H, 5, bg=(110, 40, 30)))
    ok1 = ask("RGB24: a test card on DARK RED with 6 white blocks, clean colours, no tint, "
              "no garbage band at the bottom?")
    draw_nv12(t, FB[1], card(W, H, 6, bg=(40, 40, 40)))
    ok2 = ask("NV12: a test card on DARK GREY with 7 white blocks, clean colours, no tint, "
              "no garbage band at the bottom?")
    log(f"RESULT {label}: RGB24 {'clean' if ok1 else 'NOT clean'}, NV12 {'clean' if ok2 else 'NOT clean'}")
    return ok1 and ok2


def r(t, j):
    log("\n=== R: recovery attempts")
    try:
        t.cout(REQ_RESET_JPEG, 0, 0)
        log("  request 40 86 (reset JPEG engine): accepted")
    except usb.core.USBError as e:
        log(f"  request 40 86 (reset JPEG engine): {e}")
    if k(t, j, "R1 (after 0x86)"):
        return
    t.set_mode(t.find_mode(W, H, 60))
    t.cout(0x31, 0, 0)
    t.output(True)
    input("  mode set again. Press Enter once the LG shows a picture... ")
    if k(t, j, "R2 (after a mode set)"):
        return
    t.seq += 1
    img = card(W, H, 5, bg=(110, 40, 30))
    pitch = ((W + 31) // 32 * 32) * 3
    rows = np.zeros((H, pitch), np.uint8)
    rows[:, :W * 3] = np.ascontiguousarray(img[..., ::-1]).reshape(H, W * 3)
    payload = rows.tobytes()
    hdr = struct.pack("<4I2H3I I3I", 3, len(payload), t.seq, FMT_BGR24, pitch, 0,
                      FB[2] + 0x30, FB[2] + 0x30, 0, FMT_BGR24, 0, 0, 0)
    t.send(hdr + payload, dest=FB[2], more_flag=True)
    k(t, j, "R3 (after a full raw frame the vendor way)")


def x(t, j):
    """Request 0x86 on its own (MCT's t6.h VENDOR_REQ_RESET_JPEG_ENGINE; the
    newer firmware's dispatcher has no case for it), then one JPEG frame."""
    log("\n=== X: request 0x86 (reset JPEG engine), then a JPEG frame")
    try:
        t.cout(REQ_RESET_JPEG, 0, 0)
        log("  request 40 86: accepted")
    except usb.core.USBError as e:
        log(f"  request 40 86: {e} (this firmware doesn't have it)")
    fb = next(f for f in FB if f != j.shown)
    r = j.wait(j.flip(jpeg(card(W, H, 8, bg=(90, 30, 90)), 95), fb))
    log("  JPEG frame afterwards: " + ("no frame-done" if r is None else
                                     f"frame-done after {r[0] * 1000:.1f} ms" + (" (DECODER ERROR)" if r[1] else "")))
    ok = ask("The LG shows a test card on PURPLE with 9 white blocks, clean?")
    log(f"RESULT X: {'decoder works after 0x86' if ok else 'NOT shown after 0x86'}")


STEPS = {"J1": j1, "J2": j2, "J3": j3, "P": p, "K": k, "R": r, "X": x}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--steps", default="J1,P,J3,K,R")
    steps = [s.strip().upper() for s in ap.parse_args().steps.split(",") if s.strip()]
    for s in steps:
        if s not in STEPS:
            raise SystemExit(f"unknown step {s}; steps: {', '.join(STEPS)}")
    log(f"log: {log_path}; steps {','.join(steps)}")
    t = t6dev.T6()
    if t.status() != 1:
        log("no monitor on the T6's output: stopping")
        return
    e = t.edid()
    name = next((bytes(e[o + 5:o + 18]).split(b"\n")[0].decode("ascii", "replace")
                 for o in (54, 72, 90, 108) if e[o:o + 4] == b"\0\0\0\xfc"), "?")
    log(f"monitor: {name}")
    t.set_mode(t.find_mode(W, H, 60))
    t.cout(0x31, 0, 0)
    t.output(True)
    input("  1920x1080@60 set. Press Enter once the LG shows a picture (or a black screen)... ")
    j = Jpeg(t)
    try:
        for s in steps:
            if s == "J1" and not j1(t, j):
                log("J1 failed: the JPEG path doesn't work right now, stopping")
                return
            if s == "R":
                if "K" in steps and STEPS.get("_k_clean"):
                    continue
            if s == "K":
                STEPS["_k_clean"] = k(t, j)
                continue
            if s != "J1":
                STEPS[s](t, j)
    finally:
        log(f"\n{j.ev.count} interrupt events, {j.ev.errors} read errors; log: {log_path}")
        log("If the last clean check wasn't clean: unplug and replug the T6 now (the "
            "driver's picture stays tinted until the adapter is power-cycled).")
        j.close()


if __name__ == "__main__":
    try:
        main()
    finally:
        logf.close()
