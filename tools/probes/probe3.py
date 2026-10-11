# SPDX-License-Identifier: GPL-2.0-only
import usb.core, usb.util

dev = usb.core.find(idVendor=0x0711, idProduct=0x520d)
assert dev is not None
usb.util.claim_interface(dev, 0)
print("claimed")
try:
    def out(ep, hx, timeout=2000):
        return dev.write(ep, bytes.fromhex(hx), timeout)

    def read_all(ep, timeout=1500, max_chunks=16, chunk=512):
        data = b""
        for _ in range(max_chunks):
            try:
                d = bytes(dev.read(ep, chunk, timeout))
                data += d
                if len(d) < chunk:
                    break
            except Exception as e:
                print(f"  read stop: {repr(e)}")
                break
        return data

    print("== 0x91 ==")
    out(0x03, "918000ae00000100")  # guess: same shape as 0x1d per doc pattern? doc only says returns 512
    r = read_all(0x81)
    print(f"  got {len(r)} bytes, nonzero={sum(1 for b in r if b)}")
    if r:
        print("  head:", r[:64].hex())
        open("data/t2_91.bin", "wb").write(r)
except Exception as e:
    print("91 FAIL:", repr(e))

try:
    print("== 0x1b retry, drain up to 4K ==")
    out(0x03, "1b8000a000800000")
    r = read_all(0x81)
    print(f"  got {len(r)} bytes, nonzero={sum(1 for b in r if b)}")
    if r:
        print("  head:", r[:64].hex())
finally:
    usb.util.release_interface(dev, 0)
    print("released")
