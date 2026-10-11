mct-linux
=========

A Linux DRM/KMS driver for USB displays built on MCT (Magic Control
Technology) Trigger chips:

- Trigger 6 (T6): USB 3 to HDMI adapters such as the j5create JUA254
  ("T6 USB Station", 0711:5600; 0711:5601 should work too). Any HDMI monitor, from its EDID:
  up to 1080p at 144 Hz, 2560x1440 and 3440x1440 at 60 Hz, 3840x2160 at
  30 Hz.
- Trigger II (T2): the Wacom DTU-1031X pen display (1024x600, USB
  0711:520d / 0711:5209). The pen (056a:032f) is a separate USB function
  handled by the in-kernel wacom driver. **The T2 backend has only been
  tested on the DTU-1031X** and is written for it: its fixed panel and 180
  degree scanout, its init sequence taken from Wacom's driver, its
  brightness control through the pen function. Trigger II products differ
  from each other far more than Trigger 5/6 adapters do (each has its own
  init sequence and panel setup), so other T2 devices will most likely not
  work without changes.

MCT's own drivers are for Windows and macOS, plus a userspace Linux driver
for the T6. The protocols here were worked out from USB captures, the vendor
drivers' disassembly and testing on real hardware.

Features
--------

Both chips:
- works with Wayland and X11 compositors and the text console
- hardware cursor, DPMS, suspend/resume, hotplug of the USB device
- commits never wait for USB: a worker sends what changed, the newest image
  wins, tear-free
- only rows that really changed are sent
- statistics in debugfs (mct_t2_stats, mct_t6_stats)

Trigger 6:
- modes from the monitor's EDID, plus extra refresh rates up to what the
  monitor and the adapter can do; reduced blanking or a lower rate when a
  monitor's own timing is out of the adapter's reach
- pixel formats: exact RGB or YUV 4:4:4 while the screen is still, NV12
  during large changes (auto, the default), or a fixed format
- monitor hotplug (debounced), recovery from USB transfer errors, USB 2
  links (modes up to 2048 wide); a fallback for monitors without a usable
  EDID (not yet tried on hardware)
- hardware cursor up to 128 px
- software gamma (the adapter has no gamma tables)

Trigger II (DTU-1031X):
- lossless picture by default, optional lossy mode for heavy motion that
  turns lossless again once motion stops
- backlight brightness, recovery from a device left mid-frame

Not supported: the T6's JPEG mode (it would need an encoder in the kernel;
see docs/roadmap.md), HDMI audio, models with two outputs, hardware rotation
(compositors rotate), the T6's YUYV format (doesn't scan out properly).

Contents
--------

  kernel/   the driver, module "mct": a shared DRM core (mct_drv.c) and a
            backend per chip (mct_t2.c, mct_t6.c)
  app/      MCT Display Manager (GTK 4 window and tray icon) and mctctl
            (command line): per-device settings, status, identification
  nix/      flake package and NixOS module, two optional niri patches
  docs/     protocol references, limits, lab notes (docs/README.md)
  tools/    test and research scripts (tools/README.md)
  data/, captures/, vm/
            first probe outputs, an encoded test frame, how the Windows VM
            for the captures was run (the captures themselves and the vendor
            files are not published)
  wacom-t2d/
            legacy: the DTU-1031X as a userspace daemon on top of EVDI, for
            kernels older than 7.2 and for protocol work; also holds the
            tablet's init sequence the kernel driver is built from. Written
            for this tablet only, like the init it replays

Supported devices
-----------------

  0711:5600  j5create JUA254, "T6 USB Station" (Trigger 6)   tested
  0711:5601  single-output Trigger 6 adapter               untested
  0711:520d  Wacom DTU-1031X (Trigger II), interface 0      tested
  0711:5209  the same tablet, which enumerates with either ID

Other Trigger 6 IDs (MCT uses 0711:5600-561f and more) are not matched yet:
they may have other outputs or features this driver doesn't handle. Other
Trigger II products need their own init and modes (see above).

Building
--------

Needs Linux 7.2 or later (DRM API) with DRM, DRM_KMS_HELPER,
DRM_GEM_SHMEM_HELPER, BACKLIGHT_CLASS_DEVICE and USB enabled, and the
kernel's headers or build tree.

    make -C kernel KDIR=/lib/modules/$(uname -r)/build
    sudo make -C kernel install
    sudo depmod -a
    sudo modprobe mct

modprobe also loads the DRM helper modules mct uses; a plain
`sudo insmod kernel/mct.ko` works for a quick test only once those are
loaded (otherwise it fails with "Unknown symbol"). To reload after a
rebuild, unbind the devices (`echo <interface> >
/sys/bus/usb/drivers/mct/unbind` for each entry there), then
`rmmod mct` and `modprobe mct`. With Secure Boot, the module has to be
signed with a key the kernel trusts.

`MCT_T2=n` or `MCT_T6=n` on the make command line leaves a backend out. The
driver binds to supported devices automatically. The T6 shows up as an
HDMI-A-n connector, the tablet as USB-n.

The scripts in tools/ open the devices directly; give your user access with
a udev rule such as

    SUBSYSTEM=="usb", ATTR{idVendor}=="0711", ATTR{idProduct}=="5209|520d|5600|5601", TAG+="uaccess"

(the NixOS module installs this one).

NixOS, plain configuration.nix:

    imports = [ /path/to/mct-linux/nix/nixos.nix ];
    hardware.mct.enable = true;

