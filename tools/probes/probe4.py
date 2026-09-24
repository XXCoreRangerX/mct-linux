import usb.core, usb.util

dev = usb.core.find(idVendor=0x0711, idProduct=0x520d)
assert dev is not None
usb.util.claim_interface(dev, 0)
print("claimed")
try:
    for ep in (0x81, 0x85, 0x03, 0x02):
        try:
            dev.clear_halt(ep)
            print(f"clear_halt {ep:#04x} ok")
        except Exception as e:
            print(f"clear_halt {ep:#04x} fail: {repr(e)}")

    print("== 0x1d baseline ==")
    try:
        dev.write(0x03, bytes.fromhex("1d8000ae00000100"), 2000)
        r = bytes(dev.read(0x81, 512, 2000))
        print(f"got {len(r)} bytes, head: {r[:16].hex()}")
    except Exception as e:
        print("1d FAIL:", repr(e))

    print("== 0x1b baseline ==")
    try:
        dev.write(0x03, bytes.fromhex("1b8000a000800000"), 2000)
        r = bytes(dev.read(0x81, 512, 2000))
        print(f"got {len(r)} bytes, nonzero={sum(1 for b in r if b)}")
    except Exception as e:
        print("1b FAIL:", repr(e))
finally:
    usb.util.release_interface(dev, 0)
    print("released")
