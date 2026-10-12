#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Filip Piękoś <mail@filippiekos.com>
"""Interactive probe for unverified T2 frame commands (DTU-1031X, 0711:520d).

Tests features found by static analysis of the vendor drivers
(captures/t2-research/binary-analysis.md) that no captured traffic uses:
large payload transfers, raw 0x11 rects, hardware upscaling, RGB565,
0x13 with separate source/target addresses, YUV 4:2:2 RLE. Steps, in order
of risk, each one skippable:

  a  control: 0x13 RGB24 RLE rect, header exactly as Windows sends it
  b  full frame 0x13 with the payload in 512 KiB transfers (Windows URB size)
  c  0x11 raw RGB24 rect (B,G,R on the wire), scale 0x4000
  d  0x11 half-size source, scale 0x2000 (2x hardware upscale), source-size payload
  e  0x11 RGB565 (format 0x04), 2 bytes per pixel, little endian
  f  0x13 decoded to a scratch VRAM address, target in the back buffer, scale 1.0
  g  0x13 2x upscale (half-size source)
  h  0x13 YUV 4:2:2 RLE (format 0x8a, mode 6, Y/U/V planes 1536/768/768 per unit)
  i  HIGH RISK, VRAM blit engine: not implemented, see captures/t2-research/blit-engine.md

How it works: run the normal init, paint buffer A dark blue and show it,
paint buffer B (hidden) dark green. Every step draws into B only: a
reference box through the known-good 0x13 path on the left, the probed
command on the right (boxes at visible x 320 and 576, y 268, 128x64).
Then it flips to B, asks what you see, and flips back to A.
After every command written to EP 0x02 it reads register fc01 over EP 0x03
(05 01 fc, reply 07) with a short timeout. No reply means the EP2 parser is
still waiting for payload bytes (docs/reference/protocol-t2.md, "Detecting an
interrupted framebuffer payload"): the tool then stops the step, sends zero
padding until the queued reply arrives (small chunks first, so the byte
count tells how much the chip still wanted), runs the normal init and checks
fc01/feb0/fcf6.

Risks: a header the firmware does not like can leave the parser waiting
or write VRAM where it should not. The padding recovery usually fixes the
first; if the chip does not answer within 45 s it needs a power cycle
(unplug and replug the tablet). The tool never sends a USB reset (that can
make this chip drop off the bus until a power cycle). Step f writes VRAM
at 0x3a0000 (above the RLE staging buffer at 0x390000, inside space Windows
uses for staging big frames). Step b overwrites all of buffer B.

Needs the tablet unbound from mct (the tool refuses otherwise and prints the
commands), pyusb and access to the device node (the NixOS module's uaccess
rule, or root). Run from the tools directory or anywhere:

  echo 6-3.1:1.0 | sudo tee /sys/bus/usb/drivers/mct/unbind
  python3 tools/t2_probe.py [--steps a,c-e] [--log ~/lab]
  echo 6-3.1:1.0 | sudo tee /sys/bus/usb/drivers/mct/bind

  t2_probe.py --dry-run [--steps ...] [--full-hex]   print every packet, no USB
  t2_probe.py --self-test                            check the packet builders

Log: <lab dir>/captures/t2_probe-<time>.log with --log, else
/tmp/t2_probe-<time>.log. Payloads up to 32 KiB are logged in full, bigger
ones as length, sha256 and head/tail (they are deterministic, --dry-run
--full-hex prints them).
"""
import argparse, datetime, errno, glob, hashlib, os, signal, struct, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import t2rle

# pyusb and t2dev (which imports pyusb) are only loaded for a live run, so
# --dry-run and --self-test work without them.
usb = None
t2dev = None

# --- device constants (same values as t2dev.py / kernel/mct_t2.c)
W, H = 1024, 600
STRIDE = W * 3
FB_A, FB_B = 0x00C000, 0x1CE000
RLD_BUF = 0x390000          # 0x13 byte 23: VRAM where the compressed stream is staged
SCRATCH = 0x3A0000          # step f decode target: 64 KiB above RLD_BUF
CHUNK_13 = 19968            # vendor 0x13 payload transfer size
CHUNK_11 = 16384            # vendor 0x11 payload transfer size
CHUNK_BIG = 512 * 1024      # Windows' URB size (capture-analysis.md)
READ_FC01 = bytes((0x05, 0x01, 0xFC))
FC01_OK = 0x07
PAD_FINE, PAD_FINE_UNTIL = 512, 64 * 1024     # recovery: 512 B steps first
PAD_COARSE, PAD_MAX = 19968, 2 * 1024 * 1024  # then vendor chunks, up to 2 MiB
RECOVERY_S = 45             # the firmware can sit ~30 s in its own timeout
BIND_DRV = "/sys/bus/usb/drivers"

# --- colours (visible orientation; no 0x30 anywhere, it is the RLE escape)
BG_A = (0x18, 0x20, 0x58)   # dark blue: buffer A, shown between steps
BG_B = (0x18, 0x50, 0x20)   # dark green: buffer B, shown during a step
RED, GREEN, BLUE = (0xFF, 0, 0), (0, 0xFF, 0), (0, 0, 0xFF)
YELLOW, WHITE = (0xFF, 0xFF, 0), (0xFF, 0xFF, 0xFF)

# --- boxes in buffer B, visible coordinates (the panel scans out rotated 180)
BOX_W, BOX_H = 128, 64
REF_VX, TEST_VX, BOX_VY = 320, 576, 268


class Fatal(Exception):
    """The chip does not answer any more; the user has to power-cycle it."""


class Quit(Exception):
    pass


# ---------------------------------------------------------------------------
# packet builders

