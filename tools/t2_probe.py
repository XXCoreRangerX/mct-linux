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
  j  partial 2x upscale, src = target: are the neighbouring pixels untouched?
  k  partial 2x upscale from scratch VRAM: are the neighbouring pixels untouched?
  l  0x11 32-bit source (format 0x0e), 4 bytes per pixel B,G,R,X
  m  0x11 YUV 4:2:2 source (format 0x8a), YUY2 = Y0 U Y1 V
  n  0x11 YUV 12-bit source (format 0x96): the legacy driver's packing is
     per row, per 4 px Y0 U V Y1 Y2 Y3 (FUN_140038d44); --n-layout i420/nv12
     tries real 4:2:0 planes instead
  p  8-bit palette: EP3 0x0a table (256 x B,G,R) loaded, 0x11 format 0x00 at
     1 byte per pixel, table set back to 768 zeros (as the init sends) after
     the step, also when it fails or you quit
  q  0x0a table as hardware gamma (the step p test showed it acts on all
     output when non-zero): test picture (4 ramps, 4-colour box) in B, then
     with B shown q1 identity table, q2 red only (B 0, G 0, R i: tells the
     entry order), q3 gamma 2.2 on all channels, q4 the 768 zeros again
     (also sent when the step fails or you quit); a PASS/FAIL answer for each
  r  scaler completion signal: r1 reads 58 candidate registers at rest
     (fe70-72, fe57, fe36, fc59, fc34, fc50-5d, fe60-67, fe78-7b, fe80-89,
     fb60-6e, fc01 as control) 3 times; then four full-screen 0x13 into B
     (2x from 512x300 at 0x560000 like mct_t2's half mode, 1:1, 2x, 2x), each
     followed at once by a bounded tight loop of register reads (--r-window-ms,
     --r-max-reads) timed from the end of the payload transfer, the health
     check and one settled read of all 58; prints which registers/bits changed
     and when, and the read latencies (a read blocked while the chip is busy
     is a signal as well), then flips to B for a PASS/FAIL look

There is no step o (0x13 RLE of the 12-bit YUV format): the legacy RLE
builder only encodes 24/32-bit (mode 5) and YUY2 (mode 6) and returns
early for every other format, so there is no layout to copy.

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
uses for staging big frames). Step b overwrites all of buffer B. Step p
changes a table on EP 0x03 that nothing else touches after the init; the
normal init (also run by every recovery) loads the zeros again; step q
loads three more tables into it. Step r only adds register reads (EP3 0x05)
to commands of the kind the driver sends; a read gets no reply only when
the EP2 parser waits for payload bytes, which then goes to the recovery.

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
    if op == 0x0a and len(hdr) == 769:
        used = [(i, hdr[1 + 3 * i:4 + 3 * i]) for i in range(256) if any(hdr[1 + 3 * i:4 + 3 * i])]
        if not used:
            return "0x0a table (EP3): 768 zeros, as the init sends"
        if len(used) > 8:
            cols = [list(hdr[1 + k::3]) for k in range(3)]
            return ("0x0a table (EP3), columns as sent: byte 0 (B) " + curve_name(cols[0])
                    + ", byte 1 (G) " + curve_name(cols[1]) + ", byte 2 (R) "
                    + curve_name(cols[2]))
        return "0x0a table (EP3), entries B,G,R: " + "  ".join(
            f"[{i:#04x}] {e.hex(' ')}" for i, e in used) + "  (all others 0)"
    return f"opcode {op:#04x}, {len(hdr)} bytes"


# ---------------------------------------------------------------------------
# images: built in visible orientation as packed RGB24 bytes, then rotated

def solid(w, h, c):
    return bytes(c) * (w * h)


def quads(w, h, border=True):
    """Red top left, green top right, blue bottom left, yellow bottom right,
    1 px white border. Tells R/B swaps, mirroring and row shifts apart.
    The YUV steps leave the border out (subsampled chroma would smear it)."""
    img = bytearray(w * h * 3)
    for y in range(h):
        for x in range(w):
            if border and (x in (0, w - 1) or y in (0, h - 1)):
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


