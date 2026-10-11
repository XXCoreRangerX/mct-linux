#!/usr/bin/env python3
"""Check whether command 0x0a (0a + 768 bytes) is a hardware gamma table.

The init sends 0x0a with 768 zeros, which would black out the panel if it were
a plain lookup table that is always on. This script unbinds mct_t2, runs the
init, shows a test pattern (grey ramp plus red, green and blue ramps) and then
sends a few tables, one per Enter press. Watch the panel and note what each
step does. The driver is bound again on exit, which re-runs the normal init.

Needs write access to /sys/bus/usb/drivers/mct/{bind,unbind} and to the
device node, same as t2_recovery_test.py.

Usage: gamma_test.py [--intf 6-3.1:1.0]
"""
import argparse, os, sys, time

import t2dev

DRV = "/sys/bus/usb/drivers/mct"
W, H = t2dev.W, t2dev.H

ident = bytes(range(256))
inv = bytes(255 - i for i in range(256))
zero = bytes(256)
full = bytes([255]) * 256

# (label, table, what to look for)
STEPS = [
    ("768 zeros, as the init sends", zero * 3,
     "no change expected (baseline)"),
    ("identity, planar R G B", ident * 3,
     "no change if it is a table"),
    ("inverted, planar", inv * 3,
     "negative picture if it is a planar table"),
    ("first 256 inverted, rest identity", inv + ident + ident,
     "one channel inverted: which colour ramp flips tells the plane order"),
    ("inverted, interleaved RGB triplets", bytes(b for i in range(256) for b in (255 - i,) * 3),
     "negative picture if the table is interleaved"),
    ("first 256 all 255, rest zero", full + zero + zero,
     "one channel saturated, others black if it is a table"),
    ("identity again", ident * 3,
     "back to normal"),
]


def pattern():
    img = bytearray(W * H * 3)
    band = H // 4
    for x in range(W):
        v = x * 255 // (W - 1)
        cols = [(v, v, v), (v, 0, 0), (0, v, 0), (0, 0, v)]
        for i, c in enumerate(cols):
            for y in range(i * band, (i + 1) * band):
                o = (y * W + x) * 3
                img[o:o + 3] = bytes(c)
    return bytes(img)


def sysfs_write(name, intf):
    with open(os.path.join(DRV, name), "w") as f:
        f.write(intf)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--intf", default="6-3.1:1.0")
    args = ap.parse_args()
    if not os.path.exists(os.path.join(DRV, args.intf)):
        sys.exit(f"{args.intf} is not bound to mct_t2; bind it first")

    sysfs_write("unbind", args.intf)
    time.sleep(2)
    t = None
    try:
        t = t2dev.T2()
        t.run_init(t2dev.load_init(), smart=True)
        t.show(pattern())
        print("pattern shown: rows from the top are grey, red, green, blue ramps "
              "(dark on the left)")
        for n, (label, table, expect) in enumerate(STEPS, 1):
            input(f"\nEnter for step {n}: {label} ({expect}) ")
            t.out(3, b"\x0a" + table)
            time.sleep(0.02)
            t.wreg(0xfc6f, 0x00)
            time.sleep(0.1)
            t.show(pattern())
            print(f"step {n} sent, feb0={t.reg(0xfeb0):02x}")
        input("\nEnter to finish and give the tablet back to mct_t2 ")
    finally:
        if t:
            t.close()
        sysfs_write("bind", args.intf)
        print("rebound")


if __name__ == "__main__":
    main()
