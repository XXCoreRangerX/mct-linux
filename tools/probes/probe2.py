# SPDX-License-Identifier: GPL-2.0-only
import usb.core, usb.util

dev = usb.core.find(idVendor=0x0711, idProduct=0x520d)
assert dev is not None, "MCT device not found"

usb.util.claim_interface(dev, 0)
print("claimed")
try:
    def cmd(ep_out, data_hex, ep_in, read_len, timeout=3000):
        data = bytes.fromhex(data_hex)
        n = dev.write(ep_out, data, timeout)
        print(f"  OUT {ep_out:#04x} {data_hex}: wrote {n}")
        try:
            resp = bytes(dev.read(ep_in, read_len, timeout))
            print(f"  IN {ep_in:#04x}: got {len(resp)} bytes")
            return resp
        except Exception as e:
            print(f"  IN {ep_in:#04x} FAIL: {repr(e)}")
            return None

    print("== 0x1d device info ==")
    r = cmd(0x03, "1d8000ae00000100", 0x81, 512)
    if r:
        print("  hex[0:64]:", r[:64].hex())
        print("  hex[64:128]:", r[64:128].hex())
        # look for VID/PID ascii or le
        print("  contains 07115200:", "07115200" in r.hex())
        print("  ascii printable scan:", "".join(chr(b) if 32 <= b < 127 else "." for b in r[:128]))
        open("data/t2_1d_info.bin", "wb").write(r)
        print("  saved data/t2_1d_info.bin")

    print("== 0x1b EDID ==")
    r = cmd(0x03, "1b8000a000800000", 0x81, 512)
    if r:
        print("  hex[0:64]:", r[:64].hex())
        print("  edid magic (00ff..00):", r[:8].hex())
        open("data/t2_1b_edid.bin", "wb").write(r)
        print("  saved data/t2_1b_edid.bin")

    print("== 0x05 reg read feb0 ==")
    r = cmd(0x03, "05b0fe", 0x81, 64)
    if r:
        print("  hex:", r[:16].hex(), "len:", len(r))
finally:
    usb.util.release_interface(dev, 0)
    print("released")
