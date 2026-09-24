#!/usr/bin/env python3
"""Show an RGB image on an already-initialised DTU-1031X (after replay.py).
Usage: show_image.py testpattern|<file.ppm|png via PIL> [--raw (no 180 rot)] [--flipv] [--buf A|B]"""
import sys, time
import os
import usb.core, usb.util
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import t2rle
from replay import flip_cmd

W, H = 1024, 600
BUF = {"A": 0x00C000, "B": 0x1CE000}


def test_pattern():
    """Top-left RED square, top-right GREEN, bottom-left BLUE, bottom-right WHITE,
    grey horizontal gradient band across the top, yellow arrow-ish bar pointing right."""
    img = bytearray(W * H * 3)
    def rect(x0, y0, x1, y1, c):
        for y in range(y0, y1):
            img[(y * W + x0) * 3:(y * W + x1) * 3] = bytes(c) * (x1 - x0)
    rect(0, 0, 200, 200, (255, 0, 0))
    rect(W - 200, 0, W, 200, (0, 255, 0))
    rect(0, H - 200, 200, H, (0, 0, 255))
    rect(W - 200, H - 200, W, H, (255, 255, 255))
    for x in range(W):                      # gradient band y 250..300: dark left -> bright right
        v = x * 255 // (W - 1)
        for y in range(250, 300):
            img[(y * W + x) * 3:(y * W + x) * 3 + 3] = bytes((v, v, v))
    rect(300, 400, 700, 440, (255, 255, 0))  # yellow bar
    for i in range(60):                       # arrow head at right end
        rect(700 + i, 420 - (60 - i) // 1 + 20 - 20, 701 + i, 420 + (60 - i), (255, 255, 0)) if False else None
        y0, y1 = 420 - (60 - i), 420 + (60 - i)
        rect(700 + i, max(y0, 0), 701 + i, y1, (255, 255, 0))
    return bytes(img)


def load(arg):
    if arg == "testpattern":
        return test_pattern()
    from PIL import Image
    im = Image.open(arg).convert("RGB").resize((W, H))
    return im.tobytes()


def rot180(rgb):
    """Panel scans out rotated 180 deg (Windows pre-rotates too): reverse pixel order."""
    px = [rgb[i:i + 3] for i in range(0, len(rgb), 3)]
    return b"".join(reversed(px))


def flipv(rgb):
    s = W * 3
    return b"".join(rgb[y * s:(y + 1) * s] for y in range(H - 1, -1, -1))


def send(dev, addr, rgb):
    fc = t2rle.frame_cmd(addr, rgb, W, H)
    dev.write(2, fc[:36], 5000)
    for k in range(36, len(fc), 19968):
        dev.write(2, fc[k:k + 19968], 30000)
    dev.write(2, flip_cmd(addr), 5000)
    return len(fc) - 36


if __name__ == "__main__":
    rgb = load(sys.argv[1] if len(sys.argv) > 1 else "testpattern")
    if "--raw" not in sys.argv:
        rgb = rot180(rgb)
    if "--flipv" in sys.argv:
        rgb = flipv(rgb)
    buf = sys.argv[sys.argv.index("--buf") + 1] if "--buf" in sys.argv else "B"
    dev = usb.core.find(idVendor=0x0711, idProduct=0x520d)
    usb.util.claim_interface(dev, 0)
    try:
        t = time.time(); n = send(dev, BUF[buf], rgb)
        print(f"sent {n} B RLE to buf {buf} in {time.time() - t:.2f}s")
    finally:
        usb.util.release_interface(dev, 0)
