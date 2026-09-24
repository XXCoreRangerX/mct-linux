import usb.core, usb.util

dev = usb.core.find(idVendor=0x0711, idProduct=0x520d)
assert dev is not None
usb.util.claim_interface(dev, 0)
print("claimed")
try:
    print("== poll EP 0x85 (500ms) ==")
    try:
        r = bytes(dev.read(0x85, 512, 500))
        print(f"got {len(r)} bytes: {r[:64].hex()}")
    except Exception as e:
        print("no data:", repr(e))

    print("== std descriptors (ctrl, read-only) ==")
    for req, val, name in [
        (0x0100, 0x0000, "device"),
        (0x0200, 0x0000, "config"),
        (0x0300, 0x0000, "string0/lang"),
        (0x0300, 0x0010, "string16/mfr?"),
        (0x0300, 0x0020, "string32/prod?"),
    ]:
        try:
            r = bytes(dev.ctrl_transfer(0x80, 0x06, val, 0, 256, 1000))
            print(f"{name}: {len(r)}B {r[:48].hex()}")
        except Exception as e:
            print(f"{name} FAIL: {repr(e)}")

    print("== MS OS descriptor probe (0xEE string) ==")
    try:
        r = bytes(dev.ctrl_transfer(0x80, 0x06, 0x03EE, 0, 256, 1000))
        print(f"0xEE: {len(r)}B {r.hex()}")
    except Exception as e:
        print("0xEE FAIL:", repr(e))
finally:
    usb.util.release_interface(dev, 0)
    print("released")
