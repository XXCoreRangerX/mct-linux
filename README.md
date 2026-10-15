# mct-linux

A Linux driver for USB displays built on MCT (Magic Control Technology)
Trigger chips, with a desktop app for their settings.

- **Trigger 6:** USB 3 to HDMI adapters such as the j5create JUA254. Any
  HDMI monitor, up to 1080p at 144 Hz or 4K at 30 Hz, with HDMI audio.
- **Trigger II:** the Wacom DTU-1031X pen display. The pen keeps working
  through the kernel's own wacom driver.

MCT only ships drivers for Windows and macOS, plus a userspace Linux driver
for the Trigger 6. This one is a kernel driver: it works with any Wayland or
X11 desktop and the text console.

> [!NOTE]
> Work in progress, tested on one device of each chip. Trigger II products
> differ a lot from each other, so other Trigger II devices most likely need
> changes before they work.

## Highlights

- **Lossless picture** at rest; only the parts of the screen that changed
  are sent, without tearing.
- **High refresh rates** on the Trigger 6: 1080p up to 144 Hz, 720p up to
  240 Hz.
- **A fast hardware cursor**, as fast as MCT's Windows driver, also on
  Wayland with the included [niri patches](#fast-cursor-on-wayland).
- **A desktop app and `mctctl`** for per-display settings, saved and
  restored.

## Status

✅ works &nbsp; ⚠️ works with limits, or untested &nbsp; ❌ not supported &nbsp; ➖ doesn't apply

| | Trigger 6 (j5create JUA254) | Trigger II (Wacom DTU-1031X) |
|---|---|---|
| Picture | ✅ any HDMI monitor | ✅ built-in 1024x600 panel |
| Modes | ✅ from the monitor: 1080p up to 144 Hz, 720p up to 240 Hz, 1440p and 3440x1440 at 60 Hz, 4K at 30 Hz | ✅ 1024x600 at 60 Hz |
| Picture quality | ✅ lossless at rest, NV12 during large motion | ✅ lossless; optional lossy mode for heavy motion |
| Hardware cursor | ✅ | ✅ |
| Night light (gamma) | ✅ | ✅ |
| Brightness | ➖ use the monitor's buttons | ✅ |
| HDMI audio | ✅ | ➖ |
| Hotplug, suspend/resume | ✅ | ✅ |
| Text console | ✅ | ✅ |
| USB link | ✅ USB 3; ⚠️ USB 2 is slower | ✅ USB 2 |
| Pen | ➖ | ✅ |
| JPEG compression | 🚧 [researched](docs/research/t6-jpeg.md), not in the driver | ➖ |
| Second output (docks) | ❌ | ➖ |
| Hardware rotation | ❌ the desktop rotates instead | ❌ the desktop rotates instead |

Linux 6.12 or later; mainly developed on 7.2.

## Supported devices

| USB ID | Device | Status |
|---|---|---|
| `0711:5600` | j5create JUA254 ("T6 USB Station") | ✅ tested |
| `0711:520d`, `0711:5209` | Wacom DTU-1031X (it uses either ID) | ✅ tested |
| `0711:5601` | single-output Trigger 6 adapter | ⚠️ untested |
| `0711:5201`, `0711:520f` | other DTU-1031X units | ⚠️ untested |
| `0711:5602`-`560b`, `0711:561a`, `19ff:560b`-`560e`, `03f0:0182` | other Trigger 6 adapters and docks (j5create, StarTech, Mobile Pixels, Insignia, HP) | ⚠️ untested, [opt-in](docs/usage.md#settings) |
| `0711:5609`, `056a:039f` | Wacom DTU-1031AX | ⚠️ untested, opt-in |

Docks with two outputs drive one of them.

## Installing

- **From a release:** each
  [release](https://github.com/XXCoreRangerX/mct-linux/releases) has the
  driver as a DKMS package and the app for x86_64 and aarch64.
- **NixOS:** import the module from this repository and set
  `hardware.mct.enable = true`.
- **From source:** `make -C kernel && sudo make -C kernel install`.

Step by step, with the NixOS options: [docs/install.md](docs/install.md).

## Using it

The displays show up like any other monitor; set them up in your desktop's
display settings. Per-display options (pixel format, lossy mode, extra
modes) are in **MCT Display Manager**, its tray icon, or on the command
line:

```sh
mctctl list                  # the displays, numbered
mctctl set 0 format nv12     # a setting on display 0
```

All settings, the sysfs files behind them and debugging:
[docs/usage.md](docs/usage.md).

## Fast cursor on Wayland

Wayland desktops normally move the cursor together with the next frame, so
with a fast mouse each move can wait up to one refresh (17 ms at 60 Hz).
X11 and Windows move it straight away. mct can take cursor moves between
frames, and the patches in [`nix/`](nix) make niri (and smithay, the
library it's built on) do that. A move then takes about 0.4 ms, the same as
X11 and Windows.

On NixOS, turn on `niriFix` and `niriImmediateCursor`. Other Wayland
desktops would need a similar change.
[Why this happens, and the measurements.](docs/research/cursor-latency.md)

## Compared with the vendor drivers

| | mct | MCT's Windows driver |
|---|---|---|
| Picture | lossless | JPEG (Trigger 6), slightly lossy (Trigger II) |
| Refresh rate (Trigger 6) | up to 144 Hz | 60 Hz |
| Cursor move, fast mouse | 0.44 ms | 0.39 ms |
| Video | steady | bursty; stalls on the Trigger II |
| Two-output docks, USB 2 | ❌, slower | ✅ |

Measured side by side; the details, and MCT's Linux driver:
[docs/comparison.md](docs/comparison.md).

## Known issues

- Some monitors hang after the signal drops and comes back, or on a mode
  they list but don't accept; turning the monitor off and on fixes it.
- After a USB error the Trigger 6 resets itself: about 3 s without picture.
- The DTU-1031X sometimes starts with faint vertical lines; replugging
  fixes it. If they never go away, its controller chip may be failing,
  which is common on these tablets.

## Documentation

[docs/README.md](docs/README.md) lists everything:

- [install.md](docs/install.md), [usage.md](docs/usage.md): installing,
  settings, debugging
- [comparison.md](docs/comparison.md): mct and the vendor drivers
- [`docs/reference/`](docs/reference): the chips' USB protocols and limits
- [`docs/driver/`](docs/driver): how the driver works
- [`docs/research/`](docs/research): investigations (cursor latency, JPEG,
  the vendor drivers)
- [`docs/history/`](docs/history): the lab notebooks, every experiment
- [roadmap.md](docs/roadmap.md): what's next
- [CHANGELOG.md](CHANGELOG.md): changes per version

## Repository

| Path | Contents |
|---|---|
| [`kernel/`](kernel) | the driver (module `mct`): a shared DRM core and a backend per chip |
| [`app/`](app) | MCT Display Manager and `mctctl` |
| [`nix/`](nix) | flake package, NixOS module, the niri and smithay patches |
| [`packaging/`](packaging) | release files: DKMS, udev rules, install script |
| [`docs/`](docs) | documentation |
| [`tools/`](tools) | test and research scripts ([index](tools/README.md)) |
| [`wacom-t2d/`](wacom-t2d) | legacy: the DTU-1031X as a userspace daemon on top of EVDI |
| `data/`, `captures/`, `vm/` | early probe outputs and capture setup notes |

## How this was made

This driver was developed with AI coding agents (Anthropic's Claude, through
Claude Code). They sped up the slow parts: analysing the vendor drivers for
interoperability, reading USB captures, writing test scripts, drafting code
and documentation, and chasing down bugs.

Every step was directed, reviewed and tested by a human
([Filip Piękoś](https://github.com/XXCoreRangerX)) on real hardware: the
DTU-1031X and the JUA254 with several monitors. Nothing went into the
driver on an agent's word alone; the lab notes in
[`docs/history/`](docs/history) record the tests, including the ones that
failed.

## Credits

- [cyrozap's notes](https://github.com/cyrozap/mct-usb-display-adapter-re)
  were the starting point for the protocol documentation.
- [JingYenLoh/trigger6](https://github.com/JingYenLoh/trigger6) and its
  Windows capture were the starting point for the Trigger 6 work;
  [y-chan](https://github.com/y-chan/trigger6)'s and
  [ramifriedman](https://github.com/ramifriedman/mct-t6-linux)'s drivers
  helped with partial updates.
- MCT's own Linux driver, [triggerdm](https://github.com/mcttrigger/triggerdm),
  was read for protocol facts; no code was taken.
- [rhgndf/trigger2](https://github.com/rhgndf/trigger2) is where the Trigger
  II timing registers and clock formula came from.
- The driver is built like the kernel's own USB display drivers, udl and
  gud.

How mct compares with these drivers:
[Trigger 6](docs/research/t6-other-drivers.md),
[Trigger II](docs/research/t2-other-drivers.md).

## License

Code (`kernel/`, `app/`, `tools/`, `wacom-t2d/`, `nix/`) is GPL-2.0-only
([LICENSE](LICENSE)); Copyright (C) 2026 Filip Piękoś
<mail@filippiekos.com>. The patches in `nix/` follow the licence of the
project they change (niri: GPL-3.0-or-later, smithay: MIT, EVDI: GPL-2.0).
The documentation in `docs/` is CC BY-SA 4.0
([docs/LICENSE.md](docs/LICENSE.md)), as it builds on cyrozap's notes. No
vendor code or binaries are included.
