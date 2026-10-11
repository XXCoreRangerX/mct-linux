#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Init-trimming experiments. Record indices refer to wacom-t2d/init.bin (359 records)."""
import os, sys, time, t2dev

W, H = t2dev.W, t2dev.H
CLEARS = range(130, 358)       # two 0x11 black clears (hdr + 113 zero payloads each)
ENABLE = 358                   # 0x14 display enable (last record)


def solid(c):
    return bytes(c) * (W * H)


def variant(t, recs, keep, pace, color, first_frame_before_enable=False):
    sel = [recs[i] for i in keep]
    t0 = time.time()
    if first_frame_before_enable:
        t.run_init([r for r, i in zip(sel, keep) if i != ENABLE], pace_ctrl=pace, pace_fb=pace)
        t.show(solid(color), addr=t2dev.FB_A)      # 0x14 selects buffer A
        t.run_init([recs[ENABLE]], pace_ctrl=pace, pace_fb=pace)
    else:
        t.run_init(sel, pace_ctrl=pace, pace_fb=pace)
        t.show(solid(color))
    return time.time() - t0


if __name__ == "__main__":
    recs = t2dev.load_init()
    allidx = list(range(len(recs)))
    noclear = [i for i in allidx if i not in CLEARS]
    reads = [0, 2, 3, 6, 8, 21, 22, 55, 56, 66]      # 0x05 register reads (values unused)
    idrom = [1]                                     # 0x91 ID-ROM query
    probe_fill = [32, 33, 34]                       # 0x90 + 32x1 0x11 fill at address 0 + 0x90
    flip_regs = [i for i in range(74, 113) if i != 96]   # manual A/B scanout programming (0x15 does this)
    dup_block = list(range(113, 130))               # exact repeat of records 38..54
    DROP = set(CLEARS) | set(reads) | set(idrom) | set(probe_fill) | set(flip_regs) | set(dup_block)
    fb_bank = list(range(75, 91))                   # FB-bank address setup for buffers A and B: needed
    DROP -= set(fb_bank)
    minimal = [i for i in allidx if i not in DROP]
    t = t2dev.T2()
    plan = {
        "V1": (allidx, 0.005, (255, 0, 0), False),
        "V2": (allidx, 0.0, (0, 255, 0), False),
        "V3": (noclear, 0.0, (0, 0, 255), True),
        "V4": (minimal, 0.0, (255, 255, 0), True),     # yellow
        "FULL": (allidx, 0.02, (255, 255, 255), False),
    }
    if "--cold" in sys.argv:
        print("feb0=%02x feb1=%02x (cold = 00/f5)" % (t.reg(0xfeb0), t.reg(0xfeb1)), flush=True)
        time.sleep(8)                                   # 8051 boot after power-on
        img = t2dev.test_pattern()
        t0 = time.time()
        pace = float(os.environ.get("PACE", "0"))
        t.run_init([recs[i] for i in minimal if i != ENABLE], pace_ctrl=pace, pace_fb=min(pace, 0.005),
                   smart=os.environ.get("SMART") == "1")
        t.show(img, addr=t2dev.FB_A)
        t.run_init([recs[ENABLE]], pace_ctrl=0, pace_fb=0)
        print(f"cold minimal init ({len(minimal)} records) + first frame: {time.time() - t0:.2f}s")
        time.sleep(2)
        t.show(img, addr=t2dev.FB_B)                    # also exercise a flip to B
        print("flipped to B")
        t.close(); sys.exit()
    if "--flipregs" in sys.argv:
        tests = [("RED +75..90", list(range(75, 91)), (255, 0, 0)),
                 ("GREEN +75..82", list(range(75, 83)), (0, 255, 0)),
                 ("BLUE +74..95", list(range(74, 96)), (0, 0, 255))]
        for k, (name, add, col) in enumerate(tests):
            if k:
                variant(t, recs, allidx, 0.0, (255, 255, 255), False); time.sleep(4)
            keep = sorted(set(minimal) | set(add))
            variant(t, recs, keep, 0.0, col, True)
            print(time.strftime("%H:%M:%S"), name, len(keep), flush=True)
            time.sleep(10)
        t.close(); sys.exit()
    if "--drop1" in sys.argv:
        tests = [("RED -reads/idrom", reads + idrom, (255, 0, 0)), ("GREEN -probe_fill", probe_fill, (0, 255, 0)),
                 ("BLUE -flip_regs", flip_regs, (0, 0, 255)), ("MAGENTA -dup_block", dup_block, (255, 0, 255))]
        for k, (name, drop, col) in enumerate(tests):
            if k:
                variant(t, recs, allidx, 0.0, (255, 255, 255), False); time.sleep(4)
            keep = [i for i in noclear if i not in drop]
            variant(t, recs, keep, 0.0, col, True)
            print(time.strftime("%H:%M:%S"), name, len(keep), flush=True)
            time.sleep(10)
        t.close(); sys.exit()
    if "--order" in sys.argv:
        tests = [("RED minimal, frame after enable", minimal, (255, 0, 0), False),
                 ("GREEN noclear, frame before enable", noclear, (0, 255, 0), True),
                 ("BLUE minimal, frame before enable", minimal, (0, 0, 255), True)]
        for k, (name, keep, col, ffbe) in enumerate(tests):
            if k:
                variant(t, recs, allidx, 0.0, (255, 255, 255), False); time.sleep(4)
            variant(t, recs, keep, 0.0, col, ffbe)
            print(time.strftime("%H:%M:%S"), name, flush=True)
            time.sleep(10)
        t.close(); sys.exit()
    if "--bisect" in sys.argv:
        groups = [("RED control", [], (255, 0, 0)), ("GREEN +reads/idrom", reads + idrom, (0, 255, 0)),
                  ("BLUE +probe_fill", probe_fill, (0, 0, 255)), ("MAGENTA +flip_regs", flip_regs, (255, 0, 255)),
                  ("CYAN +dup_block", dup_block, (0, 255, 255))]
        for k, (name, add, col) in enumerate(groups):
            if k:
                variant(t, recs, allidx, 0.0, (255, 255, 255), False)   # reset: full init, white
                time.sleep(3)
            keep = sorted(set(minimal) | set(add))
            variant(t, recs, keep, 0.0, col, True)
            print(time.strftime("%H:%M:%S"), name, len(keep), flush=True)
            time.sleep(7)
        t.close(); sys.exit()
    if "--list" in sys.argv:
        print(len(minimal), "records kept:", minimal); sys.exit()
    for name in sys.argv[1:]:
        keep, pace, col, ffbe = plan[name]
        dt = variant(t, recs, keep, pace, col, ffbe)
        print(f"{name}: {len(keep)} records, pace {pace*1000:.0f} ms -> {dt:.2f}s, colour {col}", flush=True)
        time.sleep(6)
    t.close()
