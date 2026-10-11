#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Hardware cursor details on the T6, at 2560x1440@60 on a grey screen.
Each step changes the cursor and waits for y/n; summary at the end.

  1  64x64 red L (tip top left) at (300, 300), 512-byte uploads (as now)
  2  the same at x = -20 (sent as x - 1, the vendor encoding): the L's
     left 20 px cut off at the left edge
  3  the same at y = -20: top 20 px cut off at the top edge
  4  128x128 blue square with a white border, 4096-byte uploads
  5  96x96 green square with a white border, 4096-byte uploads

  t6_cursor_test.py 128   only: 128x128 blue square, upload cut at 0x10000
                          bytes (the last 2 pixels never sent), like the
                          16-bit offset allows

Run through tools/t6_run.sh.
"""
import os, sys, struct, time, select
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import t6dev

W, H = 2560, 1440
FB0 = 0x0


def mode():
    """the monitor's 2560x1440@60 timing (241.5 MHz), 40 MHz PLL base"""
    khz = 241500
    vco = khz * 4
    n = vco // 40000
    p = (vco - n * 40000) * 1000 // 40000
    return struct.pack("<IBB8HHHBBBBBB", khz, 60, 0, 2720, W, 2608, 32, 1481, H, 1443, 5,
                       p, 1000, n, 3, 1, 0, 0, 0)


def grey_frame(t):
    y = bytes([110]) * (W * H)
    uv = bytes([128]) * (W * H // 2)
    payload = y + uv
    t.seq += 1
    p0 = FB0 + 0x30
    hdr = struct.pack("<4I2H3I I3I", 3, len(payload), t.seq, 6, W, W, p0, p0 + W * H, p0, 6, 0, 0, 0)
    data = hdr + payload
    for off in range(0, len(data), t6dev.FRAG):
        frag = data[off:off + t6dev.FRAG]
        t.dev.write(2, struct.pack("<8I", 0, len(data), FB0, len(frag), off,
                                   1 if off + t6dev.FRAG < len(data) else 0, 0, 0), 5000)
        t.dev.write(2, frag, 5000)


def cursor_img(size, kind):
    c = np.zeros((size, size, 4), np.uint8)                   # B, G, R, A
    if kind == "L":
        c[0:48, 0:10] = (0, 0, 255, 255)
        c[38:48, 0:40] = (0, 0, 255, 255)
        c[0:6, 0:6] = (255, 255, 255, 255)
    else:
        col = (255, 0, 0, 255) if kind == "blue" else (0, 200, 0, 255)
        c[...] = (255, 255, 255, 255)
        c[4:-4, 4:-4] = col
    return struct.pack("<4H", 1, size, size, size * 4) + c.tobytes()


def upload(t, slot, img, chunk, limit=None):
    for off in range(0, min(len(img), limit or len(img)), chunk):
        t.cout(0x10, slot, off, img[off:off + chunk])
    t.cout(0x05, slot, 1)


def pos(t, x, y):
    t.cout(0x04, (x - 1) & 0xffff if x < 0 else x, (y - 1) & 0xffff if y < 0 else y)


def ask(q):
    while True:
        a = input(f"  {q} y/n + Enter: ").strip().lower()
        if a in ("y", "n"):
            return a


t = t6dev.T6()
res = []
if sys.argv[1:] == ["128"]:
    try:
        t.cout(0x12, t.out, 0, mode()); t.cout(0x31, 0, 0); t.output(True)
        time.sleep(1)
        grey_frame(t)
        print("mode 2560x1440@60 set; wait for the grey picture", flush=True)
        print("\nSTEP 1: 128x128 blue square, upload stops at 0x10000 bytes")
        upload(t, 1, cursor_img(128, "blue"), 4096, 0x10000); pos(t, 600, 400)
        a = ask("Blue square with a white border, about 128 px (twice the size of a normal pointer)?")
        print(f"\nsummary:\n  128 px cursor, cut upload     {'ok' if a == 'y' else 'BAD'}")
        t.cout(0x05, 1, 0)
    finally:
        t.close()
    sys.exit(0)
try:
    t.cout(0x12, t.out, 0, mode()); t.cout(0x31, 0, 0); t.output(True)
    time.sleep(1)
    grey_frame(t)
    print("mode 2560x1440@60 set; wait for the grey picture", flush=True)

    print("\nSTEP 1: red L, 64x64, at (300, 300)")
    upload(t, 0, cursor_img(64, "L"), 512); pos(t, 300, 300)
    res.append(("64 px cursor, 512 B uploads", ask("Red L with a white tip near the top left?")))

    print("\nSTEP 2: the same at x = -20")
    pos(t, -20, 300)
    res.append(("negative x", ask("L at the left edge with its left part cut off (not jumping elsewhere)?")))

    print("\nSTEP 3: the same at y = -20")
    pos(t, 300, -20)
    res.append(("negative y", ask("L at the top edge with its top part cut off?")))

    print("\nSTEP 4: 128x128 blue square, 4096 B uploads")
    t0 = time.time(); upload(t, 1, cursor_img(128, "blue"), 4096); dt = time.time() - t0
    pos(t, 600, 400)
    print(f"  upload took {dt * 1000:.0f} ms")
    res.append(("128 px cursor, 4096 B uploads", ask("Blue square with white border, about twice the L's size?")))

    print("\nSTEP 5: 96x96 green square, 4096 B uploads")
    upload(t, 2, cursor_img(96, "green"), 4096)
    res.append(("96 px cursor", ask("Green square, a bit smaller than the blue one, clean?")))

    t.cout(0x05, 2, 0)
    print("\nsummary:")
    for name, a in res:
        print(f"  {name:32} {'ok' if a == 'y' else 'BAD'}")
finally:
    t.close()