def q214(src, dst):
    """Scale factor src/dst in 2.14 fixed point, like FUN_14002ad1c (0x4000 = 1.0)."""
    if src == dst:
        return 0x4000
    return int(src * 16384 // dst)


def hdr11(addr, sw, sh, dw, dh, fmt, length):
    """0x11 StretchCmd, 21 bytes: target address, source and target size,
    scale, format byte, payload length (bytes of source data)."""
    return (b"\x11" + struct.pack("<IHHHHHHB", addr, sw, sh, dw, dh,
                                  q214(sw, dw), q214(sh, dh), fmt)
            + length.to_bytes(3, "little"))


def hdr13(src, dst, sw, sh, dw, dh, fmt, rawlen, complen, esc=0x30, mode=5, flags=0x28,
          rld=RLD_BUF):
    """0x13 IndirRldCmd, 36 bytes (binary-analysis.md section 1.2)."""
    return (b"\x13\x00" + struct.pack("<IIHHHHHHBI", src, dst, sw, sh, dw, dh,
                                      q214(sw, dw), q214(sh, dh), fmt, rld)
            + rawlen.to_bytes(3, "little") + complen.to_bytes(3, "little")
            + bytes((esc, mode, flags)))


def flip_bytes(buf):
    """0x15 flip to buffer A or B, as t2dev.T2.flip sends it."""
    a = buf >> 2
    return bytes([0x15, 0x07, 0xfb, 0x65, 0x10 if buf == FB_A else 0x20,
                  0xfc, 0xb4, 0x11, 0xfc, 0xb1, a & 0xff, 0xfc, 0xb2, (a >> 8) & 0xff,
                  0xfc, 0xb3, (a >> 16) & 0xff, 0xfc, 0xb4, 0x15, 0xfc, 0xb4, 0x11])


def describe(hdr):
    """One line with the fields of a 0x11/0x13/0x15 header."""
    op = hdr[0]
    if op == 0x11 and len(hdr) == 21:
        a, sw, sh, dw, dh, sx, sy, fmt = struct.unpack_from("<IHHHHHHB", hdr, 1)
        ln = int.from_bytes(hdr[18:21], "little")
        return (f"0x11 addr {a:#08x}  src {sw}x{sh}  dst {dw}x{dh}  scale {sx:#06x}/{sy:#06x}"
                f"  fmt {fmt:#04x}  len {ln}")
    if op == 0x13 and len(hdr) == 36:
        s, d, sw, sh, dw, dh, sx, sy, fmt, rld = struct.unpack_from("<IIHHHHHHBI", hdr, 2)
        raw = int.from_bytes(hdr[27:30], "little")
        comp = int.from_bytes(hdr[30:33], "little")
        return (f"0x13 src {s:#08x}  dst {d:#08x}  src {sw}x{sh}  dst {dw}x{dh}"
                f"  scale {sx:#06x}/{sy:#06x}  fmt {fmt:#04x}  rld {rld:#08x}  raw {raw}"
                f"  comp {comp}  esc {hdr[33]:02x} mode {hdr[34]:02x} flags {hdr[35]:02x}")
    if op == 0x15:
        a = hdr[10] | hdr[13] << 8 | hdr[16] << 16
        return f"0x15 flip  SEL {hdr[4]:#04x}  scanout {a << 2:#08x}"
    return f"opcode {op:#04x}, {len(hdr)} bytes"


# ---------------------------------------------------------------------------
# images: built in visible orientation as packed RGB24 bytes, then rotated

def solid(w, h, c):
    return bytes(c) * (w * h)


def quads(w, h):
    """Red top left, green top right, blue bottom left, yellow bottom right,
    1 px white border. Tells R/B swaps, mirroring and row shifts apart."""
    img = bytearray(w * h * 3)
    for y in range(h):
        for x in range(w):
            if x in (0, w - 1) or y in (0, h - 1):
                c = WHITE
            elif y < h // 2:
                c = RED if x < w // 2 else GREEN
            else:
                c = BLUE if x < w // 2 else YELLOW
            img[(y * w + x) * 3:(y * w + x) * 3 + 3] = bytes(c)
    return bytes(img)


def upscale2(rgb, w, h):
    """Nearest-neighbour 2x: what a perfect hardware upscale would show."""
    out = bytearray()
    for y in range(h):
        row = bytearray()
        for x in range(w):
            row += rgb[(y * w + x) * 3:(y * w + x) * 3 + 3] * 2
        out += row + row
    return bytes(out)


def rot180(rgb):
    """Visible -> device orientation (the panel scans out rotated 180 degrees)."""
    return b"".join(reversed([rgb[i:i + 3] for i in range(0, len(rgb), 3)]))


def gradient_frame():
    """Full frame for step b: red sawtooth (4 ramps across), green ramp top to
    bottom, blue ramp across in 4 px steps. Literal-heavy, so the RLE payload
    is about 1 MB; corrupted bytes show up as specks in the smooth ramps."""
    red = bytes(x & 0xFF for x in range(W))
    blue = bytes((x // 4) & 0xFF for x in range(W))
    img = bytearray(W * H * 3)
    for y in range(H):
        row = bytearray(W * 3)
        row[0::3] = red
        row[1::3] = bytes([0x40 + y * 0xA0 // (H - 1)]) * W
        row[2::3] = blue
        img[y * STRIDE:(y + 1) * STRIDE] = row
    return bytes(img)


def to_bgr(rgb):
    """0x11 payload byte order: B, G, R (EncodeRgbDTR copies BGRX bytes 0..2)."""
    out = bytearray(len(rgb))
    out[0::3], out[1::3], out[2::3] = rgb[2::3], rgb[1::3], rgb[0::3]
    return bytes(out)


def to_565(rgb):
    """RGB565 little endian, the 5-6-5 positions FUN_14002dc28 uses for format 4."""
    out = bytearray()
    for i in range(0, len(rgb), 3):
        r, g, b = rgb[i:i + 3]
        out += ((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)).to_bytes(2, "little")
    return bytes(out)


def from_565(pay):
    """Inverse of to_565 (bit replication), for the reference box and self-test."""
    out = bytearray()
    for i in range(0, len(pay), 2):
        v = int.from_bytes(pay[i:i + 2], "little")
        r, g, b = v >> 11, (v >> 5) & 0x3F, v & 0x1F
        out += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))
    return bytes(out)


# YUV test bands for step h: (Y, U, V) and the RGB it should look like
# (BT.601, limited range; the chip's matrix and range are unknown)
YUV_BANDS = [((235, 128, 128), WHITE), ((128, 128, 128), (130, 130, 130)),
             ((16, 128, 128), (0, 0, 0)), ((81, 90, 240), RED)]


def yuv_bands(w, h):
    """-> (YUY2 bytes in device orientation, RGB reference in visible orientation)."""
    bh = h // len(YUV_BANDS)
    px, ref = [], bytearray()
    for y in range(h):
        yuv, rgb = YUV_BANDS[min(y // bh, len(YUV_BANDS) - 1)]
        px += [yuv] * w
        ref += bytes(rgb) * w
    px.reverse()                            # rotate 180
    out = bytearray()
    for i in range(0, len(px), 2):          # Y0 U Y1 V
        out += bytes((px[i][0], px[i][1], px[i + 1][0], px[i][2]))
    return bytes(out), bytes(ref)


# ---------------------------------------------------------------------------
# encoders (t2rle does the RLE itself)

def rle_rect(dev_rgb, w, h):
    """RGB24 rect in device orientation -> 0x13 mode 5 payload: the pixels row
    major, cut into 1024-px units, each unit as R, G, B lines (like the kernel)."""
    assert w * h % 1024 == 0, "3K alignment: w*h must be a multiple of 1024"
    return t2rle.encode(dev_rgb, 1024, w * h // 1024)


def rle_yuy2(yuyv):
    """YUY2 bytes -> 0x13 mode 6 payload: per 3 KiB unit (1536 px) a Y line of
    1536 values, a U line of 768, a V line of 768 (legacy FUN_1400280f8)."""
    assert len(yuyv) % 3072 == 0, "YUV units are 1536 px"
    out = bytearray()
    for u in range(0, len(yuyv), 3072):
        unit = yuyv[u:u + 3072]
        for plane in (unit[0::2], unit[1::4], unit[3::4]):
            t2rle._encode_line(plane, out)
    return bytes(out)


def rle_line_decode(pay, i, n):
    """Decode one RLE line of n values starting at pay[i] -> (values, next i)."""
    vals = bytearray()
    while len(vals) < n:
        if pay[i] == 0x30:
            vals += bytes([pay[i + 2]]) * (pay[i + 1] + 1)
            i += 3
        else:
            vals.append(pay[i])
            i += 1
    if len(vals) != n:
        raise ValueError("run crosses the end of a line")
    return bytes(vals), i


# ---------------------------------------------------------------------------
# geometry

def dev_xy(vx, vy, w, h):
    """Visible top-left -> device top-left of the same box."""
    return W - vx - w, H - vy - h


def fb_addr(buf, vx, vy, w, h):
    dx, dy = dev_xy(vx, vy, w, h)
    assert dx % 64 == 0 and dx >= 0 and dx + w <= W and 0 <= dy and dy + h <= H
    return buf + dy * STRIDE + dx * 3


# ---------------------------------------------------------------------------
# commands

class Cmd:
    """One EP2 command: header in its own transfer, payload in `chunk`-sized
    transfers (the firmware parses EP2 per transfer, protocol-t2.md)."""

    def __init__(self, name, header, payload=b"", chunk=CHUNK_13):
        self.name, self.header, self.payload, self.chunk = name, header, payload, chunk

    def transfers(self):
        yield self.header
        for k in range(0, len(self.payload), self.chunk):
            yield self.payload[k:k + self.chunk]


def cmd_rle(name, buf, vx, vy, w, h, vis_rgb):
    """Known-good path: 0x13 RGB24 RLE, header exactly as Windows builds it."""
    a = fb_addr(buf, vx, vy, w, h)
    pay = rle_rect(rot180(vis_rgb), w, h)
    return Cmd(name, hdr13(a, a, w, h, w, h, 0x00, w * h * 3, len(pay)), pay)


def cmd_full(name, buf, color):
    return cmd_rle(name, buf, 0, 0, W, H, solid(W, H, color))


def cmd_flip(buf):
    return Cmd(f"flip to {'A' if buf == FB_A else 'B'}", flip_bytes(buf))


# ---------------------------------------------------------------------------
# steps: each returns (commands drawn into B before the flip, what to look for)

def prep(ref_rgb, h=BOX_H):
    """Clean buffer B and draw the reference box on the left."""
    return [cmd_full("clear buffer B (dark green)", FB_B, BG_B),
            cmd_rle("reference box (known-good 0x13)", FB_B, REF_VX, BOX_VY, BOX_W, h, ref_rgb)]


QUADS_TEXT = ("red top left, green top right, blue bottom left, yellow bottom right, "
              "thin white border")


def step_a(opt):
    img = quads(BOX_W, BOX_H)
    cmds = prep(img) + [cmd_rle("test box: the same 0x13 again", FB_B, TEST_VX, BOX_VY,
                                BOX_W, BOX_H, img)]
    return cmds, f"two equal boxes on dark green: {QUADS_TEXT}"


def step_b(opt):
    pay = rle_rect(rot180(gradient_frame()), W, H)
    hdr = hdr13(FB_B, FB_B, W, H, W, H, 0x00, W * H * 3, len(pay))
    note = ""
    if len(pay) % 512 == 0:
        note = " (payload is a multiple of 512: no short packet ends it, see docstring)"
    return ([Cmd("full frame, payload in 512 KiB transfers" + note, hdr, pay, CHUNK_BIG)],
            "the whole screen: 4 red ramps across, green getting brighter downwards, "
            "blue ramp across; smooth, no specks or wrong pixels (look closely at the top: "
            "the end of the payload is the top of the visible picture)")


def step_c(opt):
    img = quads(BOX_W, BOX_H)
    a = fb_addr(FB_B, TEST_VX, BOX_VY, BOX_W, BOX_H)
    pay = to_bgr(rot180(img))
    return (prep(img) + [Cmd("test box: 0x11 raw RGB24 (B,G,R)",
                             hdr11(a, BOX_W, BOX_H, BOX_W, BOX_H, 0x00, len(pay)), pay, CHUNK_11)],
            f"two equal boxes: {QUADS_TEXT}. Red and blue swapped on the right = "
            "the wire order is R,G,B")


def step_d(opt):
    sw, sh = BOX_W // 2, BOX_H // 2
    src = quads(sw, sh)
    a = fb_addr(FB_B, TEST_VX, BOX_VY, BOX_W, BOX_H)
    pay = to_bgr(rot180(src))
    return (prep(upscale2(src, sw, sh)) +
            [Cmd(f"test box: 0x11 {sw}x{sh} -> {BOX_W}x{BOX_H}, scale 0x2000",
                 hdr11(a, sw, sh, BOX_W, BOX_H, 0x00, len(pay)), pay, CHUNK_11)],
            f"two boxes of the same size: {QUADS_TEXT} (2 px on the left). The right one "
            "may be softer if the chip filters. Right box only a quarter filled, or "
            "repeated 4 times = no scaling, payload taken as target pixels")


def step_e(opt):
    img = quads(BOX_W, BOX_H)
    a = fb_addr(FB_B, TEST_VX, BOX_VY, BOX_W, BOX_H)
    pay = to_565(rot180(img))
    return (prep(from_565(to_565(img))) +
            [Cmd("test box: 0x11 RGB565 (fmt 0x04)",
                 hdr11(a, BOX_W, BOX_H, BOX_W, BOX_H, 0x04, len(pay)), pay, CHUNK_11)],
            f"two equal boxes: {QUADS_TEXT}. Swapped red/blue = BGR565; "
            "garbage stripes = the format byte is not RGB565")


def step_f(opt):
    img = quads(BOX_W, BOX_H)
    dst = fb_addr(FB_B, TEST_VX, BOX_VY, BOX_W, BOX_H)
    pay = rle_rect(rot180(img), BOX_W, BOX_H)
    assert RLD_BUF + len(pay) <= opt.scratch, "staged stream would overlap the scratch area"
    return (prep(img) +
            [Cmd(f"test box: 0x13 decoded at {opt.scratch:#x}, target in B",
                 hdr13(opt.scratch, dst, BOX_W, BOX_H, BOX_W, BOX_H, 0x00, BOX_W * BOX_H * 3,
                       len(pay)), pay)],
            f"two equal boxes: {QUADS_TEXT}. Right box missing = the chip decodes to the "
            "source address only (no copy to the target)")


def step_g(opt):
    sw, sh = BOX_W // 2, BOX_H // 2
    src = quads(sw, sh)
    dst = fb_addr(FB_B, TEST_VX, BOX_VY, BOX_W, BOX_H)
    s = opt.scratch if opt.stretch_src == "scratch" else dst
    pay = rle_rect(rot180(src), sw, sh)
    return (prep(upscale2(src, sw, sh)) +
            [Cmd(f"test box: 0x13 {sw}x{sh} -> {BOX_W}x{BOX_H}, scale 0x2000, "
                 f"src {'scratch' if s != dst else '= target'}",
                 hdr13(s, dst, sw, sh, BOX_W, BOX_H, 0x00, sw * sh * 3, len(pay)), pay)],
            f"two boxes of the same size: {QUADS_TEXT}. Right box maybe softer. Only the "
            "top/left quarter drawn = no scaling")


def step_h(opt):
    h = 48                                  # 128x48 = 4 units of 1536 px
    yuyv, ref = yuv_bands(BOX_W, h)
    dst = fb_addr(FB_B, TEST_VX, BOX_VY, BOX_W, h)
    s = opt.scratch if opt.stretch_src == "scratch" else dst
    pay = rle_yuy2(yuyv)
    return (prep(ref, h) +
            [Cmd(f"test box: 0x13 YUV 4:2:2 RLE (fmt 0x8a, mode 6), "
                 f"src {'scratch' if s != dst else '= target'}",
                 hdr13(s, dst, BOX_W, h, BOX_W, h, 0x8a, len(yuyv), len(pay), mode=6), pay)],
            "two boxes with 4 bands top to bottom: white, grey, black, red. On the right "
            "light grey instead of white = full-range YUV; red turned blue/purple = U and V "
            "swapped; coloured noise = wrong plane layout")


STEPS = [
    ("a", "control: 0x13 RGB24 RLE rect as Windows sends it", "low", step_a),
    ("b", "full frame, payload in 512 KiB transfers", "low", step_b),
    ("c", "0x11 raw RGB24 rect, scale 0x4000", "low", step_c),
    ("d", "0x11 2x hardware upscale (half-size source)", "medium", step_d),
    ("e", "0x11 RGB565 (format 0x04)", "low-medium", step_e),
    ("f", "0x13 with source != target (scratch decode)", "medium-high", step_f),
    ("g", "0x13 2x upscale (half-size source)", "medium-high", step_g),
    ("h", "0x13 YUV 4:2:2 RLE (format 0x8a, mode 6)", "medium-high", step_h),
    ("i", "VRAM blit engine (0x14 / 0x15 register program)", "HIGH", None),
]


def parse_steps(spec):
    keys = [k for k, *_ in STEPS]
    if spec == "all":
        return keys
    out = []
    for part in spec.split(","):
        part = part.strip()
        if "-" in part:
            a, b = part.split("-")
            out += keys[keys.index(a):keys.index(b) + 1]
        elif part in keys:
            out.append(part)
        else:
            raise SystemExit(f"unknown step {part!r}; steps are {''.join(keys)}")
    return [k for k in keys if k in out]


# ---------------------------------------------------------------------------
# output and logging

def hexs(b):
    return b.hex(" ")


class Log:
    def __init__(self, path):
        self.path = path
        self.f = None
        if path:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            self.f = open(path, "a")

    def __call__(self, kind, text=""):
        if self.f:
            ts = datetime.datetime.now().isoformat(timespec="milliseconds")
            self.f.write(f"{ts} {kind} {text}\n")
            self.f.flush()

    def data(self, kind, b):
        if len(b) <= 32768:
            self(kind, f"{len(b)} B {b.hex()}")
        else:
            self(kind, f"{len(b)} B sha256 {hashlib.sha256(b).hexdigest()} "
                       f"head {b[:64].hex()} tail {b[-64:].hex()}")


def print_cmd(cmd, full, timeout_ms):
    print(f"  EP2  {cmd.name}")
    print(f"       header {len(cmd.header)} B: {hexs(cmd.header)}")
    print(f"       {describe(cmd.header)}")
    p = cmd.payload
    if p:
        n = (len(p) + cmd.chunk - 1) // cmd.chunk
        print(f"       payload {len(p)} B in {n} transfer(s) of up to {cmd.chunk} B, "
              f"sha256 {hashlib.sha256(p).hexdigest()[:16]}")
        if full or len(p) <= 64:
            for k in range(0, len(p), 64):
                print(f"         {p[k:k + 64].hex()}")
        else:
            print(f"         head {p[:32].hex()}")
            print(f"         tail {p[-16:].hex()}")
    print(f"  EP3  health check: {hexs(READ_FC01)} -> expect {FC01_OK:02x} within {timeout_ms} ms")


def ask(prompt, choices="yn"):
    """y/n (or y/n/q) on stdin; q or EOF quits."""
    while True:
        try:
            a = input(f"{prompt} [{'/'.join(choices)}] ").strip().lower()
        except EOFError:
            raise Quit()
        if a == "q" and "q" in choices:
            raise Quit()
        if a and a[0] in choices:
            return a[0]


def ask_note():
    try:
        return input("  note (Enter for none): ").strip()
    except EOFError:
        return ""


class HoldCtrlC:
    """Delay Ctrl-C until an EP2 command is complete: quitting halfway through a
    payload leaves the parser waiting (the very state this tool tries to avoid)."""

    def __enter__(self):
        self.hit = False
        self.old = signal.signal(signal.SIGINT, self._handler)
        return self

    def _handler(self, *a):
        self.hit = True
        print("\n(Ctrl-C held until this command is complete)", flush=True)

    def __exit__(self, typ, *a):
        signal.signal(signal.SIGINT, self.old)
        if self.hit and typ is None:
            raise KeyboardInterrupt


# ---------------------------------------------------------------------------
# device access

def is_timeout(e):
    return (isinstance(e, getattr(usb.core, "USBTimeoutError", ()))
            or getattr(e, "errno", None) == errno.ETIMEDOUT)


def find_tablets():
    """T2 interfaces from sysfs (no USB access): [(intf, product, driver or None)]."""
    found = []
    for d in sorted(glob.glob("/sys/bus/usb/devices/*:1.0")):
        dev = os.path.dirname(os.path.realpath(d))
        try:
            vid = open(os.path.join(dev, "idVendor")).read().strip()
            pid = open(os.path.join(dev, "idProduct")).read().strip()
        except OSError:
            continue
        if vid == "0711" and pid in ("520d", "5209"):
            drv = os.path.join(d, "driver")
            found.append((os.path.basename(d), pid,
                          os.path.basename(os.path.realpath(drv)) if os.path.exists(drv) else None))
    return found


def bind_cmds(intf, drv="mct"):
    return (f"echo {intf} | sudo tee {BIND_DRV}/{drv}/unbind",
            f"echo {intf} | sudo tee {BIND_DRV}/mct/bind")


class Probe:
    def __init__(self, dev, log, dry, full_hex, timeout_ms):
        self.t, self.log, self.dry = dev, log, dry
        self.full_hex, self.timeout_ms = full_hex, timeout_ms
        self.in_recovery = False
        self.last_pad = None            # bytes of padding the last recovery needed

    # --- low level
    def _out(self, ep, data, timeout):
        self.log.data(f"EP{ep:x} OUT", data)
        self.t.out(ep, data, timeout)

    def _read(self, timeout):
        return bytes(self.t.dev.read(0x81, 512, timeout))

    def drain(self):
        """Throw away replies left from earlier commands."""
        for _ in range(16):
            try:
                r = self._read(20)
            except usb.core.USBError as e:
                if is_timeout(e):
                    return
                raise
            self.log("EP81 drained", r.hex())

    def reg(self, addr, timeout=1000):
        """Register read; None on timeout."""
        self._out(3, bytes((0x05, addr & 0xFF, addr >> 8)), 2000)
        try:
            r = self._read(timeout)
        except usb.core.USBError as e:
            if is_timeout(e):
                return None
            raise
        self.log("EP81 IN", r.hex())
        return r[0] if r else None

    def verify(self):
        """Registers the normal init leaves at known values."""
        self.drain()
        got = {a: self.reg(a) for a in (0xfc01, 0xfeb0, 0xfcf6)}
        want = {0xfc01: FC01_OK, 0xfeb0: 0x40, 0xfcf6: 0x01}
        text = " ".join(f"{a:04x}={'--' if v is None else f'{v:02x}'}" for a, v in got.items())
        self.log("VERIFY", text)
        return got == want, text

    # --- EP2 command plus health check
    def send(self, cmd):
        """Send one EP2 command and check the parser. True = in step;
        False = it was stuck and has been recovered (device re-initialised)."""
        if self.dry:
            print_cmd(cmd, self.full_hex, self.timeout_ms)
            return True
        self.log("CMD", f"{cmd.name}: {describe(cmd.header)}")
        with HoldCtrlC():
            try:
                for i, tr in enumerate(cmd.transfers()):
                    self._out(2, tr, 2000 if i == 0 else 30000)
            except usb.core.USBError as e:
                if not is_timeout(e):
                    raise Fatal(f"USB error while sending {cmd.name}: {e}")
                self.log("EP2 TIMEOUT", cmd.name)
                print(f"!! EP2 write timed out during '{cmd.name}'")
                return self.stuck(cmd.name, read_queued=False)
            return self.health(cmd.name)

    def health(self, after):
        """EP3 05 01 fc, expect 07 within the short timeout."""
        self.drain()
        t0 = time.monotonic()
        try:
            self._out(3, READ_FC01, 2000)
        except usb.core.USBError as e:
            raise Fatal(f"EP3 write failed ({e}); the chip does not take commands")
        try:
            r = self._read(self.timeout_ms)
        except usb.core.USBError as e:
            if not is_timeout(e):
                raise Fatal(f"EP81 read failed: {e}")
            self.log("HEALTH TIMEOUT", f"after {after}")
            print(f"!! no reply to the fc01 read after '{after}': the parser waits for more bytes")
            return self.stuck(after, read_queued=True)
        ms = (time.monotonic() - t0) * 1000
        self.log("HEALTH", f"after {after}: {r.hex()} in {ms:.0f} ms")
        if r[:1] == bytes((FC01_OK,)):
            print(f"       health: fc01 = {FC01_OK:02x} ({ms:.0f} ms), parser in step")
            return True
        # a reply, but not the one expected: replies out of order or a changed chip state
        print(f"!! fc01 read answered {r.hex()} instead of {FC01_OK:02x}")
        if self.in_recovery:
            raise Fatal("unexpected register value during recovery")
        print("   re-running the init to get back to a known state")
        self.setup()
        return False

    def stuck(self, after, read_queued):
        if self.in_recovery:
            raise Fatal(f"parser stuck again during recovery (after {after})")
        self.recover(after, read_queued)
        return False

    # --- recovery (protocol-t2.md, "Detecting an interrupted framebuffer payload")
    def recover(self, after, read_queued):
        print("   recovery: zero padding on EP2 until the queued fc01 read is answered "
              f"(up to {RECOVERY_S} s; Ctrl-C is held)")
        self.log("RECOVERY", f"start, after {after}")
        self.in_recovery = True
        try:
            self._pad(read_queued)
        except usb.core.USBError as e:
            raise Fatal(f"USB error during recovery: {e}")
        finally:
            self.in_recovery = False

    def _pad(self, read_queued):
        """Zero padding until the queued fc01 read is answered, then init."""
        if not read_queued:
            self._out(3, READ_FC01, 2000)
        deadline = time.monotonic() + RECOVERY_S
        sent, reply, n = 0, None, 0
        while reply is None and sent < PAD_MAX and time.monotonic() < deadline:
            n = PAD_FINE if sent < PAD_FINE_UNTIL else PAD_COARSE
            slow = False
            try:
                self.t.out(2, bytes(n), 2000)
            except usb.core.USBError as e:
                if not is_timeout(e):
                    raise Fatal(f"USB error during padding: {e}")
                slow = True     # the chip NAKs; the payload may have completed inside
            sent += n
            try:
                reply = self._read(2000 if slow else 20)
            except usb.core.USBError as e:
                if not is_timeout(e):
                    raise Fatal(f"EP81 read failed during padding: {e}")
        self.log("RECOVERY", f"{sent} B of zero padding sent")
        while reply is None and time.monotonic() < deadline:
            # no more padding: the firmware may be in its ~30 s internal timeout
            try:
                reply = self._read(1000)
            except usb.core.USBError as e:
                if not is_timeout(e):
                    raise Fatal(f"EP81 read failed: {e}")
        if reply is None:
            raise Fatal(f"no reply after {sent} B of padding and {RECOVERY_S} s")
        lo = max(sent - n + 1, 1)
        self.last_pad = sent
        msg = (f"parser answered ({reply.hex()}) after {sent} B of padding: the command "
               f"was short by about {lo}..{sent} B (chunk {n} B)")
        print("   recovery: " + msg)
        self.log("RECOVERY", msg)
        self.drain()
        self.setup()
        print("   recovery: device re-initialised, buffer A shown again")

    # --- normal init and the two backgrounds
    def setup(self):
        """Normal init (wacom-t2d/init.bin) with buffer A painted before the
        display enable, check registers, show A, paint B."""
        if self.dry:
            print("  init: wacom-t2d/init.bin (78 transfers on EP3/EP4) without the final 0x14")
            self.send(cmd_full("paint buffer A (dark blue) before the display enable", FB_A, BG_A))
            print("  init: 0x14 display enable")
            print("  EP3  verify: fc01 = 07, feb0 = 40, fcf6 = 01")
            self.send(cmd_flip(FB_A))
            self.send(cmd_full("paint buffer B (dark green)", FB_B, BG_B))
            return
        prev, self.in_recovery = self.in_recovery, True
        try:
            recs = t2dev.load_init()
            assert recs[-1][2] is not None and recs[-1][2][0] == 0x14, "init.bin must end with 0x14"
            for attempt in (1, 2):
                self.log("INIT", f"attempt {attempt}")
                try:
                    self.t.run_init(recs[:-1], smart=True)
                    self.send(cmd_full("paint buffer A (dark blue)", FB_A, BG_A))
                    self.t.run_init(recs[-1:], smart=True)
                except usb.core.USBError as e:
                    raise Fatal(f"init failed: {e}")
                ok, text = self.verify()
                print(f"  init attempt {attempt}: {text}{'' if ok else '  (not as expected)'}")
                if ok:
                    break
            else:
                raise Fatal("the init did not take (fc01/feb0/fcf6 wrong)")
            self.send(cmd_flip(FB_A))
            self.send(cmd_full("paint buffer B (dark green)", FB_B, BG_B))
        finally:
            self.in_recovery = prev


# ---------------------------------------------------------------------------
# running the steps

def run_step(p, key, title, risk, fn, opt, results):
    print(f"\n=== step {key}: {title}  [risk: {risk}]")
    if fn is None:
        print("not implemented: see captures/t2-research/blit-engine.md")
        p.log("STEP", f"{key} stub")
        results.append((key, "not implemented"))
        return
    cmds, expect = fn(opt)
    print("sends:")
    if p.dry:
        for c in cmds:
            p.send(c)
        print("  then:")
        p.send(cmd_flip(FB_B))
        print(f"  expect on the panel: {expect}")
        p.send(cmd_flip(FB_A))
        print("  expect: plain dark blue again")
        results.append((key, "dry run"))
        return
    for c in cmds:
        print_cmd(c, False, p.timeout_ms)
    print(f"  then flip to B ({hexs(flip_bytes(FB_B))}), look, flip back to A, "
          "fc01 check after each flip")
    print(f"expect: {expect}")
    if ask(f"run step {key}?", "ynq") == "n":
        p.log("STEP", f"{key} skipped")
        results.append((key, "skipped"))
        return
    p.log("STEP", f"{key} start: {title}")
    for c in cmds + [cmd_flip(FB_B)]:
        print(f"-> {c.name}")
        if not p.send(c):
            res = f"parser stuck after '{c.name}', recovered with {p.last_pad} B padding"
            p.log("RESULT", f"{key}: {res}")
            results.append((key, res))
            if ask("continue with the next step?") == "n":
                raise Quit()
            return
    seen = ask(f"do you see: {expect}?")
    note = ask_note()
    p.log("ANSWER", f"{key} seen={seen} note={note!r}")
    res = f"{'as expected' if seen == 'y' else 'NOT as expected'}{': ' + note if note else ''}"
    print("-> flip back to A")
    if p.send(cmd_flip(FB_A)):
        back = ask("plain dark blue screen again?")
        p.log("ANSWER", f"{key} back-to-A={back}")
        if back == "n":
            res += "; flip back to A not seen"
    else:
        res += "; parser stuck after the flip back, recovered"
    results.append((key, res))
    p.log("RESULT", f"{key}: {res}")


def self_test():
    """Packet builder checks against captured bytes; no USB."""
    # 0x13 builder against t2rle.frame_cmd (the old addr<<8 reading, same bytes)
    rgb = solid(W, H, BG_A)
    old = t2rle.frame_cmd(FB_B, rgb)
    new = cmd_full("x", FB_B, BG_A)
    assert old[:36] == new.header and old[36:] == new.payload, "0x13 header differs from t2rle"
    # 0x11 builders against the captured Windows clears
    assert hdr11(FB_A, W, H, W, H, 0, W * H * 3) == bytes.fromhex(
        "11 00c00000 0004 5802 0004 5802 0040 0040 00 00201c".replace(" ", ""))
    assert hdr11(0, 32, 1, 32, 1, 0, 96) == bytes.fromhex(
        "11 00000000 2000 0100 2000 0100 0040 0040 00 600000".replace(" ", ""))
    # the captured cursor-free buffer A header bytes (protocol-t2.md)
    h = hdr13(FB_A, FB_A, W, H, W, H, 0, W * H * 3, 0x1234)
    assert h[:10] == bytes.fromhex("130000c0000000c00000")
    assert h[18:27] == bytes.fromhex("004000400000003900") and h[33:] == bytes.fromhex("300528")
    # flips (protocol-t2.md: B scanout 00 38 07)
    assert flip_bytes(FB_B) == bytes.fromhex("1507fb6520fcb411fcb100fcb238fcb307fcb415fcb411")
    assert flip_bytes(FB_A)[4] == 0x10 and flip_bytes(FB_A)[10:17:3] == bytes((0x00, 0x30, 0x00))
    assert len(hdr11(0, 1, 1, 1, 1, 0, 3)) == 21 and len(h) == 36
    assert q214(64, 128) == 0x2000 and q214(32, 128) == 0x1000 and q214(5, 5) == 0x4000
    # geometry: test box in B, x multiple of 64
    a = fb_addr(FB_B, TEST_VX, BOX_VY, BOX_W, BOX_H)
    assert (a - FB_B) % STRIDE % 192 == 0
    # RLE round trips (t2rle.decode works on full 1024-px lines)
    for w, hh in ((BOX_W, BOX_H), (BOX_W // 2, BOX_H // 2)):
        dev = rot180(quads(w, hh))
        assert t2rle.decode(rle_rect(dev, w, hh), 1024, w * hh // 1024) == dev
    assert upscale2(quads(2, 2), 2, 2)[:6] == bytes(WHITE) * 2
    # RGB565
    assert to_565(bytes(RED + GREEN + BLUE + WHITE)) == bytes.fromhex("00f8e0071f00ffff")
    img = quads(BOX_W, BOX_H)
    assert from_565(to_565(img)) == img             # the test colours are exact in 565
    assert to_bgr(bytes(RED)) == bytes((0, 0, 0xFF))
    # YUY2 planes decode back
    yuyv, ref = yuv_bands(BOX_W, 48)
    assert len(yuyv) == BOX_W * 48 * 2 and len(ref) == BOX_W * 48 * 3
    pay, i, dec = rle_yuy2(yuyv), 0, bytearray()
    for u in range(len(yuyv) // 3072):
        y, i = rle_line_decode(pay, i, 1536)
        cu, i = rle_line_decode(pay, i, 768)
        cv, i = rle_line_decode(pay, i, 768)
        unit = bytearray(3072)
        unit[0::2], unit[1::4], unit[3::4] = y, cu, cv
        dec += unit
    assert i == len(pay) and bytes(dec) == yuyv
    # the step b frame: no 0x30 outside what the encoder maps, payload size
    g = rot180(gradient_frame())
    pay_b = rle_rect(g, W, H)
    assert t2rle.decode(pay_b, W, H) == g.replace(b"\x30", b"\x31")
    # every step builds, headers parse
    class O:
        scratch, stretch_src = SCRATCH, "target"
    for key, _, _, fn in STEPS:
        if fn:
            for c in fn(O)[0]:
                assert describe(c.header).startswith(f"0x{c.header[0]:02x}")
    print(f"self-test ok (step b payload {len(pay_b)} B, {len(pay_b) % 512} B in the last "
          f"512 B packet, {(len(pay_b) + CHUNK_BIG - 1) // CHUNK_BIG} transfers)")


def main():
    ap = argparse.ArgumentParser(description="Interactive probe for unverified T2 frame "
                                 "commands; read the docstring first.")
    ap.add_argument("--steps", default="all", help="e.g. a,c-e (default all: a-i)")
    ap.add_argument("--dry-run", action="store_true", help="print every packet, no USB access")
    ap.add_argument("--full-hex", action="store_true", help="dry run: print whole payloads")
    ap.add_argument("--self-test", action="store_true", help="check the packet builders and exit")
    ap.add_argument("--log", metavar="LABDIR", help="log to LABDIR/captures/ (default /tmp)")
    ap.add_argument("--timeout-ms", type=int, default=300, help="health check read timeout")
    ap.add_argument("--scratch", type=lambda s: int(s, 0), default=SCRATCH,
                    help=f"VRAM scratch address for step f (default {SCRATCH:#x})")
    ap.add_argument("--stretch-src", choices=("target", "scratch"), default="target",
                    help="0x13 source address for steps g/h: = target like the legacy "
                    "driver (default), or the scratch address (try after f worked)")
    opt = ap.parse_args()
    keys = parse_steps(opt.steps)

    if opt.self_test or opt.dry_run:
        self_test()
        if opt.self_test:
            return
    if opt.dry_run:
        p = Probe(None, Log(None), True, opt.full_hex, opt.timeout_ms)
        print("\n=== setup")
        p.setup()
        for key, title, risk, fn in STEPS:
            if key in keys:
                run_step(p, key, title, risk, fn, opt, [])
        return

    # refuse while a kernel driver owns the tablet
    tabs = find_tablets()
    if not tabs:
        sys.exit("no T2 tablet (0711:520d / 0711:5209) found")
    intf, pid, drv = tabs[0]
    unbind, bind = bind_cmds(intf, drv or "mct")
    if drv:
        sys.exit(f"{intf} (0711:{pid}) is bound to '{drv}'. Unbind it first:\n  {unbind}\n"
                 f"and bind it again afterwards:\n  {bind}")

    global usb, t2dev
    import usb.core
    import t2dev as _t2dev
    t2dev = _t2dev

    ts = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    path = (os.path.join(os.path.expanduser(opt.log), "captures", f"t2_probe-{ts}.log")
            if opt.log else f"/tmp/t2_probe-{ts}.log")
    log = Log(path)
    log("START", f"steps {''.join(keys)} intf {intf} pid {pid} timeout {opt.timeout_ms} ms "
                 f"scratch {opt.scratch:#x} stretch-src {opt.stretch_src}")
    print(f"log: {path}")

    results = []
    dev = None
    try:
        dev = t2dev.T2(wait=5)
        p = Probe(dev, log, False, False, opt.timeout_ms)
        print("checking the parser before the init")
        p.drain()
        if p.health("startup"):
            print("running the normal init")
            p.setup()
        if ask("panel shows a plain dark blue screen?") == "n":
            log("ANSWER", "baseline not seen")
            if ask("continue anyway?") == "n":
                raise Quit()
        for key, title, risk, fn in STEPS:
            if key in keys:
                run_step(p, key, title, risk, fn, opt, results)
    except Quit:
        log("QUIT")
        print("quit")
    except KeyboardInterrupt:
        log("QUIT", "ctrl-c")
        print("\ninterrupted")
    except RuntimeError as e:          # t2dev.T2 could not claim the interface
        log("FATAL", str(e))
        print(f"!! {e}: no access to the device node (run as root or add the uaccess "
              "rule), or something else has claimed it")
    except (Fatal, usb.core.USBError) as e:
        log("FATAL", str(e))
        print(f"\n!! {e}\n!! The chip needs a power cycle: unplug the tablet's USB cable, "
              "wait a few seconds, plug it back in.\n!! The mct driver binds on its own "
              f"after replugging; if it does not:\n  {bind}")
        results.append(("-", f"FATAL: {e}"))
    finally:
        if dev is not None:
            try:
                dev.close()
            except Exception:
                pass
        print("\nsummary:")
        for k, r in results:
            print(f"  {k}: {r}")
            log("SUMMARY", f"{k}: {r}")
        print(f"log: {path}\ngive the tablet back to the driver:\n  {bind}")


if __name__ == "__main__":
    main()
