#!/usr/bin/env python3
"""Small pyusb toolkit for live T2 experiments (init trimming, cursor).
Needs the video function accessible (udev MODE=0666 / NixOS module rule)."""
import os, struct, time
import usb.core, usb.util

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
PORT_DISABLE = "/sys/bus/usb/devices/6-3:1.0/6-3-port1/disable"   # tablet's internal hub, port 1
W, H = 1024, 600
FB_A, FB_B = 0x00C000, 0x1CE000


def load_init(path=os.path.join(ROOT, "t2d", "init.bin")):
    """-> list of (ep, flags, data_or_None, length, rdlen)"""
    b = open(path, "rb").read(); p = 0; recs = []
    while p < len(b):
        ep, fl, ln = struct.unpack_from("<BBI", b, p); p += 6
        d = None
        if not fl & 1:
            d = b[p:p + ln]; p += ln
        rd = None
        if fl & 2:
            rd = struct.unpack_from("<I", b, p)[0]; p += 4
        recs.append((ep, fl, d, ln, rd))
    return recs


def port_cycle(off_s=3.0):
    """Disable/enable the hub port the T2 sits on (re-enumerates the device)."""
    open(PORT_DISABLE, "w").write("1")
    time.sleep(off_s)
    open(PORT_DISABLE, "w").write("0")


def pace_for(ep, data):
    """Selective pacing for the 8051 firmware (validated cold, see docs/init-sequence.md)."""
    if ep == 2:
        return 0.0
    if ep == 3 and data[0] == 0x02:                      # single register write
        return 0.05 if data[3] == 0xFE and data[4] in (0xB0, 0xB1) else 0.002
    return 0.02                                          # lists, timing/mode packets, 0x0a, 0x90, 0x1d, EP4


class T2:
    def __init__(self, wait=15.0):
        t0 = time.time()
        while True:
            self.dev = usb.core.find(idVendor=0x0711, custom_match=lambda d: d.idProduct in (0x5209, 0x520d))
            if self.dev is not None:
                try:
                    usb.util.claim_interface(self.dev, 0)
                    break
                except usb.core.USBError:
                    pass
            if time.time() - t0 > wait:
                raise RuntimeError("tablet not available")
            time.sleep(0.3)

    def close(self):
        usb.util.release_interface(self.dev, 0)
        usb.util.dispose_resources(self.dev)

    # --- raw
    def out(self, ep, data, timeout=5000):
        self.dev.write(ep, data, timeout)

    def read(self, n=512, timeout=3000):
        return bytes(self.dev.read(0x81, max(n, 1), timeout))

    def reg(self, addr):
        self.out(3, bytes([0x05, addr & 0xFF, addr >> 8]))
        return self.read(1)[0]

    def wreg(self, addr, val):
        self.out(3, bytes([0x02, 0x00, 0x01, addr >> 8, addr & 0xFF, val]))

    # --- init
    def run_init(self, recs, pace_ctrl=0.02, pace_fb=0.005, log=None, smart=False):
        for i, (ep, fl, d, ln, rd) in enumerate(recs):
            data = bytes(ln) if fl & 1 else d
            self.out(ep, data, 30000)
            if fl & 2:
                self.read(rd)
            if smart:
                time.sleep(pace_for(ep, data))
            elif ep != 2 or ln < 64:
                time.sleep(pace_ctrl if ep != 2 else pace_fb)
            if log:
                log(i)

    # --- frames
    def frame(self, addr, rgb, x=0, y=0, w=W, h=H):
        import t2rle
        pay = t2rle.encode(rgb, w, h)
        a = (addr + y * W * 3 + x * 3) << 8
        hdr = (b"\x13" + a.to_bytes(4, "little") * 2 + b"\x00"
               + struct.pack("<HHHH", w, h, w, h) + bytes.fromhex("004000400000003900")
               + (w * h * 3).to_bytes(3, "little") + len(pay).to_bytes(3, "little")
               + bytes.fromhex("300528"))
        self.out(2, hdr)
        for k in range(0, len(pay), 19968):
            self.out(2, pay[k:k + 19968], 30000)

    def flip(self, addr):
        a = addr >> 2
        self.out(2, bytes([0x15, 0x07, 0xfb, 0x65, 0x10 if addr == FB_A else 0x20,
                           0xfc, 0xb4, 0x11, 0xfc, 0xb1, a & 0xff, 0xfc, 0xb2, (a >> 8) & 0xff,
                           0xfc, 0xb3, (a >> 16) & 0xff, 0xfc, 0xb4, 0x15, 0xfc, 0xb4, 0x11]))

    def show(self, rgb, addr=FB_B):
        """Send a full visible-orientation RGB image (rotated 180 here) + flip."""
        px = [rgb[i:i + 3] for i in range(0, len(rgb), 3)]
        self.frame(addr, b"".join(reversed(px)))
        self.flip(addr)


def test_pattern():
    img = bytearray(W * H * 3)
    def rect(x0, y0, x1, y1, c):
        for yy in range(y0, y1):
            img[(yy * W + x0) * 3:(yy * W + x1) * 3] = bytes(c) * (x1 - x0)
    rect(0, 0, W, H, (30, 30, 60))
    rect(0, 0, 200, 200, (255, 0, 0)); rect(W - 200, 0, W, 200, (0, 255, 0))
    rect(0, H - 200, 200, H, (0, 0, 255)); rect(W - 200, H - 200, W, H, (255, 255, 255))
    for xx in range(0, W, 4):
        v = xx * 255 // W
        rect(xx, 260, xx + 4, 340, (v, v, v))
    return bytes(img)
