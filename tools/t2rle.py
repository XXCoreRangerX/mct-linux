#!/usr/bin/env python3
"""T2 (MCT Trigger II) planar RLE codec, per T2Indisp.dll EncodeRle @0x180013278.

Payload: for each row, three channel-lines R, G, B, each covering `w` px:
  byte != 0x30     -> one literal pixel of that value
  30 NN VV         -> NN+1 pixels of value VV
Pixel value 0x30 is unrepresentable as a literal (Windows maps it to 0x31).
Windows limits NN <= 0xFA and avoids NN == 0x30 (emits 30 2f VV VV instead).
"""


def decode(pay, w, h):
    """-> bytes RGB24 (w*h*3). Raises on malformed stream."""
    out = bytearray(w * h * 3)
    i = 0
    for y in range(h):
        for c in range(3):
            x = 0
            while x < w:
                b = pay[i]
                if b == 0x30:
                    n, v = pay[i + 1] + 1, pay[i + 2]; i += 3
                else:
                    n, v = 1, b; i += 1
                if x + n > w:
                    raise ValueError(f"run overflows line y={y} c={c} x={x} n={n} @{i}")
                base = (y * w + x) * 3 + c
                out[base:base + 3 * n:3] = bytes([v]) * n
                x += n
    if i != len(pay):
        raise ValueError(f"trailing {len(pay) - i} bytes")
    return bytes(out)


def _encode_line(ln, out, tol=0):
    """Encode one channel line. tol > 0 merges values within +-tol of the run
    value into the run (lossy, like the vendor encoder uses with 4)."""
    n = len(ln); x = 0
    while x < n:
        v = ln[x]
        if v == 0x30: v = 0x31
        r = 1
        while x + r < n and r < 0xFB and abs((ln[x + r] if ln[x + r] != 0x30 else 0x31) - v) <= tol:
            r += 1
        if r >= 3:
            if r - 1 == 0x30:           # avoid count byte 0x30 (Windows does too)
                out += bytes((0x30, 0x2F, v, v))
            else:
                out += bytes((0x30, r - 1, v))
        else:
            out += bytes(b if b != 0x30 else 0x31 for b in ln[x:x + r])
        x += r


def encode(rgb, w, h, tol=0):
    """RGB24 bytes (w*h*3) -> RLE payload."""
    out = bytearray()
    stride = w * 3
    for y in range(h):
        row = rgb[y * stride:(y + 1) * stride]
        for c in range(3):
            _encode_line(row[c::3], out, tol)
    return bytes(out)


def frame_cmd(addr, rgb, w=1024, h=600):
    """Full 0x13 command (36 B header + payload) for byte address `addr` (buffer start)."""
    pay = encode(rgb, w, h)
    a = addr << 8
    hdr = (b"\x13" + a.to_bytes(4, "little") * 2 + b"\x00"
           + w.to_bytes(2, "little") + h.to_bytes(2, "little")
           + w.to_bytes(2, "little") + h.to_bytes(2, "little")
           + bytes.fromhex("004000400000003900")
           + (w * h * 3).to_bytes(3, "little") + len(pay).to_bytes(3, "little")
           + bytes.fromhex("300528"))
    assert len(hdr) == 36
    return hdr + pay
