# mct-linux

A Linux DRM/KMS driver for USB displays built on MCT (Magic Control
Technology) Trigger chips, with a desktop app and a command line tool for
its settings.

- **Trigger 6 (T6):** USB 3 to HDMI adapters such as the j5create JUA254
  ("T6 USB Station", `0711:5600`; `0711:5601` should work too). Any HDMI
  monitor, from its EDID: up to 1080p at 144 Hz, 2560x1440 and 3440x1440 at
  60 Hz, 3840x2160 at 30 Hz.
- **Trigger II (T2):** the Wacom DTU-1031X pen display (1024x600, USB
  `0711:520d` / `0711:5209`). The pen (`056a:032f`) is a separate USB
  function handled by the in-kernel wacom driver.

> [!IMPORTANT]
> The T2 backend has only been tested on the **Wacom DTU-1031X** and is
> written for it: its fixed panel and 180 degree scanout, its init sequence
> taken from Wacom's driver, its brightness control through the pen
> function. Trigger II products differ from each other far more than
> Trigger 5/6 adapters do (each has its own init sequence and panel setup),
> so other T2 devices will most likely not work without changes.

MCT's own drivers are for Windows and macOS, plus a userspace Linux driver
for the T6. The protocols here were worked out from USB captures, analysis
of the vendor drivers for interoperability, and testing on real hardware.

## Status

✅ works &nbsp; ⚠️ works with limits, or untested &nbsp; ❌ not supported

**Trigger 6 (j5create JUA254, `0711:5600`)**

| | Feature | Notes |
|---|---|---|
| ✅ | Picture on any HDMI monitor | modes from the monitor's EDID |
| ✅ | High refresh rates | up to 1080p at 144 Hz |
| ✅ | Large modes | 2560x1440@60, 3440x1440@60, 3840x2160@30 |
| ✅ | Lossless picture | RGB or YUV 4:4:4 at rest, NV12 during motion (automatic) |
| ✅ | Hardware cursor | up to 128 px |
| ✅ | Monitor and adapter hotplug | |
| ✅ | DPMS, suspend/resume | |
| ✅ | Gamma (night light) | |
| ✅ | Text console | |
| ⚠️ | USB 2 ports | works, but modes up to 2048 wide and slower motion |
| ⚠️ | `0711:5601` single-output adapters | should work, untested |
| ❌ | Second output of dual-head adapters and docks | |
| ❌ | HDMI audio | |
| ❌ | JPEG mode | would need an encoder in the kernel ([roadmap](docs/roadmap.md)) |
| ❌ | Hardware rotation | the compositor rotates instead |

**Trigger II (Wacom DTU-1031X)**

