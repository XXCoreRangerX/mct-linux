#!/usr/bin/env python3
"""Small pyusb toolkit for live Trigger 6 experiments (frame addressing,
partial updates, formats). The display driver must be unbound first, and the
device node must be accessible.

    t = T6(); t.status(); t.set_mode(t.find_mode(1920, 1080, 60)); t.output(True)
    t.send_raw(test_card(1920, 1080, "A"))

Header field values are arguments, so experiments can vary them; the defaults
are the layout that works on the 0711:5600 (see docs/protocol-t6.md).
"""
import struct, time
import usb.core, usb.util

VID = 0x0711
EP_OUT, EP_ACK = 0x02, 0x83
FRAG = 0x19000

REQ_EDID, REQ_STATUS, REQ_MODES, REQ_SETMODE, REQ_OUTPUT = 0x80, 0x87, 0x89, 0x12, 0x03
FMT_JPEG, FMT_NV12, FMT_BGR24 = 0xD, 0x6, 0x9

MODE_FIELDS = ("clock_khz refresh htotal hdisplay hsync_start hsync_len "
               "vtotal vdisplay vsync_start vsync_len u0 u1 u2 hpol vpol u3").split()
MODE_FMT = "<IH8H3HBBH"


class T6:
    def __init__(self, wait=10.0, output=0):
        t0 = time.time()
        while True:
            self.dev = usb.core.find(idVendor=VID, custom_match=lambda d: 0x5600 <= d.idProduct <= 0x56ff)
            if self.dev is not None:
                try:
                    usb.util.claim_interface(self.dev, 0)
                    break
                except usb.core.USBError:
                    pass
            if time.time() - t0 > wait:
                raise RuntimeError("T6 not available (driver still bound? device node permissions?)")
            time.sleep(0.3)
        self.out = output
        self.seq = 0

    def close(self):
        usb.util.release_interface(self.dev, 0)
        usb.util.dispose_resources(self.dev)

    # --- control requests
    def cin(self, req, value, index, n):
        return bytes(self.dev.ctrl_transfer(0xC0, req, value, index, n, 2000))

    def cout(self, req, value, index, data=b""):
        return self.dev.ctrl_transfer(0x40, req, value, index, data, 2000)

    def status(self, output=None):
        return self.cin(REQ_STATUS, self.out if output is None else output, 0, 1)[0]

    def edid(self, block=0):
        return self.cin(REQ_EDID, block * 128, self.out, 128)

    def modes(self):
        raw = self.cin(REQ_MODES, self.out, 0, 512) + self.cin(REQ_MODES, self.out, 512, 448)
        out = []
        for i in range(30):
            m = dict(zip(MODE_FIELDS, struct.unpack_from(MODE_FMT, raw, i * 32)))
            m["raw"] = raw[i * 32:(i + 1) * 32]
            if m["hdisplay"] and m["vdisplay"] and m["hdisplay"] != 0xFFFF:
                out.append(m)
        return out

    def find_mode(self, w, h, hz):
        for m in self.modes():
            if (m["hdisplay"], m["vdisplay"], m["refresh"]) == (w, h, hz):
                return m
        raise KeyError(f"{w}x{h}@{hz} not in the device's mode table")

    def set_mode(self, m):
        self.cout(REQ_SETMODE, self.out, 0, m["raw"])

    def output(self, on):
        self.cout(REQ_OUTPUT, self.out, 1 if on else 0)

    # --- frames
    def send(self, frame, dest=0x30, sess_type=0, more_flag=False, timeout=5000):
        """frame = video header + payload; one session header per fragment."""
        total = len(frame)
        for off in range(0, total, FRAG):
            frag = frame[off:off + FRAG]
            more = 1 if (more_flag and off + FRAG < total) else 0
            sess = struct.pack("<8I", sess_type, total, dest, len(frag), off, more, 0, 0)
            self.dev.write(EP_OUT, sess, timeout)
            self.dev.write(EP_OUT, frag, timeout)

    @staticmethod
    def video_header(htype, data_len, seq, unk4, w16, h16, start, end, unk9, fmt, extra=(0, 0, 0)):
        return struct.pack("<4I2H3I I3I", htype, data_len, seq, unk4, w16, h16, start, end, unk9,
                           fmt, *extra)

    def send_raw(self, bgr, w, htype=3, unk4=9, w16=None, h16=0, start=0x60, end=0x60,
                 dest=0x30, seq=None):
        """Full raw BGR24 frame with trigger6's header values unless overridden."""
        self.seq += 1
        n = 48 + len(bgr)
        hdr = self.video_header(htype, n, self.seq if seq is None else seq, unk4,
                                w * 3 if w16 is None else w16, h16, start, end, 0, FMT_BGR24)
        self.send(hdr + bgr, dest=dest)

    def acks(self, timeout=50):
        """Drain frame-done notifications from EP 0x83."""
        got = []
        while True:
            try:
                got.append(bytes(self.dev.read(EP_ACK, 64, timeout)))
            except usb.core.USBTimeoutError:
                return got


def test_card(w, h, label=0, rgb=(40, 40, 90)):
    """BGR24 test image: background, white border, a horizontal colour
    gradient band and `label` + 1 vertical bars at the left, so successive
    images are easy to tell apart and tearing shows up as a step."""
    img = bytearray(bytes((rgb[2], rgb[1], rgb[0])) * (w * h))

    def rect(x0, y0, x1, y1, bgr):
        line = bytes(bgr) * (x1 - x0)
        for y in range(y0, y1):
            img[(y * w + x0) * 3:(y * w + x1) * 3] = line

    rect(0, 0, w, 4, (255, 255, 255)); rect(0, h - 4, w, h, (255, 255, 255))
    rect(0, 0, 4, h, (255, 255, 255)); rect(w - 4, 0, w, h, (255, 255, 255))
    band = bytearray()
    for x in range(w):
        v = x * 255 // (w - 1)
        band += bytes((255 - v, 128, v))
    for y in range(max(0, h // 2 - 40), min(h, h // 2 + 40)):
        img[y * w * 3:(y + 1) * w * 3] = band
    for i in range(label + 1):
        rect(40 + i * 60, 80, 80 + i * 60, h - 80, (0, 255, 255))
    return bytes(img)
