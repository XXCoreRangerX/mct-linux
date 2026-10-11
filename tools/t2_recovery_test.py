#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Regression test for mct_t2's recovery from an interrupted frame payload.

Each cycle unbinds the display interface from mct_t2, optionally leaves the
T2 firmware halfway through a 0x13 frame (header plus part of its payload),
then binds the driver again and checks the kernel log and the DRM connector:
the driver must recover (or find the device idle), initialize and connect.

The interface is always rebound on exit, including on errors and Ctrl-C.
Needs write access to /sys/bus/usb/drivers/mct/{bind,unbind} and to the
device node (e.g. the NixOS module's uaccess rule plus a chmod of bind/unbind).

Usage: t2_recovery_test.py [--cycles N] [--clean] [--intf 6-3.1:1.0]
  --clean   don't interrupt a frame; only check plain unbind/bind cycles
The picture itself is not checked; look at the panel for that.
"""
import argparse, glob, os, random, subprocess, sys, time

import t2dev
import t2rle

DRV = "/sys/bus/usb/drivers/mct"


def bound(intf):
    return os.path.exists(os.path.join(DRV, intf))


def sysfs_write(name, intf):
    with open(os.path.join(DRV, name), "w") as f:
        f.write(intf)


def connector_status():
    st = [open(p).read().strip() for p in glob.glob("/sys/class/drm/card*-USB-*/status")]
    return st[0] if st else "absent"


def journal_cursor():
    out = subprocess.run(["journalctl", "-k", "-n", "0", "--show-cursor", "--no-pager"],
                         capture_output=True, text=True).stdout
    return out.strip().rsplit("-- cursor: ", 1)[-1]


def kernel_log_after(cursor):
    """Kernel messages after a journal cursor (immune to wall-clock jumps)."""
    out = subprocess.run(["journalctl", "-k", "--no-pager", "-o", "cat", "--after-cursor", cursor],
                         capture_output=True, text=True).stdout
    return [l for l in out.splitlines() if "mct_t2" in l]


def interrupt_frame(fraction):
    """Send a full-screen 0x13 header and only `fraction` of its payload."""
    t = t2dev.T2()
    try:
        rgb = os.urandom(1024 * 600 * 3)
        pay = t2rle.encode(rgb, 1024, 600)
        a = t2dev.FB_B << 8
        hdr = (b"\x13" + a.to_bytes(4, "little") * 2 + b"\x00"
               + bytes.fromhex("0004580200045802004000400000003900")
               + (1024 * 600 * 3).to_bytes(3, "little") + len(pay).to_bytes(3, "little")
               + bytes.fromhex("300528"))
        t.out(2, hdr)
        n = int(len(pay) * fraction) // 19968 * 19968
        for k in range(0, n, 19968):
            t.out(2, pay[k:k + 19968], 30000)
        return len(pay) - n
    finally:
        t.close()


def cycle(intf, clean):
    cursor = journal_cursor()
    sysfs_write("unbind", intf)
    time.sleep(2)
    left = 0
    frac = random.uniform(0.1, 0.9)
    if not clean:
        left = interrupt_frame(frac)
    sysfs_write("bind", intf)
    for _ in range(240):                    # init takes about 1 s; recovery 3 s, or ~30 s
        time.sleep(0.25)                    # when the chip waits out its own timeout
        if connector_status() == "connected":
            break
    time.sleep(1)
    log = kernel_log_after(cursor)
    return {
        "frac": round(frac, 3),
        "left": left,
        "bound": bound(intf),
        "connector": connector_status(),
        "recovered": any("recovered" in l or "completing it" in l for l in log),
        "initialized": any("Initialized mct_t2" in l for l in log),
        "errors": [l for l in log if "ERROR" in l or "failed" in l or "check failed" in l],
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cycles", type=int, default=5)
    ap.add_argument("--clean", action="store_true")
    ap.add_argument("--intf", default="6-3.1:1.0")
    args = ap.parse_args()
    if not bound(args.intf):
        sys.exit(f"{args.intf} is not bound to mct_t2; bind it first")

    ok = 0
    try:
        for i in range(args.cycles):
            r = cycle(args.intf, args.clean)
            good = (r["bound"] and r["connector"] == "connected" and r["initialized"]
                    and not r["errors"] and (args.clean or r["recovered"]))
            ok += good
            print(f"cycle {i + 1}: {'PASS' if good else 'FAIL'}  frac={r['frac']} left={r['left']}  "
                  f"recovered={r['recovered']} connector={r['connector']} errors={r['errors']}",
                  flush=True)
            if not good:
                break
    finally:
        if not bound(args.intf):
            try:
                sysfs_write("bind", args.intf)
            except OSError as e:
                print(f"could not rebind {args.intf}: {e}", file=sys.stderr)
    print(f"{ok}/{args.cycles} passed")
    sys.exit(0 if ok == args.cycles else 1)


if __name__ == "__main__":
    main()
