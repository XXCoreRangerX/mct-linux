# Other Linux drivers for MCT Trigger 6 (2026-10-11)

> **Research snapshot** (2026-10-11): the other projects as of that day, compared with mct 0.5.0 (mct_t6 0.27.0). Not updated afterwards.

## The projects

| Project | What it is | Licence | State (2026-10-11) |
|---|---|---|---|
| [mcttrigger/triggerdm](https://github.com/mcttrigger/triggerdm) | MCT's own Linux driver: a userspace daemon on EVDI + libusb + libjpeg-turbo, built for ChromeOS developer mode | GPL-2.0-or-later (headers and LICENSE) | on GitHub since 2024-08, last change 2026-10-07 |
| MCT's Ubuntu package (`T6evdi`) | the same code base packaged for Ubuntu, from MCT's download page | no licence in the package; its headers say "All rights reserved" | the version read for [t6-vendor-review.md](t6-vendor-review.md) |
| [ramifriedman/mct-t6-linux](https://github.com/ramifriedman/mct-t6-linux) ("trigger6") | a kernel DRM driver plus a Python userspace driver and GTK tools, for the StarTech USBC2HD4 (two T6 chips, four HDMI ports, `0711:5601`) | kernel GPL-2.0-only, tools MIT | "beta"; last push 2026-04-08 |
| [rhgndf/trigger2](https://github.com/rhgndf/trigger2) | a GPL DRM driver for the Trigger II in j5create JUA230 dongles, by the author of the drm/trigger5 patch | GPL-2.0-only | added 2026-10-12: published 2026-09-25, last change 2026-10-08; timing registers, the PLL formula and the in-band `0x16` register list are now in protocol-t2.md |
| [JingYenLoh/trigger6](https://github.com/JingYenLoh/trigger6) and forks ([y-chan](https://github.com/y-chan/trigger6), [MatthieuLeboeuf](https://github.com/MatthieuLeboeuf/trigger6), [benjaminpotter](https://github.com/benjaminpotter/trigger6)) | the first open kernel driver for the T6, the starting point for mct's T6 work | GPL-2.0 | experimental |

triggerdm's README describes the ChromeOS setup; its code is close to the
Ubuntu daemon (`t6.h`, `t6bulkdef.h`, `t6auddef.h`, `evdi_t6.h` differ by
20 to 50 lines each). Since MCT publishes it under the GPL, the facts mct
took from the Ubuntu daemon are also in openly licensed MCT code.

## mct and ramifriedman's trigger6

Both are kernel DRM drivers with damage tracking, DKMS packaging and a
companion app. They aim at different hardware: trigger6 at a four-output
dock with two chips, mct at single-output adapters (and the Trigger II
tablet).

| | mct 0.5.0 | trigger6 (2026-04-08) |
|---|---|---|
| Tested hardware | j5create JUA254 (`0711:5600`, one HDMI); Wacom DTU-1031X (Trigger II) | StarTech USBC2HD4 (`0711:5601`, 2 chips x 2 HDMI) |
| Outputs per chip | one (the second output of dual-head chips is not driven) | **two**, and several adapters at once |
| Trigger II | yes (DTU-1031X) | no |
| Modes | from the monitor's EDID, timings and PLL computed per mode (MCT's formula): up to 1080p144, 2560x1440@60, 3440x1440@60, 3840x2160@30; extra refresh rates; fallbacks for out-of-reach and missing EDIDs | 9 fixed modes with validated timing blobs, 60 Hz (1080p also 30 Hz) |
| Pixel formats | RGB24, RGB32, YUV 4:4:4, NV12; auto: lossless at rest, NV12 during large motion; per-format scanout limits | output 0 raw XRGB8888, output 1 NV12; in-kernel JPEG encoder as a fallback |
| Partial updates | rectangle writes (changed rows and columns, up to 16 bands) into a 3-buffer ring, tear-free | output 0: changed rows written in place; output 1: full NV12 frames into 3 slots |
| Hardware cursor | yes, 64/96/128 px | no |
| Gamma / night light | yes (applied in software; the chip has no tables at feature level 6) | no |
| Monitor hotplug | interrupt events, debounced; polling fallback | EDID read at probe; keepalive work |
| Error recovery | vendor cancel request, endpoint reset, redraw on request | reset on timeout, circuit breaker after 5 failures |
| USB 2 links | yes (smaller writes, modes up to 2048 wide) | not addressed |
| Kernels | 7.2 and later | 5.10 and later |
| Per-device settings | sysfs per device (format, all_modes, lossy) | module parameters (global), per-output enable in sysfs |
| Device information | board, firmware, functions, mode table, EDID, USB in sysfs / app | metrics in sysfs |
| Companion app | Rust, GTK 4 / libadwaita window and tray, `mctctl` | Python GTK 4 window, tray, `mct-t6-ctl` |
| Mainline fit | no codecs in the kernel; DRM helpers (GEM shmem, damage helpers, vblank timer) like udl and gud | in-kernel JPEG encoder; compatibility code for older kernels |

What trigger6 has that mct doesn't: **two outputs per chip and several
chips**, older kernels, and work on the `0711:5601` dock family. Its
mode-switch sequence for sibling heads (timing before and after setting
all heads) is worth reading if mct drives a second output.

What mct has that trigger6 doesn't: modes from the EDID with computed
timings and high refresh rates, lossless formats with an automatic switch,
the hardware cursor, gamma, debounced hotplug from interrupt events, USB 2
support, and the Trigger II backend.

## Structure

trigger6 describes its DRM side as following the in-kernel udl driver.
mct's shared core (`kernel/mct_drv.c`) is built from the same parts as the
in-kernel USB display drivers udl and gud: GEM shmem buffers, a shadow
plane, damage clips, a worker that sends updates without blocking commits,
and the vblank timer helpers.
