mct-t2-linux
============

Linux support for USB displays built on the MCT Trigger II (T2) chip, such as
the Wacom DTU-1031X pen display (1024x600, USB 0711:520d). The vendor only
provides a Windows driver; the protocol was worked out from USB captures of
that driver, its disassembly, and testing on real hardware.

The pen (056a:032f) is a separate USB function handled by the in-kernel
wacom driver.

Contents
--------

  kernel/   mct_t2, a DRM/KMS driver. Works with Wayland/X11 and the text
            console, hardware cursor, DPMS. This is the one to use.
  t2d/      userspace daemon on top of EVDI. Older, handy for protocol work
            since it needs no kernel module reloads.
  nix/      flake packages and a NixOS module.
  tools/    Python tools used during reverse engineering (codec, init
            replay, capture parsing, live register experiments).
  docs/     protocol and research notes.
  captures/ USB capture of the Windows driver's init.
  vm/       Windows VM setup used for captures (vendor driver not included).

Building
--------

Kernel module, out of tree:

    make -C kernel KDIR=/lib/modules/$(uname -r)/build
    sudo insmod kernel/mct_t2.ko

The driver binds to the tablet automatically and shows up as a USB-n
connector. To put a text console on it, map a VT to its framebuffer, e.g.
`con2fbmap 2 1`.

NixOS, plain configuration.nix:

    imports = [ /path/to/mct-t2-linux/nix/nixos.nix ];
    hardware.mct-t2.enable = true;

or with flakes, add this repository as an input and use
`nixosModules.default` with the same option. `hardware.mct-t2.driver =
"evdi"` selects the EVDI + t2d setup instead of the kernel driver.

t2d needs EVDI; on Linux 7.2 and later EVDI requires nix/evdi-linux-7.2.patch.

Documentation
-------------

docs/protocol-t2.md is the protocol reference: transfer framing, frame
updates, the RLE codec, the hardware cursor and panel power. The other files
cover the init sequence, the hardware, the vendor driver, the capture setup,
and notes on both drivers.

Credits
-------

cyrozap's notes at https://github.com/cyrozap/mct-usb-display-adapter-re
(doc/Protocol-T2.md) were the starting point for the protocol documentation.
https://github.com/JingYenLoh/trigger6 is a separate Linux driver for the
Trigger 6.

Mainline Linux has no driver for MCT's Trigger display chips. MCT's USB vendor
ID shows up there only for the mct_u232 serial adapter, some SiS-based
sisusbvga adapters, an ax88179 Ethernet adapter and an audio quirk for the
Trigger 5.

License
-------

Code is GPL-2.0 (see LICENSE). The documentation in docs/ is CC BY-SA 4.0,
as it builds on cyrozap's notes. No vendor code or binaries are included.
