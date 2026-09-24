import usb.core, usb.util, sys

dev = usb.core.find(idVendor=0x0711, idProduct=0x520d)
print("MCT:", dev)
if dev is None:
    sys.exit(1)
print("bus:", dev.bus, "addr:", dev.address)
try:
    print("manufacturer:", usb.util.get_string(dev, dev.iManufacturer))
    print("product:", usb.util.get_string(dev, dev.iProduct))
except Exception as e:
    print("string fail:", repr(e))

cfg = dev.get_active_configuration()
print("config:", cfg.bConfigurationValue, "interfaces:", cfg.bNumInterfaces)
for intf in cfg:
    print(f"IF {intf.bInterfaceNumber} class={intf.bInterfaceClass:02x} eps={intf.bNumEndpoints}")
    for ep in intf:
        d = "IN" if usb.util.endpoint_direction(ep.bEndpointAddress) == usb.util.ENDPOINT_IN else "OUT"
        print(f"  EP {ep.bEndpointAddress:#04x} {d} attr={ep.bmAttributes:#04x} maxpkt={ep.wMaxPacketSize}")

# Try to claim interface (needs write perm on /dev/bus/usb node)
try:
    if dev.is_kernel_driver_active(0):
        print("kernel driver active on if0")
    else:
        print("no kernel driver on if0")
except Exception as e:
    print("kernel_driver check fail:", repr(e))

try:
    usb.util.claim_interface(dev, 0)
    print("CLAIM OK")
    usb.util.release_interface(dev, 0)
    print("RELEASE OK")
except Exception as e:
    print("CLAIM FAIL:", repr(e))