def to_bgrx(rgb):
    """32-bit source for format 0x0e: B, G, R, X per pixel (the legacy driver
    sends the GDI 32 bpp surface, BGRX in memory; its black-level code ORs
    bytes 0..2 of every 4). X = 0."""
    out = bytearray(len(rgb) // 3 * 4)
    out[0::4], out[1::4], out[2::4] = rgb[2::3], rgb[1::3], rgb[0::3]
    return bytes(out)


# RGB -> YUV exactly like the legacy driver's tables (FUN_140038af0, BT.601
# limited range, 8 fraction bits, 0x80 rounding): per channel value
#   Y = (66 R + 129 G + 25 B + 0x1080) >> 8
#   U = (0x8080 - 38 R - 74 G + 112 B) >> 8,  V = (0x8080 + 112 R - 94 G - 18 B) >> 8
# Chroma of 2 (4:2:2) or 4 (12-bit) pixels is summed and shifted by 9 / 10.

def _yuv_terms(rgb, i):
    r, g, b = rgb[i:i + 3]
    return ((66 * r + 129 * g + 25 * b + 0x1080) >> 8,
            0x8080 - 38 * r - 74 * g + 112 * b, 0x8080 + 112 * r - 94 * g - 18 * b)


def rgb_to_yuv(c):
    y, u, v = _yuv_terms(bytes(c), 0)
    return y, u >> 8, v >> 8


def to_yuy2(rgb, w):
    """Format 0x8a payload: per 2 px Y0 U Y1 V, chroma averaged over the pair
    (FUN_140039000, the converter for interface type 5 / format 0x20000)."""
    assert w % 2 == 0
    out = bytearray()
    for i in range(0, len(rgb), 6):
        y0, u0, v0 = _yuv_terms(rgb, i)
        y1, u1, v1 = _yuv_terms(rgb, i + 3)
        out += bytes((y0, (u0 + u1) >> 9 & 0xFF, y1, (v0 + v1) >> 9 & 0xFF))
    return bytes(out)


def to_y411(rgb, w):
    """Format 0x96 payload as the legacy driver builds it (FUN_140038d44,
    called by FUN_14002b798 with format 0x40000 and length w*h*6>>2): every
    row on its own, per 4 px Y0 U V Y1 Y2 Y3, chroma averaged over the 4 px.
    No vertical subsampling, so it is 4:1:1 packed, not planar 4:2:0."""
    assert w % 4 == 0
    out = bytearray()
    for i in range(0, len(rgb), 12):
        t = [_yuv_terms(rgb, i + 3 * k) for k in range(4)]
        out += bytes((t[0][0], sum(x[1] for x in t) >> 10 & 0xFF,
                      sum(x[2] for x in t) >> 10 & 0xFF, t[1][0], t[2][0], t[3][0]))
    return bytes(out)


def to_420(rgb, w, h, nv12):
    """Real 4:2:0 for --n-layout (not what the driver does): Y plane, then
    U and V planes (I420) or interleaved U,V (NV12), chroma averaged over 2x2."""
    assert w % 2 == 0 and h % 2 == 0
    ys, us, vs = bytearray(), bytearray(), bytearray()
    for y in range(h):
        for x in range(w):
            ys.append(_yuv_terms(rgb, (y * w + x) * 3)[0])
    for y in range(0, h, 2):
        for x in range(0, w, 2):
            t = [_yuv_terms(rgb, ((y + dy) * w + x + dx) * 3) for dy in (0, 1) for dx in (0, 1)]
            us.append(sum(a[1] for a in t) >> 10 & 0xFF)
            vs.append(sum(a[2] for a in t) >> 10 & 0xFF)
    if nv12:
        uv = bytearray(len(us) * 2)
        uv[0::2], uv[1::2] = us, vs
        return bytes(ys + uv)
    return bytes(ys + us + vs)


# step p palette: index -> colour, at spread-out indices so that a chip
# that ignores the table (index used as a grey level) shows greys
PALETTE = {0x20: WHITE, 0x40: RED, 0x80: GREEN, 0xC0: BLUE, 0xE0: YELLOW}


def lut_bytes(pal):
    """EP3 0x0a command: 0x0a + 256 entries of B, G, R (interleaved). Layout
    from the legacy miniport (t2exgrp64.sys FUN_00019030, 8 bpp branch): it
    turns the VIDEO_CLUT entries (R, G, B, 0) into table[3i..3i+2] =
    B, G, R and hands the 768 bytes to t2usb64.sys, which sends 0a + them
    unchanged (FUN_14003b1c8, length 0x301)."""
    t = bytearray(768)
    for i, (r, g, b) in pal.items():
        t[3 * i:3 * i + 3] = bytes((b, g, r))
    return b"\x0a" + bytes(t)


def to_index(rgb, pal):
    inv = {bytes(c): i for i, c in pal.items()}
    return bytes(inv[rgb[i:i + 3]] for i in range(0, len(rgb), 3))


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
    transfers (the firmware parses EP2 per transfer, protocol-t2.md).
    ep=3: one EP3 command without reply, sent as a single transfer."""

    def __init__(self, name, header, payload=b"", chunk=CHUNK_13, ep=2):
        self.name, self.header, self.payload, self.chunk = name, header, payload, chunk
        self.ep = ep
        self.watch = None       # step r: a Watch, register reads right after the payload

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


GREY = (0x80, 0x80, 0x80)
BAND_VY, BAND_H = BOX_VY - 32, BOX_H + 96   # full-width grey band around the box


def partial_half(opt, src_at_scratch):
    """A half-size source scaled 2x into the box, inside a plain grey band, to
    see whether the chip touches pixels next to the box (in-place decoding
    could write the packed source over the neighbouring columns)."""
    sw, sh = BOX_W // 2, BOX_H // 2
    src = quads(sw, sh)
    dst = fb_addr(FB_B, TEST_VX, BOX_VY, BOX_W, BOX_H)
    s = opt.scratch if src_at_scratch else dst
    pay = rle_rect(rot180(src), sw, sh)
    return ([cmd_full("clear buffer B (dark green)", FB_B, BG_B),
             cmd_rle("grey band across the screen", FB_B, 0, BAND_VY, W, BAND_H,
                     solid(W, BAND_H, GREY)),
             Cmd(f"test box: 0x13 {sw}x{sh} -> {BOX_W}x{BOX_H}, scale 0x2000, "
                 f"src {'scratch' if src_at_scratch else '= target'}",
                 hdr13(s, dst, sw, sh, BOX_W, BOX_H, 0x00, sw * sh * 3, len(pay)), pay)],
            "PASS = one box with the 4 colours inside a plain grey band, and the grey "
            "band is clean everywhere (look right of and below the box). FAIL = any "
            "coloured specks, lines or garbage in the grey band")


def step_j(opt):
    return partial_half(opt, False)


def step_k(opt):
    return partial_half(opt, True)


def cmd_raw11(name, w, h, fmt, pay):
    """0x11 test box: source size = target size, scale 0x4000."""
    a = fb_addr(FB_B, TEST_VX, BOX_VY, w, h)
    return Cmd(name, hdr11(a, w, h, w, h, fmt, len(pay)), pay, CHUNK_11)


PASS_SAME = f"PASS = both boxes identical ({QUADS_TEXT})"


def step_l(opt):
    img = quads(BOX_W, BOX_H)
    pay = to_bgrx(rot180(img))
    return (prep(img) + [cmd_raw11("test box: 0x11 32-bit B,G,R,X (fmt 0x0e)",
                                   BOX_W, BOX_H, 0x0e, pay)],
            f"{PASS_SAME}. FAIL = anything else: red and blue swapped, colours shifted "
            "or striped, box only partly drawn, or garbage")


YUV_TEXT = ("red top left, green top right, blue bottom left, yellow bottom right, "
            "no border")
YUV_PASS = (f"PASS = both boxes show the same 4 colours in the same places ({YUV_TEXT}); "
            "slightly different shades are normal for YUV. FAIL = colours swapped "
            "(e.g. red shown as blue or purple), green/pink tint, stripes, garbage, "
            "or the box only partly drawn")


def step_m(opt):
    img = quads(BOX_W, BOX_H, border=False)
    pay = to_yuy2(rot180(img), BOX_W)
    return (prep(img) + [cmd_raw11("test box: 0x11 YUV 4:2:2 YUY2 (fmt 0x8a)",
                                   BOX_W, BOX_H, 0x8a, pay)], YUV_PASS)


def step_n(opt):
    img = quads(BOX_W, BOX_H, border=False)
    dev = rot180(img)
    if opt.n_layout == "y411":
        pay, what = to_y411(dev, BOX_W), "driver packing Y0 U V Y1 Y2 Y3"
    else:
        pay, what = to_420(dev, BOX_W, BOX_H, opt.n_layout == "nv12"), opt.n_layout.upper()
    return (prep(img) + [cmd_raw11(f"test box: 0x11 YUV 12-bit (fmt 0x96), {what}",
                                   BOX_W, BOX_H, 0x96, pay)], YUV_PASS)


def step_p(opt):
    img = quads(BOX_W, BOX_H)
    pay = to_index(rot180(img), PALETTE)
    return ([Cmd("load the 0x0a table with the test palette (EP3)", lut_bytes(PALETTE), ep=3)]
            + prep(img)
            + [cmd_raw11("test box: 0x11 8-bit indices (fmt 0x00, 1 byte per pixel)",
                         BOX_W, BOX_H, 0x00, pay)],
            f"{PASS_SAME}, and the dark green around them looks as in the other steps. "
            "FAIL = the right box grey, black, red/blue swapped, only a third drawn or "
            "garbage. ALSO FAIL (and say so in the note): the background or the left box "
            "changed colour, which would mean the table acts on 24-bit output too",
            [Cmd("restore the 0x0a table: 768 zeros, as the init sends (EP3)",
                 lut_bytes({}), ep=3)])


# --- step q: the 0x0a table as a hardware gamma ramp

def lut_channels(r, g, b):
    """0x0a command from three 256-value curves: entry i = B[i], G[i], R[i]
    (the order lut_bytes uses; step q2 checks it)."""
    assert len(r) == len(g) == len(b) == 256
    t = bytearray(768)
    t[0::3], t[1::3], t[2::3] = bytes(b), bytes(g), bytes(r)
    return b"\x0a" + bytes(t)


IDENT = list(range(256))
ZEROS = [0] * 256
GAMMA22 = [round(255 * (i / 255) ** 2.2) for i in range(256)]

LUT_IDENT = lut_channels(IDENT, IDENT, IDENT)
LUT_RED = lut_channels(IDENT, ZEROS, ZEROS)
LUT_GAMMA = lut_channels(GAMMA22, GAMMA22, GAMMA22)
LUT_ZERO = lut_bytes({})


def curve_name(c):
    if c == IDENT:
        return "identity"
    if c == ZEROS:
        return "zeros"
    if c == GAMMA22:
        return "gamma 2.2"
    return "curve " + ",".join(f"{c[i]:02x}" for i in (0, 64, 128, 192, 255))


def ramp(w, h, chan):
    """Ramp dark (left) to bright (right), visible orientation; chan = (r, g, b) in 0/1."""
    row = b"".join(bytes(x * 255 // (w - 1) * k for k in chan) for x in range(w))
    return row * h


Q_RAMPS = [("grey", (1, 1, 1), 96, 64), ("red", (1, 0, 0), 160, 32),
           ("green", (0, 1, 0), 192, 32), ("blue", (0, 0, 1), 224, 32)]
Q_PIC = ("dark green background; 4 ramps across the whole width, dark on the left to "
         "bright on the right: grey (tall), red, green, blue; below them one box: "
         f"{QUADS_TEXT}")


def cmd_lut(name, lut):
    return Cmd(name, lut, ep=3)


class Check:
    """A sub-check run while buffer B is shown: send cmds, ask PASS/FAIL.
    restores = the cmds put the table back (the step's restore is then done)."""

    def __init__(self, sid, cmds, expect, restores=False):
        self.sid, self.cmds, self.expect, self.restores = sid, cmds, expect, restores


def step_q(opt):
    cmds = [cmd_full("clear buffer B (dark green)", FB_B, BG_B)]
    for name, chan, vy, h in Q_RAMPS:
        cmds.append(cmd_rle(f"{name} ramp across the screen", FB_B, 0, vy, W, h, ramp(W, h, chan)))
    cmds.append(cmd_rle("4-colour box (known-good 0x13)", FB_B, REF_VX, BOX_VY, BOX_W, BOX_H,
                        quads(BOX_W, BOX_H)))
    restore = [cmd_lut("restore the 0x0a table: 768 zeros, as the init sends (EP3)", LUT_ZERO)]
    checks = [
        Check("q1", [cmd_lut("q1: identity table, entry i = i,i,i (EP3)", LUT_IDENT)],
              "PASS = the picture looks exactly unchanged (same colours, same ramps). "
              "FAIL = anything changed; say what in the note (darker, inverted, black, "
              "colour cast, stripes)"),
        Check("q2", [cmd_lut("q2: red-only table, entry i = B 0, G 0, R i (EP3)", LUT_RED)],
              "PASS = everything only in shades of red: red quadrant, yellow quadrant and "
              "white border red, green and blue quadrants black, the grey and red ramps "
              "black-to-red, the green and blue ramps black, background almost black. "
              "FAIL = anything else. If it is all shades of BLUE instead, answer n and "
              "write 'blue' in the note (then the entries are R,G,B, not B,G,R); if the "
              "green or blue parts are red too, note that"),
        Check("q3", [cmd_lut("q3: gamma 2.2 on all channels, entry i = 255*(i/255)^2.2 (EP3)",
                             LUT_GAMMA)],
              "PASS = the picture is visibly darker in the midtones (the middle of the grey "
              "ramp much darker, the dark end crushed to black, the bright end still white), "
              "the colours keep their hue, no colour cast. FAIL = no change, a colour cast, "
              "or anything else"),
        Check("q4", restore,
              "PASS = the normal picture is back, exactly as at the start of the step "
              "(before q1). FAIL = it still looks changed", restores=True),
    ]
    return cmds, f"PASS = the test picture with the table at zeros: {Q_PIC}", restore, checks


# --- step r: does a register tell when the 0x13 2x stretch has finished?

HALF_SRC = 0x560000         # mct_t2's T2_HALF_SRC: past a worst-case staged full frame
R_HOT = [0xfe70, 0xfc59, 0xfc34, 0xfe57, 0xfe36, 0xfe71, 0xfe72, 0xfc01]   # fc01 = control
R_COLD = ([a for a in range(0xfc50, 0xfc5e) if a != 0xfc59] + list(range(0xfe60, 0xfe68))
          + list(range(0xfe78, 0xfe7c)) + list(range(0xfe80, 0xfe8a))
          + list(range(0xfb60, 0xfb6f)))
R_ALL = R_HOT + R_COLD
R_COLD_PER_PASS = 4         # each pass: the hot set + the next 4 of the others
R_AFTER_FIRST_MS = 20       # keep watching at least this long after the first reply
R_BASE_ROUNDS = 3
R_SRC_W, R_SRC_H = W // 2, H // 2


class Watch:
    """After the last payload transfer: read registers in a tight loop,
    bounded by a time window and a read count."""

    def __init__(self, label, window_ms, max_reads):
        self.label, self.window_ms, self.max_reads = label, window_ms, max_reads


class Sweep:
    """Register reads only (no EP2 command): the at-rest baseline."""
    ep = 3

    def __init__(self, name, regs, rounds):
        self.name, self.regs, self.rounds = name, regs, rounds


def watch_schedule():
    """Endless read order: every pass the hot set, then the next cold registers."""
    k = 0
    while True:
        yield from R_HOT
        for _ in range(R_COLD_PER_PASS):
            yield R_COLD[k % len(R_COLD)]
            k += 1


def r_cmds(opt, label, scaled):
    """Full screen into B: 512x300 at HALF_SRC scaled 2x, or the same picture at 1:1."""
    src = quads(R_SRC_W, R_SRC_H)
    w = Watch(label, opt.r_window_ms, opt.r_max_reads)
    if scaled:
        pay = rle_rect(rot180(src), R_SRC_W, R_SRC_H)
        assert RLD_BUF + len(pay) <= HALF_SRC, "staged stream would overlap the source"
        hdr = hdr13(HALF_SRC, FB_B, R_SRC_W, R_SRC_H, W, H, 0x00, R_SRC_W * R_SRC_H * 3, len(pay))
        what = f"0x13 {R_SRC_W}x{R_SRC_H} at {HALF_SRC:#x} -> {W}x{H} in B, scale 0x2000"
    else:
        pay = rle_rect(rot180(upscale2(src, R_SRC_W, R_SRC_H)), W, H)
        s = HALF_SRC if opt.r_plain_src == "scratch" else FB_B
        assert RLD_BUF + len(pay) <= HALF_SRC
        hdr = hdr13(s, FB_B, W, H, W, H, 0x00, W * H * 3, len(pay))
        what = (f"0x13 {W}x{H} 1:1 in B, src "
                f"{'scratch ' + hex(HALF_SRC) if s != FB_B else '= target (known-good path)'}")
    c = Cmd(f"{label}: {what}, then watch the registers", hdr, pay)
    c.watch = w
    return c


R_PASS = ("PASS = the whole screen shows 4 big quadrants: red top left, green top right, "
          "blue bottom left, yellow bottom right, with a thin white border; no stripes, "
          "garbage or small packed copy anywhere. FAIL = anything else")


def step_r(opt):
    # 1:1 between the 2x runs, so the picture in B at the end comes from a 2x run
    return ([Sweep("r1: baseline, candidate registers at rest", R_ALL, R_BASE_ROUNDS),
             r_cmds(opt, "2x #1", True), r_cmds(opt, "1:1", False),
             r_cmds(opt, "2x #2", True), r_cmds(opt, "2x #3", True)], R_PASS)


def seg_values(samples):
    """{addr: [(value, t_first_us, t_last_us), ...]} from (t0, t1, addr, value, n) samples."""
    out = {}
    for t0, t1, a, v, n in samples:
        segs = out.setdefault(a, [])
        if segs and segs[-1][0] == v:
            segs[-1][2] = t1
        else:
            segs.append([v, t1, t1])
    return out


def fmt_v(v):
    return "--" if v is None else f"{v:02x}"


def median(xs):
    xs = sorted(xs)
    return xs[len(xs) // 2] if xs else 0


def watch_report(run, baseline, base_lat):
    """Lines for one watched run: which registers/bits changed and when (ms after
    the end of the payload transfer), and the read latencies."""
    s = run["samples"]
    lines = [f"{run['label']}: payload {run['paylen']} B sent in {run['xfer_us'] / 1000:.2f} ms; "
             f"{len(s)} reads in {(s[-1][1] / 1000 if s else 0):.1f} ms"]
    if not s:
        return lines + ["  no reads"]
    lat = [t1 - t0 for t0, t1, *_ in s]
    slow = sorted(range(len(s)), key=lambda i: -lat[i])[:3]
    lines.append(f"  first reply {s[0][1] / 1000:.2f} ms after the payload (that read took "
                 f"{lat[0] / 1000:.2f} ms); read time median {median(lat) / 1000:.2f} ms"
                 + (f", at rest {base_lat / 1000:.2f} ms" if base_lat else ""))
    lines.append("  slowest reads: " + ", ".join(
        f"{lat[i] / 1000:.2f} ms ({s[i][2]:04x}, sent at {s[i][0] / 1000:.2f} ms)" for i in slow))
    if any(n != 1 for *_, n in s):
        lines.append("  !! some replies were not 1 byte: " + ", ".join(
            f"{a:04x}:{n} B" for _, _, a, _, n in s if n != 1))
    same, regs = 0, []
    segmap = seg_values(s)
    for a in sorted(segmap, key=lambda x: R_ALL.index(x) if x in R_ALL else len(R_ALL)):
        segs = segmap[a]
        rest = {v for v in baseline.get(a, []) if v is not None}
        vals = {v for v, *_ in segs if v is not None}
        if len(segs) == 1 and rest == vals:
            same += 1
            continue
        bits = 0
        for v in vals | rest:
            for w in vals | rest:
                bits |= v ^ w
        regs.append(f"    {a:04x}  " + " | ".join(
            f"{fmt_v(v)} {t0 / 1000:.2f}-{t1 / 1000:.2f} ms" for v, t0, t1 in segs)
            + f"   rest {','.join(fmt_v(v) for v in sorted(rest)) or '?'}"
            + f"   bits {bits:02x}" + ("  (varies at rest)" if len(rest) > 1 else ""))
    lines.append("  changed in the window or not as at rest:" if regs
                 else "  every register read as at rest")
    lines += regs
    lines.append(f"  unchanged and as at rest: {same} registers")
    st = run.get("settled")
    if st is not None:
        diff = [f"{a:04x}={fmt_v(v)}" for a, v in st.items() if v not in baseline.get(a, [v])]
        lines.append("  after the health check: " + (("not as at rest: " + " ".join(diff))
                                                     if diff else "all as at rest"))
    return lines


def last_change(run, a):
    segs = seg_values(run["samples"]).get(a, [])
    return f"{segs[-1][1] / 1000:.2f}" if len(segs) > 1 else "-"


def watch_summary(runs, baseline, base_lat):
    lines = []
    for run in runs:
        lines += watch_report(run, baseline, base_lat)
    moved = [a for a in R_ALL if any(last_change(r, a) != "-" for r in runs)]
    lines.append("last change per run (ms after the payload; "
                 + ", ".join(r["label"] for r in runs) + "):")
    for a in moved:
        lines.append(f"    {a:04x}: " + " / ".join(last_change(r, a) for r in runs))
    if not moved:
        lines.append("    no register changed during any window")
    lines.append("first reply per run (ms): " + " / ".join(
        f"{r['samples'][0][1] / 1000:.2f}" if r["samples"] else "-" for r in runs))
    return lines


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
    ("j", "partial 2x upscale, src = target: are the neighbours untouched?", "medium", step_j),
    ("k", "partial 2x upscale from scratch VRAM: are the neighbours untouched?", "medium",
     step_k),
    ("l", "0x11 32-bit BGRX source (format 0x0e)", "low-medium", step_l),
    ("m", "0x11 YUV 4:2:2 YUY2 source (format 0x8a)", "low-medium", step_m),
    ("n", "0x11 YUV 12-bit source (format 0x96)", "medium", step_n),
    ("p", "8-bit palette: 0x0a table + 0x11 at 1 byte per pixel; the table is set back to "
     "zeros afterwards", "medium", step_p),
    ("q", "0x0a table as hardware gamma: identity, red only, gamma 2.2; zeros afterwards",
     "medium", step_q),
    ("r", "scaler completion signal: register reads right after a full-screen 2x 0x13",
     "low-medium", step_r),
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
    print(f"  EP{cmd.ep}  {cmd.name}")
    if full or len(cmd.header) <= 64:
        print(f"       {'header' if cmd.ep == 2 else 'command'} {len(cmd.header)} B: "
              f"{hexs(cmd.header)}")
    else:
        print(f"       command {len(cmd.header)} B in one transfer, sha256 "
              f"{hashlib.sha256(cmd.header).hexdigest()[:16]}: {hexs(cmd.header[:16])} ...")
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
    if cmd.watch:
        w = cmd.watch
        print(f"  EP3  right after the last payload transfer: register reads (05 lo hi, "
              f"timeout {timeout_ms} ms each) in a tight loop for {w.window_ms} ms, at least "
              f"{R_AFTER_FIRST_MS} ms after the first reply, at most {w.max_reads} reads; "
              f"each pass {' '.join(f'{a:04x}' for a in R_HOT)} + the next "
              f"{R_COLD_PER_PASS} of the other {len(R_COLD)}")
    print(f"  EP3  health check: {hexs(READ_FC01)} -> expect {FC01_OK:02x} within {timeout_ms} ms")
    if cmd.watch:
        print(f"  EP3  then one read of each of the {len(R_ALL)} registers (settled values)")


def print_sweep(sw, timeout_ms):
    print(f"  EP3  {sw.name}")
    print(f"       {sw.rounds} rounds of {len(sw.regs)} register reads (05 lo hi, timeout "
          f"{timeout_ms} ms each), no EP2 command:")
    for k in range(0, len(sw.regs), 16):
        print("         " + " ".join(f"{a:04x}" for a in sw.regs[k:k + 16]))


def print_item(c, full, timeout_ms):
    if isinstance(c, Sweep):
        print_sweep(c, timeout_ms)
    else:
        print_cmd(c, full, timeout_ms)


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
        self.baseline = {}              # step r: addr -> values read at rest
        self.base_lat = []              # step r: read times at rest (us)
        self.watch_runs = []            # step r: one dict per watched command

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
                t_start = time.perf_counter_ns()
                for i, tr in enumerate(cmd.transfers()):
                    if i == 1:
                        t_start = time.perf_counter_ns()    # payload start
                    self._out(cmd.ep, tr, 2000 if i == 0 else 30000)
                t_end = time.perf_counter_ns()
            except usb.core.USBError as e:
                if not is_timeout(e):
                    raise Fatal(f"USB error while sending {cmd.name}: {e}")
                self.log(f"EP{cmd.ep} TIMEOUT", cmd.name)
                print(f"!! EP{cmd.ep} write timed out during '{cmd.name}'")
                return self.stuck(cmd.name, read_queued=False)
            if cmd.watch and not self._watch(cmd, t_start, t_end):
                return False
            ok = self.health(cmd.name)
            if ok and cmd.watch:
                ok = self._settled(cmd)
            return ok

    # --- step r: register reads
    def _watch(self, cmd, t_start, t_end):
        """Tight loop of register reads right after the payload; no file logging
        inside the loop (the log flushes every line), the samples go to the log
        afterwards. Each sample: (request us, reply us, addr, value, reply bytes),
        times relative to the end of the last payload transfer."""
        w = cmd.watch
        sched = watch_schedule()
        samples = []
        deadline = t_end + w.window_ms * 1_000_000
        run = {"label": w.label, "samples": samples, "paylen": len(cmd.payload),
               "xfer_us": (t_end - t_start) // 1000}
        self.watch_runs.append(run)
        try:
            while len(samples) < w.max_reads and time.perf_counter_ns() < deadline:
                a = next(sched)
                t0 = time.perf_counter_ns()
                r = self._reg_fast(a, "register watch")
                if r is None:
                    self.log("WATCH TIMEOUT", f"{w.label}: no reply to {a:04x} within "
                                              f"{self.timeout_ms} ms")
                    print(f"!! no reply to the {a:04x} read during '{w.label}': the parser "
                          "waits for more bytes")
                    return self.stuck(cmd.name, read_queued=True)
                t1 = time.perf_counter_ns()
                samples.append(((t0 - t_end) // 1000, (t1 - t_end) // 1000, a,
                                r[0] if r else None, len(r)))
                if len(samples) == 1:
                    deadline = max(deadline, t1 + R_AFTER_FIRST_MS * 1_000_000)
        finally:
            self.log("WATCH", f"{w.label}: payload {len(cmd.payload)} B in "
                              f"{run['xfer_us']} us, {len(samples)} reads "
                              "(request us, reply us, addr=value, reply bytes)")
            for k in range(0, len(samples), 16):
                self.log("WATCH", " ".join(
                    f"{t0},{t1},{a:04x}={fmt_v(v)}" + ("" if n == 1 else f",{n}B")
                    for t0, t1, a, v, n in samples[k:k + 16]))
        last = samples[-1][1] / 1000 if samples else 0
        print(f"       watch: {len(samples)} reads up to {last:.1f} ms after the payload, "
              f"first reply after {(samples[0][1] / 1000 if samples else 0):.2f} ms")
        return True

    def _read_regs(self, regs, what):
        """One read of each register -> {addr: value}, or None if the parser was
        stuck (then it has been recovered)."""
        got = {}
        for a in regs:
            t0 = time.perf_counter_ns()
            r = self._reg_fast(a, what)
            if r is None:
                self.log("SWEEP TIMEOUT", f"{what}: no reply to {a:04x}")
                print(f"!! no reply to the {a:04x} read ({what})")
                self.stuck(what, read_queued=True)
                return None
            if what == "baseline":
                self.base_lat.append((time.perf_counter_ns() - t0) // 1000)
            if len(r) != 1:
                self.log("SWEEP", f"{what}: {a:04x} answered {r.hex()}")
            got[a] = r[0] if r else None
        return got

    def _reg_fast(self, a, what):
        """EP3 05 lo hi and its reply, without logging (timing); None on timeout."""
        try:
            self.t.out(3, bytes((0x05, a & 0xFF, a >> 8)), 2000)
        except usb.core.USBError as e:
            raise Fatal(f"EP3 write failed ({what}): {e}")
        try:
            return bytes(self.t.dev.read(0x81, 512, self.timeout_ms))
        except usb.core.USBError as e:
            if not is_timeout(e):
                raise Fatal(f"EP81 read failed ({what}): {e}")
            return None

    def _settled(self, cmd):
        """After the health check: one read of every register (settled values)."""
        got = self._read_regs(R_ALL, f"settled after {cmd.watch.label}")
        if got is None:
            return False
        self.watch_runs[-1]["settled"] = got
        self.log("SETTLED", f"{cmd.watch.label}: "
                            + " ".join(f"{a:04x}={fmt_v(v)}" for a, v in got.items()))
        return True

    def sweep(self, sw):
        """Step r1: read the registers at rest, several rounds; print them."""
        if self.dry:
            print_sweep(sw, self.timeout_ms)
            return True
        self.log("SWEEP", sw.name)
        with HoldCtrlC():
            self.drain()
            for k in range(sw.rounds):
                got = self._read_regs(sw.regs, "baseline")
                if got is None:
                    return False
                for a, v in got.items():
                    self.baseline.setdefault(a, []).append(v)
                self.log("BASELINE", f"round {k + 1}: "
                                     + " ".join(f"{a:04x}={fmt_v(v)}" for a, v in got.items()))
        items = []
        for a in sw.regs:
            vals = self.baseline[a]
            items.append(f"{a:04x}={fmt_v(vals[0])}" if len(set(vals)) == 1
                         else f"{a:04x}={'/'.join(fmt_v(v) for v in vals)}(!)")
        print(f"       at rest ({sw.rounds} rounds, read time median "
              f"{median(self.base_lat) / 1000:.2f} ms; (!) = varies at rest):")
        for k in range(0, len(items), 8):
            print("         " + "  ".join(items[k:k + 8]))
        return True

    def do(self, c):
        return self.sweep(c) if isinstance(c, Sweep) else self.send(c)

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
    # a step returns (commands, expect), (commands, expect, restore commands) or
    # (commands, expect, restore commands, checks); the restore commands are sent
    # after the step however it ends, unless a check with restores=True sent them
    cmds, expect, *rest = fn(opt)
    after = rest[0] if rest else []
    checks = rest[1] if len(rest) > 1 else []
    p.baseline, p.base_lat, p.watch_runs = {}, [], []
    print("sends:")
    if p.dry:
        for c in cmds:
            p.do(c)
        print("  then:")
        p.send(cmd_flip(FB_B))
        print(f"  expect on the panel: {expect}")
        for ch in checks:
            print(f"  --- {ch.sid}, while B is shown:")
            for c in ch.cmds:
                p.send(c)
            print(f"  expect on the panel: {ch.expect}")
        p.send(cmd_flip(FB_A))
        print("  expect: plain dark blue again")
        if after:
            print(f"  then, if the step ended before {checks[-1].sid}:"
                  if checks and checks[-1].restores else "  then, however the step ended:")
            for c in after:
                p.send(c)
        results.append((key, "dry run"))
        return
    for c in cmds:
        print_item(c, False, p.timeout_ms)
    print(f"  then flip to B ({hexs(flip_bytes(FB_B))}), look"
          + (", then the checks below while B is shown" if checks else "")
          + ", flip back to A, fc01 check after each command")
    for ch in checks:
        print(f"  --- {ch.sid}:")
        for c in ch.cmds:
            print_cmd(c, False, p.timeout_ms)
        print(f"      expect: {ch.expect}")
    if after:
        print("  afterwards, also on failure or quit:")
    for c in after:
        print_cmd(c, False, p.timeout_ms)
    print(f"expect: {expect}")
    if ask(f"run step {key}?", "ynq") == "n":
        p.log("STEP", f"{key} skipped")
        results.append((key, "skipped"))
        return
    p.log("STEP", f"{key} start: {title}")
    restore = bool(after)
    done = {"restored": False}
    try:
        run_live(p, key, cmds, expect, results, checks, done)
    except Fatal:
        restore = False             # the chip does not answer, nothing to send
        raise
    finally:
        if restore and not done["restored"]:
            for c in after:
                print(f"-> {c.name}")
                if not p.send(c):
                    # the recovery ran the normal init, which restores it as well
                    p.log("RESTORE", f"{key}: parser stuck after '{c.name}', recovered")


def ask_pass(expect):
    if expect.startswith("PASS"):
        return ask(f"{expect}.\nPASS (y) or FAIL (n)?")
    return ask(f"do you see: {expect}?")


def stuck_result(p, key, c, results):
    """A command needed the recovery (which re-ran the init and shows A again);
    the step's restore commands are still sent afterwards."""
    res = f"parser stuck after '{c.name}', recovered with {p.last_pad} B padding"
    p.log("RESULT", f"{key}: {res}")
    results.append((key, res))
    if ask("continue with the next step?") == "n":
        raise Quit()


def run_live(p, key, cmds, expect, results, checks=(), done=None):
    done = {} if done is None else done
    for c in cmds:
        print(f"-> {c.name}")
        if not p.do(c):
            return stuck_result(p, key, c, results)
    if p.watch_runs:
        lines = watch_summary(p.watch_runs, p.baseline, median(p.base_lat))
        print("register watch (times in ms after the end of the payload transfer):")
        for ln in lines:
            print("  " + ln)
            p.log("WATCH SUMMARY", ln)
    c = cmd_flip(FB_B)
    print(f"-> {c.name}")
    if not p.send(c):
        return stuck_result(p, key, c, results)
    seen = ask_pass(expect)
    note = ask_note()
    p.log("ANSWER", f"{key} seen={seen} note={note!r}")
    res = f"{'as expected' if seen == 'y' else 'NOT as expected'}{': ' + note if note else ''}"
    for ch in checks:
        for c in ch.cmds:
            print(f"-> {c.name}")
            if not p.send(c):
                results.append((key, res))
                return stuck_result(p, ch.sid, c, results)
        if ch.restores:
            done["restored"] = True
        seen = ask_pass(ch.expect)
        note = ask_note()
        p.log("ANSWER", f"{ch.sid} seen={seen} note={note!r}")
        r = f"{'PASS' if seen == 'y' else 'FAIL'}{': ' + note if note else ''}"
        results.append((ch.sid, r))
        p.log("RESULT", f"{ch.sid}: {r}")
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
    # 32-bit, YUV and palette builders (steps l-p)
    assert to_bgrx(bytes(RED + BLUE)) == bytes.fromhex("0000ff00ff000000")
    assert rgb_to_yuv(RED) == (82, 90, 240) and rgb_to_yuv(WHITE) == (235, 128, 128)
    assert rgb_to_yuv((0, 0, 0)) == (16, 128, 128) and rgb_to_yuv(BLUE) == (41, 240, 110)
    assert rgb_to_yuv(GREEN) == (144, 54, 34) and rgb_to_yuv(YELLOW) == (210, 16, 146)
    assert to_yuy2(bytes(RED) * 2, 2) == bytes((82, 90, 82, 240))
    assert to_yuy2(bytes(WHITE + (0, 0, 0)), 2) == bytes((235, 128, 16, 128))
    assert to_y411(bytes(RED) * 4, 4) == bytes((82, 90, 240, 82, 82, 82))
    assert to_y411(bytes(BLUE) * 4 + bytes(YELLOW) * 4, 8) == bytes(
        (41, 240, 110, 41, 41, 41, 210, 16, 146, 210, 210, 210))
    img = rot180(quads(BOX_W, BOX_H, border=False))
    assert len(to_yuy2(img, BOX_W)) == BOX_W * BOX_H * 2
    assert len(to_y411(img, BOX_W)) == BOX_W * BOX_H * 6 >> 2        # FUN_14002b798
    for nv in (False, True):
        assert len(to_420(img, BOX_W, BOX_H, nv)) == BOX_W * BOX_H * 6 >> 2
    q = quads(4, 4, border=False)                   # 2x2 blocks: red, green / blue, yellow
    assert to_420(q, 4, 4, False) == bytes((82, 82, 144, 144) * 2 + (41, 41, 210, 210) * 2
                                           + (90, 54, 240, 16) + (240, 34, 110, 146))
    assert to_420(q, 4, 4, True)[16:] == bytes((90, 240, 54, 34, 240, 110, 16, 146))
    lut = lut_bytes(PALETTE)
    assert len(lut) == 769 and lut[0] == 0x0a and lut[1 + 3 * 0x40:4 + 3 * 0x40] == b"\0\0\xff"
    assert lut[1 + 3 * 0xC0:4 + 3 * 0xC0] == b"\xff\0\0" and sum(lut[1:]) == 255 * 8
    assert lut_bytes({}) == b"\x0a" + bytes(768)                     # init.bin record 13
    assert set(to_index(quads(BOX_W, BOX_H), PALETTE)) == set(PALETTE)
    # step q tables: 769 B, entry i = B,G,R; red only in byte 2 (the B,G,R order
    # step p's lut_bytes uses: RED = 00 00 ff)
    for t in (LUT_IDENT, LUT_RED, LUT_GAMMA, LUT_ZERO):
        assert len(t) == 769 and t[0] == 0x0a
    assert all(LUT_IDENT[1 + 3 * i:4 + 3 * i] == bytes((i, i, i)) for i in range(256))
    assert all(LUT_RED[1 + 3 * i:4 + 3 * i] == bytes((0, 0, i)) for i in range(256))
    assert LUT_RED == lut_bytes({i: (i, 0, 0) for i in range(256)})
    assert lut_bytes(PALETTE)[1 + 3 * 0x40:4 + 3 * 0x40] == LUT_RED[1 + 3 * 0xff:4 + 3 * 0xff]
    assert GAMMA22[0] == 0 and GAMMA22[255] == 255 and GAMMA22[128] == 56
    assert all(a <= b for a, b in zip(GAMMA22, GAMMA22[1:]))
    assert all(LUT_GAMMA[1 + 3 * i:4 + 3 * i] == bytes([GAMMA22[i]] * 3) for i in range(256))
    assert LUT_ZERO == b"\x0a" + bytes(768)
    assert describe(LUT_IDENT).endswith("byte 0 (B) identity, byte 1 (G) identity, "
                                        "byte 2 (R) identity")
    assert "byte 0 (B) zeros, byte 1 (G) zeros, byte 2 (R) identity" in describe(LUT_RED)
    assert describe(LUT_ZERO).endswith("768 zeros, as the init sends")
    r = ramp(W, 2, (1, 0, 0))
    assert len(r) == W * 2 * 3 and r[:3] == b"\0\0\0" and r[3 * (W - 1):3 * W] == b"\xff\0\0"
    # step r: register set, read order, the 2x header, the report
    assert len(R_ALL) == len(set(R_ALL)) == 58 and 0xfc01 in R_ALL and 0xfc59 in R_HOT
    sched = watch_schedule()
    first = [next(sched) for _ in range((len(R_HOT) + R_COLD_PER_PASS)
                                         * -(-len(R_COLD) // R_COLD_PER_PASS))]
    assert first[:len(R_HOT)] == R_HOT and set(first) == set(R_ALL)
    class O:
        scratch, stretch_src, n_layout = SCRATCH, "target", "y411"
        r_window_ms, r_max_reads, r_plain_src = 50, 400, "target"
    rc = step_r(O)[0]
    assert isinstance(rc[0], Sweep) and [c.watch.label for c in rc[1:]] == [
        "2x #1", "1:1", "2x #2", "2x #3"]
    h2 = rc[1].header
    assert h2 == hdr13(HALF_SRC, FB_B, 512, 300, W, H, 0, 512 * 300 * 3, len(rc[1].payload))
    assert h2[18:22] == bytes.fromhex("00200020") and RLD_BUF + len(rc[1].payload) <= HALF_SRC
    assert rc[2].header[2:10] == struct.pack("<II", FB_B, FB_B) and rc[2].header[18:22] == \
        bytes.fromhex("00400040")
    assert t2rle.decode(rc[2].payload, W, H) == rot180(upscale2(quads(512, 300), 512, 300))
    O.r_plain_src = "scratch"
    assert step_r(O)[0][2].header[2:6] == struct.pack("<I", HALF_SRC)
    O.r_plain_src = "target"
    smp = [(10, 300, 0xfe70, 4, 1), (300, 600, 0xfc59, 0x90, 1), (600, 9800, 0xfe70, 4, 1),
           (9800, 10100, 0xfe70, 0, 1), (10100, 10400, 0xfc01, 7, 1)]
    run = {"label": "2x #1", "samples": smp, "paylen": 3000, "xfer_us": 400,
           "settled": {0xfe70: 0, 0xfc59: 0x90}}
    base = {0xfe70: [0, 0, 0], 0xfc59: [0, 0, 0], 0xfc01: [7, 7, 7]}
    rep = "\n".join(watch_summary([run], base, 250))
    assert "fe70  04 0.30-9.80 ms | 00 10.10-10.10 ms   rest 00   bits 04" in rep, rep
    assert "fc59  90 0.60-0.60 ms   rest 00   bits 90" in rep and "fc59=90" in rep
    assert "fe70: 10.10" in rep and "unchanged and as at rest: 1 registers" in rep
    ret = step_q(O)
    assert [ch.sid for ch in ret[3]] == ["q1", "q2", "q3", "q4"] and ret[3][-1].restores
    assert [c.header for ch in ret[3] for c in ch.cmds] == [LUT_IDENT, LUT_RED, LUT_GAMMA,
                                                             LUT_ZERO]
    assert ret[2][0].header == LUT_ZERO and all(c.ep == 3 for ch in ret[3] for c in ch.cmds)
    # every step builds, headers parse, 0x11 length fields match the payload
    sizes = {"l": BOX_W * BOX_H * 4, "m": BOX_W * BOX_H * 2, "n": BOX_W * BOX_H * 3 // 2,
             "p": BOX_W * BOX_H}
    fmts = {"l": 0x0e, "m": 0x8a, "n": 0x96, "p": 0x00}
    for key, _, _, fn in STEPS:
        if fn:
            ret = fn(O)
            items = ret[0] + (ret[2] if len(ret) > 2 else [])
            items += [c for ch in (ret[3] if len(ret) > 3 else []) for c in ch.cmds]
            for c in items:
                if isinstance(c, Sweep):
                    continue
                if c.header[0] == 0x13:
                    assert int.from_bytes(c.header[30:33], "little") == len(c.payload)
                assert describe(c.header).startswith(f"0x{c.header[0]:02x}")
                if c.header[0] == 0x11:
                    assert int.from_bytes(c.header[18:21], "little") == len(c.payload)
            if key in sizes:
                t = ret[0][-1]
                assert t.header[0] == 0x11 and t.header[17] == fmts[key]
                assert len(t.payload) == sizes[key], key
                assert t.header[1:5] == fb_addr(FB_B, TEST_VX, BOX_VY, BOX_W, BOX_H).to_bytes(
                    4, "little")
    for lay in ("i420", "nv12"):
        O.n_layout = lay
        assert len(step_n(O)[0][-1].payload) == sizes["n"]
    ret = step_p(O)
    assert ret[0][0].ep == 3 and ret[0][0].header == lut and ret[2][0].header == lut_bytes({})
    print(f"self-test ok (step b payload {len(pay_b)} B, {len(pay_b) % 512} B in the last "
          f"512 B packet, {(len(pay_b) + CHUNK_BIG - 1) // CHUNK_BIG} transfers)")


def main():
    ap = argparse.ArgumentParser(description="Interactive probe for unverified T2 frame "
                                 "commands; read the docstring first.")
    ap.add_argument("--steps", default="all", help="e.g. a,c-e (default all: a-r, no o)")
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
    ap.add_argument("--r-window-ms", type=int, default=50,
                    help="step r: watch window after each payload, ms (default 50; at least "
                    f"{R_AFTER_FIRST_MS} ms after the first reply)")
    ap.add_argument("--r-max-reads", type=int, default=400,
                    help="step r: at most this many register reads per watch (default 400)")
    ap.add_argument("--r-plain-src", choices=("target", "scratch"), default="target",
                    help="step r's 1:1 comparison run: decode in place like every exact "
                    f"update (default), or from {HALF_SRC:#x} like the 2x runs (needs "
                    "1.8 MB of VRAM there, up to 0x722000, not used by anything so far)")
    ap.add_argument("--n-layout", choices=("y411", "i420", "nv12"), default="y411",
                    help="payload of step n (format 0x96): y411 = per row, per 4 px "
                    "Y0 U V Y1 Y2 Y3 as the legacy driver builds it (default); i420/nv12 = "
                    "planar 4:2:0, only to try if y411 fails")
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
                 f"scratch {opt.scratch:#x} stretch-src {opt.stretch_src} "
                 f"n-layout {opt.n_layout} r-window {opt.r_window_ms} ms "
                 f"r-max-reads {opt.r_max_reads} r-plain-src {opt.r_plain_src}")
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
