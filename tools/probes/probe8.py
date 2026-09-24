import usb.core, usb.util

dev = usb.core.find(idVendor=0x0711, idProduct=0x520d)
assert dev is not None
usb.util.claim_interface(dev, 0)
print("claimed")
try:
    def w(ep, hx, timeout=2000):
        n = dev.write(ep, bytes.fromhex(hx), timeout)
        print(f"  OUT {ep:#04x} {hx[:24]}{'...' if len(hx)>24 else ''} -> {n}B")
        return n

    def r(ep, n=512, timeout=1500):
        try:
            d = bytes(dev.read(ep, n, timeout))
            print(f"  IN {ep:#04x}: {len(d)}B {d[:32].hex()}")
            return d
        except Exception as e:
            print(f"  IN {ep:#04x}: {type(e).__name__}")
            return None

    def reg(addr):
        dev.write(0x03, bytes([0x05, addr & 0xFF, addr >> 8]), 2000)
        return bytes(dev.read(0x81, 64, 2000))[0]

    print("== baseline feb0/fc28 ==")
    print(f"  feb0={reg(0xfeb0):#04x} fc28={reg(0xfc28):#04x}")

    print("== 0x0a 768 zeros on 0x03 ==")
    dev.write(0x03, b"\x0a" + bytes(768), 3000)
    print("  sent")
    r(0x81, 512, 1000)

    print("== replay: feb0=0x43 ==")
    w(0x03, "020001feb043")
    print(f"  feb0 now={reg(0xfeb0):#04x}")

    print("== replay: fc28=0x01 ==")
    w(0x03, "020001fc2801")
    print(f"  fc28 now={reg(0xfc28):#04x}")
finally:
    usb.util.release_interface(dev, 0)
    print("released")
