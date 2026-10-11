# SPDX-License-Identifier: GPL-2.0-only
import usb.core, usb.util

dev = usb.core.find(idVendor=0x0711, idProduct=0x520d)
assert dev is not None
usb.util.claim_interface(dev, 0)
print("claimed")
try:
    dev.clear_halt(0x81)
    dev.clear_halt(0x03)
    regs = {}
    for base in (0xFC00, 0xFE00):
        for lo in range(256):
            addr = base | lo
            try:
                dev.write(0x03, bytes([0x05, addr & 0xFF, (addr >> 8) & 0xFF]), 2000)
                r = bytes(dev.read(0x81, 64, 2000))
                regs[addr] = r[0] if r else None
            except Exception as e:
                regs[addr] = f"ERR {e!r}"
                dev.clear_halt(0x81)
                dev.clear_halt(0x03)
    nz = {a: v for a, v in regs.items() if isinstance(v, int) and v != 0}
    print(f"nonzero regs: {len(nz)} / {len(regs)}")
    for a in sorted(nz):
        print(f"  {a:#06x} = {nz[a]:#04x} ({nz[a]:3d})")
    errs = {a: v for a, v in regs.items() if isinstance(v, str)}
    print(f"errors: {len(errs)}")
    for a in sorted(errs)[:5]:
        print(f"  {a:#06x}: {errs[a]}")
    with open("data/t2_regsweep.txt", "w") as f:
        for a in sorted(regs):
            f.write(f"{a:#06x} {regs[a]}\n")
    print("saved data/t2_regsweep.txt")
finally:
    usb.util.release_interface(dev, 0)
    print("released")