| | Feature | Notes |
|---|---|---|
| ✅ | Picture | 1024x600, tear-free, lossless by default |
| ✅ | Hardware cursor | |
| ✅ | Brightness | through the standard backlight interface |
| ✅ | Pen | in-kernel wacom driver |
| ✅ | DPMS, suspend/resume, hotplug | |
| ✅ | Lossy mode for heavy motion | optional |
| ⚠️ | Faint vertical lines on some starts | hardware; a reload or replug fixes it ([known issues](#known-issues)) |
| ❌ | Other Trigger II devices | see the note above |

**General**

| | Feature | Notes |
|---|---|---|
| ✅ | Wayland and X11 compositors | |
| ✅ | Linux 7.2 and later | mainly developed and tested on 7.2 |
| ✅ | Linux 6.12 LTS to 7.1 | through a compatibility layer; built in CI for 6.12 and 6.18, tested on 6.12 in a VM with the JUA254 |
| ❌ | Linux older than 6.12 | |
| ✅ | Desktop app and command line tool | MCT Display Manager, `mctctl` |

## Features

**Both chips**
- works with Wayland and X11 compositors and the text console
- hardware cursor, DPMS, suspend/resume, hotplug of the USB device
- commits never wait for USB: a worker sends what changed, the newest image
  wins, tear-free
- only rows that really changed are sent
- per-device settings and information in sysfs, statistics in debugfs

**Trigger 6**
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

**Trigger II (DTU-1031X)**
- lossless picture by default, optional lossy mode for heavy motion that
  turns lossless again once motion stops
- backlight brightness, recovery from a device left mid-frame

## Supported devices

| USB ID | Device | Chip | Status |
|---|---|---|---|
| `0711:5600` | j5create JUA254, "T6 USB Station" | Trigger 6 | tested |
| `0711:5601` | single-output Trigger 6 adapter | Trigger 6 | untested |
| `0711:520d` | Wacom DTU-1031X, interface 0 | Trigger II | tested |
| `0711:5209` | the same tablet, which enumerates with either ID | Trigger II | tested |

Other Trigger 6 IDs (MCT uses `0711:5600`-`561f` and more) are not matched
yet: they may have other outputs or features this driver doesn't handle.
Other Trigger II products need their own init and modes (see above).

## Repository

| Path | Contents |
|---|---|
| [`kernel/`](kernel) | the driver, module `mct`: a shared DRM core (`mct_drv.c`) and a backend per chip (`mct_t2.c`, `mct_t6.c`) |
| [`app/`](app) | MCT Display Manager (GTK 4 window and tray icon) and `mctctl` (command line) |
| [`nix/`](nix) | flake package and NixOS module, two optional niri patches |
| [`packaging/`](packaging) | release files: DKMS config, udev rules, the app's install script, the release build script (run by [`.github/workflows/`](.github/workflows) on a version tag) |
| [`docs/`](docs) | protocol references, limits, driver notes, lab notes ([index](docs/README.md)) |
| [`tools/`](tools) | test and research scripts ([index](tools/README.md)) |
| [`wacom-t2d/`](wacom-t2d) | legacy: the DTU-1031X as a userspace daemon on top of EVDI, for kernels older than 6.12 and for protocol work; also holds the tablet's init sequence the kernel driver is built from |
| `data/`, `captures/`, `vm/` | first probe outputs, an encoded test frame, how the Windows VM for the captures was run (the captures themselves and the vendor files are not published) |

## Installing from a release

Each [release](https://github.com/XXCoreRangerX/mct-linux/releases) has the
driver as DKMS source (built on your machine for every installed kernel, 6.12
or later) and MCT Display Manager with `mctctl` for x86_64 and aarch64:

```sh
sudo tar -C /usr/src -xzf mct-dkms-VERSION.tar.gz
sudo dkms install mct/VERSION
sudo modprobe mct

tar -xzf mct-app-VERSION-x86_64.tar.gz && cd mct-app-VERSION-x86_64
sudo ./install.sh        # app, polkit policy, udev rules; --uninstall removes them
```

The app needs GTK 4.14 and libadwaita 1.5 or later (Ubuntu 24.04, Fedora
40, Arch and newer).

## Building

Needs Linux 6.12 or later with `DRM`, `DRM_KMS_HELPER`,
`DRM_GEM_SHMEM_HELPER`, `BACKLIGHT_CLASS_DEVICE` and `USB` enabled, and the
kernel's headers or build tree.

```sh
make -C kernel KDIR=/lib/modules/$(uname -r)/build
sudo make -C kernel install
sudo depmod -a
sudo modprobe mct
```

The driver is written for the DRM API of Linux 7.2 and mainly tested there.
On 6.12 to 7.1, `kernel/Kbuild` detects which newer interfaces the kernel
lacks and `kernel/mct_compat.h` maps them onto the older ones (the renamed
atomic state, the vblank timer helpers, the fbdev setup, the DMA device for
buffer imports). On 7.2 and later none of it is used.

`modprobe` also loads the DRM helper modules mct uses; a plain
`sudo insmod kernel/mct.ko` works for a quick test only once those are
loaded (otherwise it fails with "Unknown symbol"). To reload after a
rebuild, unbind the devices (`echo <interface> >
/sys/bus/usb/drivers/mct/unbind` for each entry there), then `rmmod mct`
and `modprobe mct`. With Secure Boot, the module has to be signed with a key
the kernel trusts.

`MCT_T2=n` or `MCT_T6=n` on the make command line leaves a backend out. The
driver binds to supported devices automatically. The T6 shows up as an
`HDMI-A-n` connector, the tablet as `USB-n`.

The scripts in `tools/` open the devices directly; give your user access
with a udev rule such as (the NixOS module installs this one):

```udev
SUBSYSTEM=="usb", ATTR{idVendor}=="0711", ATTR{idProduct}=="5209|520d|5600|5601", TAG+="uaccess"
```

### NixOS

Plain `configuration.nix`:

```nix
imports = [ /path/to/mct-linux/nix/nixos.nix ];
hardware.mct.enable = true;
```

With flakes, add this repository as an input and import
`nixosModules.default`. The module builds the driver for the system's own
kernel (`boot.kernelPackages`); the flake's `packages.x86_64-linux.mct-kmod`
is built for `linuxPackages_latest`. The module also installs `mct-reload`,
which unbinds the devices and loads the module again (after a rebuild,
without a reboot).

| Option (`hardware.mct.*`) | Meaning |
|---|---|
| `enable` | build and load the driver |
| `format`, `allModes`, `lossy` | defaults for new devices (see [Settings](#settings)) |
| `driver = "evdi"` | the EVDI + wacom-t2d route for the tablet instead of the kernel driver |
| `consoleMap` | text consoles on the tablet (adds `fbcon=map:...`, e.g. `"01"` for every second VT) |
| `niriFix` | build niri with the two patches in `nix/`: drop a removed USB display's output; render display-only devices into system memory, which makes reading frames many times faster |
| `app.enable`, `app.tray` | install MCT Display Manager and `mctctl`; start the tray icon with the session |

## Settings

Every device has its own settings, in
`/sys/bus/usb/devices/<interface>/mct/`, with read-only information next to
them ([all files](docs/driver/companion-app.md)):

| File | Chip | Values |
|---|---|---|
| `format` | T6 | `auto` (default), `rgb24`, `rgb32`, `yuv444`, `nv12`. `auto` uses exact RGB or YUV 4:4:4 while the screen is still and NV12 during large changes. A fixed format stays fixed (no NV12 for motion) where the adapter can scan it out at the current mode, NV12 elsewhere. |
| `all_modes` | T6 | `1` also offers modes larger than the monitor's native resolution (off by default; monitors often list 4K and scale it down). The compositor re-probes the monitor on a change. |
| `lossy` | T2 | maximum lossy tolerance (`1`-`8`) for updates too big for one 60 Hz frame, refined to lossless once motion stops. `0` = off (default). |

```sh
echo nv12 | sudo tee /sys/bus/usb/devices/2-2:1.0/mct/format
```

The module parameters of the same names (`/sys/module/mct/parameters/`,
`modprobe mct format=...`) are the values new devices start with. To change
settings without root, make the files writable for a group, as the NixOS
module does for group `video`:

```udev
ACTION=="bind", SUBSYSTEM=="usb", DRIVER=="mct", RUN+="/bin/sh -c 'cd /sys%p/mct && for f in format all_modes lossy; do [ -e $f ] && chgrp video $f && chmod g+w $f; done'"
```

Brightness of the DTU-1031X: `/sys/class/backlight/mct_t2-N` (0 = off,
1-9), e.g. `brightnessctl -d 'mct_t2-*' set 3`.

Text console on a display: map a VT to its framebuffer, e.g.
`con2fbmap 2 1`.

## MCT Display Manager and mctctl

[`app/`](app) holds a desktop app for the settings above and a command line
tool, both per device. On NixOS: `hardware.mct.app.enable = true`, and
`app.tray = true` for the tray icon at login; elsewhere `cargo build
--release` in `app/` (see [app/README.md](app/README.md)).

- **`mct-manager`:** one page per connected display with its mode, format,
  settings, live update and data rate, transfer errors, and everything that
  identifies it (product, chip and firmware, USB, driver, the monitor's
  EDID). Switches the DTU-1031X between the kernel driver and the legacy
  wacom-t2d daemon. Warns when a T6 runs over USB 2.
- **`mct-manager --tray`:** the same from a tray icon (StatusNotifierItem).
- **`mctctl`:** `list [-v|-vv]`, `info [-v|-vv] [DEV]`, `get`/`set [DEV] KEY
  [VALUE]`, `stats -w`, `driver kernel|legacy`; `--json` for scripts.

Settings changed in the app or with `mctctl set` are remembered in
`~/.config/mct/settings.toml` and restored when the display reconnects (by
the running app, or `mctctl apply`). Writing settings without root needs
the udev rule above and membership in group `video`.

## Debugging

```sh
sudo cat /sys/kernel/debug/dri/*/mct_t6_stats
sudo cat /sys/kernel/debug/dri/*/mct_t2_stats
```

These show the mode, the format in use, update rates, bytes sent, timings
and error counters ([fields](docs/driver/debugfs.md)). The same basics,
without root, are in each device's `mct/` directory in sysfs and in
`mctctl info -v`. The T6 logs its board, firmware, feature level and video
memory at probe.

## Known issues

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
  hot, and the author of the
  [iFixit disassembly guide](https://www.ifixit.com/Guide/Wacom+DTU-1031X+Disassembly/166191)
  found it damaged in all three of their units.
- Compositors that hand over buffers in GPU memory make the driver read
  them over PCIe (a few ms per frame at 1080p); niri with the patch above
  doesn't.

## Documentation

[docs/README.md](docs/README.md) lists everything, with the status of each
file:

- [`docs/reference/`](docs/reference): the T6 and T2 protocols, the T6
  limits and the measurements behind them, the DTU-1031X's hardware
- [`docs/driver/`](docs/driver): how the driver works, its sysfs and
  debugfs files, the companion app
- [`docs/research/`](docs/research): the vendor drivers
- [`docs/history/`](docs/history): the lab notebooks
- [`docs/roadmap.md`](docs/roadmap.md): where things stand

## How this was made

This driver was developed with AI coding agents (Anthropic's Claude, through
Claude Code). These tools sped up the slow parts of the protocol work and
driver development: analysing the vendor drivers for interoperability and
reading USB captures, writing test scripts,
drafting code and documentation, and chasing down bugs.

Every step was directed, reviewed and tested by a human
([Filip Piękoś](https://github.com/XXCoreRangerX)) on real hardware: the
DTU-1031X and the JUA254 with several monitors. Nothing went into the
driver on an agent's word alone: each protocol finding was checked on the
devices, and each feature was tried live, watching the screen. The lab
notes in [`docs/history/`](docs/history) record those tests, including the
ones that failed.

## Credits

- [cyrozap's notes](https://github.com/cyrozap/mct-usb-display-adapter-re)
  (`doc/Protocol-T2.md`, `Protocol-T6.md`) were the starting point for the
  protocol documentation.
- [JingYenLoh/trigger6](https://github.com/JingYenLoh/trigger6), a separate
  Linux driver for the Trigger 6, and its Windows capture were the starting
  point for the T6 work.
- [y-chan](https://github.com/y-chan/trigger6)'s and
  [ramifriedman](https://github.com/ramifriedman/mct-t6-linux)'s T6 drivers
  and notes helped with partial updates. ramifriedman's trigger6 drives the
  four-output StarTech USBC2HD4 (two T6 chips, two outputs each), which mct
  doesn't yet.
- MCT's own Linux driver, [triggerdm](https://github.com/mcttrigger/triggerdm)
  (an EVDI daemon, GPL-2.0-or-later), and its Ubuntu version were read for
  protocol facts; no code was taken.

How mct compares with these drivers:
[docs/research/other-linux-drivers.md](docs/research/other-linux-drivers.md).

The DRM side is built like the in-kernel USB display drivers udl and gud
(GEM shmem buffers, a shadow plane, damage clips, a worker that never blocks
commits).

Mainline Linux has no driver for MCT's Trigger display chips. MCT's USB
vendor ID shows up there only for the mct_u232 serial adapter, some
SiS-based sisusbvga adapters, an ax88179 Ethernet adapter and an audio
quirk for the Trigger 5.

## License

Code (`kernel/`, `app/`, `tools/`, `wacom-t2d/`, `nix/`) is GPL-2.0-only,
see [LICENSE](LICENSE) and the SPDX line in each file; Copyright (C) 2026
Filip Piękoś <mail@filippiekos.com>. The patches in `nix/` follow the
licence of the project they change (niri: GPL-3.0-or-later, EVDI: GPL-2.0).
The documentation in `docs/` is CC BY-SA 4.0
([docs/LICENSE.md](docs/LICENSE.md)), as it builds on cyrozap's notes. No
vendor code or binaries are included. [CHANGELOG.md](CHANGELOG.md) lists
the changes per version.