or with flakes, add this repository as an input and import
`nixosModules.default`. The module builds the driver for the system's own
kernel (`boot.kernelPackages`); the flake's `packages.x86_64-linux.mct-kmod`
is built for linuxPackages_latest. The module also installs `mct-reload`,
which unbinds the devices and loads the module again (after a rebuild,
without a reboot).

Options
-------

Every device has its own settings, in
/sys/bus/usb/devices/<interface>/mct/ (with read-only information next to
them; docs/companion-app.md lists all files):

  format    T6 pixel format: auto (default), rgb24, rgb32, yuv444, nv12.
            auto uses exact RGB or YUV 4:4:4 while the screen is still and
            NV12 during large changes. A fixed format stays fixed (no NV12
            for motion) where the adapter can scan it out at the current
            mode, NV12 elsewhere.
  all_modes T6: 1 also offers modes larger than the monitor's native
            resolution (off by default; monitors often list 4K and scale it
            down). The compositor re-probes the monitor on a change.
  lossy     T2: maximum lossy tolerance (1-8) for updates too big for one
            60 Hz frame, refined to lossless once motion stops. 0 = off
            (default).

    echo nv12 | sudo tee /sys/bus/usb/devices/2-2:1.0/mct/format

The module parameters of the same names (/sys/module/mct/parameters/,
`modprobe mct format=...`) are the values new devices start with. To change
settings without root, make the files writable for a group, as the NixOS
module does for group video:

    ACTION=="bind", SUBSYSTEM=="usb", DRIVER=="mct", RUN+="/bin/sh -c 'cd /sys%p/mct && for f in format all_modes lossy; do [ -e $f ] && chgrp video $f && chmod g+w $f; done'"

NixOS options (hardware.mct.*): `enable`, `format`, `allModes` and `lossy` as above,
`driver = "evdi"` for the EVDI + wacom-t2d route instead of the kernel driver,
`consoleMap` to put text consoles on the tablet (adds fbcon=map:..., e.g.
"01" for every second VT), `niriFix` to build niri with the two patches in
nix/ (drop a removed USB display's output; render display-only devices into
system memory, which makes reading frames many times faster).

Brightness of the DTU-1031X: /sys/class/backlight/mct_t2-N (0 = off, 1-9),
e.g. `brightnessctl -d 'mct_t2-*' set 3`.

Text console on a display: map a VT to its framebuffer, e.g. `con2fbmap 2 1`.

MCT Display Manager and mctctl
------------------------------

app/ holds a desktop app for the settings above and a command line tool,
both per device (NixOS: `hardware.mct.app.enable = true`, `app.tray = true`
for the tray icon at login; elsewhere `cargo build --release`
in app/, see app/README.md):

- `mct-manager`: one page per connected display with its mode, format,
  settings, live update and data rate, transfer errors, and everything
  that identifies it (product, chip and firmware, USB, driver, the
  monitor's EDID). Switches the DTU-1031X between the kernel driver and the
  legacy wacom-t2d daemon. Warns when a T6 runs over USB 2.
- `mct-manager --tray`: the same from a tray icon (StatusNotifierItem).
- `mctctl list [-v]`, `mctctl info [-v] [DEV]`, `mctctl get/set [DEV] KEY
  [VALUE]`, `mctctl stats -w`, `mctctl driver kernel|legacy`; `--json` for
  scripts.

Writing settings without root needs the udev rule that gives group video
write access (installed by the NixOS module; see docs/companion-app.md).

Debugging
---------

    sudo cat /sys/kernel/debug/dri/*/mct_t6_stats
    sudo cat /sys/kernel/debug/dri/*/mct_t2_stats

show the mode, the format in use, update rates, bytes sent, timings and
error counters (docs/debugfs.md explains the fields). The same basics,
without root, are in each device's mct/ directory in sysfs. The T6 logs its
feature level and video memory at probe.

Known issues
------------

- Some monitors hang their on-screen menu after the signal goes away and
  comes back, or on a timing they list but don't accept; a power cycle of
  the monitor brings them back. Over HDMI from the T6, a Gigabyte MO34WQC2
  showed nothing on its own 3440x1440@60 timing; the driver offers reduced
  blanking there instead.
- After a USB transfer error the T6 is reset with the vendor's cancel
  request; the adapter then reconnects, about 3 s without picture.
- On some starts the DTU-1031X's picture shows faint vertical lines, a
  sampling issue of its analog panel link; reloading the driver or a replug
  gives another try. If the lines never go away, or the picture is
  garbled, the tablet's T2-285A controller may be failing: these chips run
  hot, and the author of the iFixit disassembly guide found it damaged in
  all three of their units
  (https://www.ifixit.com/Guide/Wacom+DTU-1031X+Disassembly/166191).
- Compositors that hand over buffers in GPU memory make the driver read
  them over PCIe (a few ms per frame at 1080p); niri with the patch above
  doesn't.

Documentation
-------------

docs/README.md lists everything: protocol references (protocol-t6.md,
protocol-t2.md), the T6 limits and the measurements behind them, the
comparison with the vendor drivers, the roadmap and the lab notebooks.

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

Code (kernel/, app/, tools/, wacom-t2d/, nix/) is GPL-2.0-only, see LICENSE and the
SPDX line in each file; Copyright (C) 2026 Filip Piękoś
<mail@filippiekos.com>. The patches in nix/ follow the licence of the
project they change (niri: GPL-3.0-or-later, EVDI: GPL-2.0). The
documentation in docs/ is CC BY-SA 4.0 (docs/LICENSE.md), as it builds on
cyrozap's notes. No vendor code or
binaries are included. CHANGELOG.md lists the changes per version.
