mct-t2-linux
============

Linux support for USB displays built on MCT (Magic Control Technology)
Trigger chips:

- Trigger II (T2): the Wacom DTU-1031X pen display (1024x600, USB
  0711:520d / 0711:5209). The pen (056a:032f) is a separate USB function
  handled by the in-kernel wacom driver. This backend is written for the
  DTU-1031X: its fixed panel and 180 degree scanout, its init sequence
  (taken from Wacom's driver) and its brightness control through the pen
  function. Other T2 adapters are untested and would need changes.
- Trigger 6 (T6): USB 3 to HDMI adapters such as the "T6 USB Station"
  (0711:5600; 0711:5601 likely works too), up to 1080p at 144 Hz.

The vendor drivers are for Windows and macOS (and a closed userspace Linux
daemon for the T6); the protocols were worked out from USB captures, the
vendor drivers' disassembly and testing on real hardware.

Contents
--------

  kernel/   mct, a DRM/KMS driver with a backend per chip (mct_t2.c,
            mct_t6.c). Works with Wayland/X11 and the text console,
            hardware cursor, DPMS. This is the one to use.
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
    sudo insmod kernel/mct.ko

`MCT_T2=n` or `MCT_T6=n` on the make command line leaves a backend out.
The driver binds to supported devices automatically; the tablet shows up as
a USB-n connector, a T6 as HDMI-A-n. To put a text console on it, map a VT to its framebuffer, e.g.
`con2fbmap 2 1`.

NixOS, plain configuration.nix:

    imports = [ /path/to/mct-t2-linux/nix/nixos.nix ];
    hardware.mct-t2.enable = true;

or with flakes, add this repository as an input and use
`nixosModules.default` with the same option. `hardware.mct-t2.driver =
"evdi"` selects the EVDI + t2d setup instead of the kernel driver.

`hardware.mct-t2.consoleMap = "01";` adds fbcon=map:01 to the kernel command
line, which puts every second VT (tty2, tty4, ...) on the tablet once its
framebuffer appears (assuming the GPU is fb0 and the tablet fb1).

`hardware.mct-t2.niriFix = true;` builds niri with two patches: it drops a
USB display's output properly when it is unplugged (niri 26.04 leaves a
stale output behind), and it renders display-only devices into their own
system-memory buffers instead of the GPU's VRAM, which the driver reads ~10x
faster.

`hardware.mct-t2.lossy = 4;` lets T2 updates too big for one 60 Hz frame go
lossy (heavy video, photo scrolling); the picture is refined to lossless once
motion stops. Off by default.

`hardware.mct-t2.format` picks the pixel format for T6 adapters: "auto"
(default: exact RGB, or YUV 4:4:4 where RGB exceeds the adapter's memory
bandwidth, while the screen is still, NV12 during large changes), or fixed
"rgb24", "rgb32", "yuv444", "nv12". Both settings can be changed at runtime
in /sys/module/mct/parameters/.

Brightness is exposed as /sys/class/backlight/mct_t2-N (0 = off, 1-9), e.g.
`brightnessctl -d 'mct_t2-*' set 3`.

t2d needs EVDI. EVDI 1.14.x does not build on Linux 7.2 without
nix/evdi-linux-7.2.patch; 1.15.1 and later build as is.

Documentation
-------------

docs/protocol-t2.md and docs/protocol-t6.md are the protocol references.
docs/t6-lab-notebook.md is the running log of the T6 work and
docs/t6-comparison.md compares the drivers with each other and with the
vendor's. The other files cover the T2's init sequence, the hardware, the
vendor driver, the capture setup and notes on the drivers.

Credits
-------

cyrozap's notes at https://github.com/cyrozap/mct-usb-display-adapter-re
(doc/Protocol-T2.md, Protocol-T6.md) were the starting point for the
protocol documentation. https://github.com/JingYenLoh/trigger6, a separate
Linux driver for the Trigger 6, and its Windows capture were the starting
point for the T6 work; y-chan's and ramifriedman's T6 forks and notes helped
with partial updates.

Mainline Linux has no driver for MCT's Trigger display chips. MCT's USB vendor
ID shows up there only for the mct_u232 serial adapter, some SiS-based
sisusbvga adapters, an ax88179 Ethernet adapter and an audio quirk for the
Trigger 5.

License
-------

Code is GPL-2.0 (see LICENSE). The documentation in docs/ is CC BY-SA 4.0,
as it builds on cyrozap's notes. No vendor code or binaries are included.
