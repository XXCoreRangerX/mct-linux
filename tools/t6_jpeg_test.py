#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Phase 0 of the T6 JPEG path (docs/roadmap.md, lab
captures/t6-research/jpeg-path.md): does the JUA254's JPEG decoder work, and
how fast is it? Run through tools/t6_run.sh (driver unbound, bound again at
the end). LG at 1920x1080@60 only; no other timing is touched.

  J1  three test cards as full-frame JPEGs (4:2:0, Q95, command 3) into the
      three frame buffers: right picture? frame-done events?
  J2  decode time at 1080p: flat, test card and noise at Q75 and Q95, one
      frame at a time (end of the send -> frame-done event), then back to
      back for frames per second
  J3  large JPEGs decoded into a hidden buffer with command 7 (no flip, no
      mode change): 2560x1440, 3440x1440, 3840x2160

Sends only what the driver sends anyway (set mode 0x12, software ready 0x31,
output 0x03, bulk sessions with signature 0). Log: /tmp/t6_jpeg-<time>.log.
"""
import io, os, statistics, struct, sys, time
import numpy as np
import usb.core
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import t6dev

W, H = 1920, 1080
FB = (0x0, 0xFFFEC0, 0x1FFFD80)          # mct_t6's three frame buffers
RING, RING_END = 0x3000000, 0x39FF800    # the bitstream ring (Windows: VRAM - 0xA00000)
FMT_NV12, FMT_JPEG = 6, 0xD
FLAG_RESET_JPEG = 0x80
EV_DONE, EV_JPEG_ERR = 0x04, 0x08

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
    """Test card: dark blue (or bg), white border, colour bars, grey ramp and
    n + 1 white blocks at the left (which buffer is shown)."""
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
    for y in range(h * 3 // 4, h - 40, 8):                 # fine lines: chroma/UV alignment
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


# ------------------------------------------------------------------ device

class Jpeg:
    def __init__(self, t):
        self.t = t
        self.ring = RING
        self.fence = 0
        self.first = True
        self.events = 0
        self.read_errors = 0

    def _payload(self, hdr, data):
        return hdr + data + bytes(1024)                 # Ubuntu daemon: JPEG + 1024 zero bytes

    def _send(self, payload, flags):
        dest = self.ring
        if dest + len(payload) > RING_END:
            dest = RING                                 # wrap: the decoder must reset
            flags |= FLAG_RESET_JPEG
            log("  ring wrapped", console=False)
        self.ring = (dest + len(payload) + 0xFFF) & ~0xFFF
        payload = bytearray(payload)
        payload[0x2F] = flags
        # fragments with their offset and "more" = 0: with "more" set on all but
        # the last (as raw frames take it) the 5600 showed nothing (2026-10-13)
        self.t.send(bytes(payload), dest=dest)

    def flip(self, data, fb, w=W, h=H):
        """Command 3: decode into frame buffer fb and show it."""
        self.fence += 1
        p0 = fb + 0x30
        hdr = struct.pack("<4I2H3II8x3xB", 3, len(data) + 1024, self.fence, FMT_NV12,
                          a16(w), a16(w), p0, p0 + a16(w) * a16(h), 0, FMT_JPEG, 0)
        flags = FLAG_RESET_JPEG if self.first else 0
        self.first = False
        self._send(self._payload(hdr, data), flags)
        return self.fence

    def decode(self, data, fb, w, h):
        """Command 7: decode a w x h JPEG into frame buffer fb, no flip."""
        self.fence += 1
        p0 = fb + 0x30
        hdr = struct.pack("<4I2H2H3II7xB", 7, len(data) + 1024, self.fence, FMT_NV12,
                          w, h, a16(w), a16(w), p0, p0 + a16(w) * a16(h), 0, FMT_JPEG, 0)
        assert len(hdr) == 48
        self._send(self._payload(hdr, data), 0)
        return self.fence

    def wait(self, fence, timeout=1.0):
        """-> (seconds until frame-done for fence, error flag), None on timeout."""
        t0 = time.perf_counter()
        while time.perf_counter() - t0 < timeout:
            try:
                ev = bytes(self.t.dev.read(t6dev.EP_ACK, 64, 100))
            except usb.core.USBTimeoutError:
                continue
            except usb.core.USBError as e:
                # pyusb's interrupt reads fail now and then (also in t6_probe
                # step E): clear the endpoint and go on
                self.read_errors += 1
                log(f"  event read error: {e}", console=False)
                try:
                    self.t.dev.clear_halt(t6dev.EP_ACK)
                except usb.core.USBError:
                    pass
                continue
            self.events += 1
            if len(ev) < 0x14:
                continue
            fid = struct.unpack_from("<I", ev, 0x0C)[0]
            log(f"  event {ev[:0x14].hex()} fence {fid}", console=False)
            if ev[0x13] & EV_JPEG_ERR:
                return time.perf_counter() - t0, True
            if ev[0x13] & EV_DONE and fid == fence:
                return time.perf_counter() - t0, False
        return None


# ------------------------------------------------------------------ steps

def j1(j):
    log("\n=== J1: three test cards as JPEG (4:2:0, Q95) into the three buffers")
    got = []
    for n, fb in enumerate(FB):
        data = jpeg(card(W, H, n), 95)
        f = j.flip(data, fb)
        r = j.wait(f)
        got.append(r)
        log(f"  buffer {n}: {len(data) // 1024} KiB, frame-done "
            + ("none" if r is None else f"after {r[0] * 1000:.1f} ms" + (" (DECODER ERROR)" if r[1] else "")))
    ok = ask("The LG shows a test card with 3 white blocks at the left, colour bars, a grey "
             "ramp and fine red lines at the bottom, colours right and no stripes?")
    log(f"RESULT J1: {'PASS' if ok else 'FAIL'}; frame-done events: "
        f"{sum(1 for r in got if r)} of 3")
    return ok


def j2(j):
    log("\n=== J2: decode time at 1080p, one frame at a time, then back to back")
    contents = (("flat grey", flat(W, H)),
                ("test card on GREEN with 5 white blocks", card(W, H, 4, bg=(20, 110, 40))),
                ("noise (coloured static)", noise(W, H)))
    results = []
    for name, img in contents:
        for q in (75, 95):
            data = jpeg(img, q)
            lat = []
            for k in range(30):
                r = j.wait(j.flip(data, FB[k % 3]))
                if r and not r[1]:
                    lat.append(r[0])
            if lat:
                med = statistics.median(lat)
                results.append((name, q, len(data), med))
                log(f"  {name:9s} Q{q}: {len(data) // 1024:5d} KiB, decode {med * 1000:6.2f} ms "
                    f"median ({min(lat) * 1000:.2f}-{max(lat) * 1000:.2f}), "
                    f"{W * H / med / 1e6:6.1f} Mpixel/s, {len(lat)}/30 events")
            else:
                log(f"  {name:9s} Q{q}: {len(data) // 1024:5d} KiB, no frame-done events")
            # back to back: keep at most 2 frames in flight
            t0, n, pending = time.perf_counter(), 60, []
            for k in range(n):
                pending.append(j.flip(data, FB[k % 3]))
                if len(pending) > 2:
                    j.wait(pending.pop(0))
            for f in pending:
                j.wait(f)
            dt = time.perf_counter() - t0
            log(f"  {'':9s}      back to back: {n / dt:6.1f} fps "
                f"({n * len(data) / dt / 1e6:.1f} MB/s over USB)")
        ok = ask(f"Does the LG show {name} now, without garbage?")
        log(f"RESULT J2 {name}: {'shown' if ok else 'NOT shown'}")
    return results


def j3(j):
    log("\n=== J3: large JPEGs decoded into a hidden buffer (command 7, no flip)")
    hidden = FB[1]          # J2 ended on FB[59 % 3] = FB[2]
    for w, h in ((1920, 1080), (2560, 1440), (3440, 1440), (3840, 2160)):
        if a16(w) * a16(h) * 3 // 2 + 0x30 > FB[1] - FB[0]:
            log(f"  {w}x{h}: does not fit a frame buffer, skipped")
            continue
        data = jpeg(card(w, h, 1), 95)
        lat = []
        for _ in range(10):
            r = j.wait(j.decode(data, hidden, w, h), timeout=2.0)
            if r is None:
                break
            if r[1]:
                log(f"  {w}x{h}: decoder error")
                break
            lat.append(r[0])
        if lat:
            med = statistics.median(lat)
            log(f"  {w}x{h}: {len(data) // 1024} KiB, decode {med * 1000:.2f} ms median, "
                f"{w * h / med / 1e6:.1f} Mpixel/s")
        else:
            log(f"  {w}x{h}: no frame-done event")
    ok = ask("The picture on the LG stayed the same during J3 (it decodes into a hidden "
             "buffer)?")
    log(f"RESULT J3: {'hidden as expected' if ok else 'the picture changed'}")


def main():
    log(f"log: {log_path}")
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
    j.wait(-1, timeout=0.3)         # drain old events (and clear a failed endpoint)
    if not j1(j):
        log("J1 failed: J2 and J3 skipped")
        return
    j2(j)
    j3(j)
    log(f"\n{j.events} interrupt events, {j.read_errors} read errors; log: {log_path}")


if __name__ == "__main__":
    try:
        main()
    finally:
        logf.close()
