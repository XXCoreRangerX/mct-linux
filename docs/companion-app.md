# Companion app: design (2026-10-11)

A desktop app for mct devices in the spirit of BetterDisplay or the
DisplayLink Manager: a tray icon, one page per connected device, settings,
live status, an About page. Several devices at once (two T6 adapters and the
tablet, say) each get their own settings.

## Driver side first

Module parameters (`format`, `all_modes`, `lossy`) apply to every device.
For per-device settings and for information without root:

- **sysfs attributes per device**, on the USB interface
  (`/sys/bus/usb/devices/<intf>/mct/`):
  - read-only, world-readable: `chip` (t2 / t6), `model`, `feature_level`,
    `vram_mb`, `usb_speed`, `mode` (current, e.g. `3440x1440@60`),
    `pixel_format` (in use), `updates`, `bytes`, `errors` / `recoveries`
  - read-write: `format` (T6), `all_modes` (T6; re-probes the connector on
    change), `lossy` (T2)
  - the module parameters become the defaults new devices start with
- the T2's brightness stays the standard backlight device
- the connector name (HDMI-A-n, USB-n) is linked from there, so the app can
  match a device to what the compositor shows

## The interface (fixed, 2026-10-11)

Per bound device, in `/sys/bus/usb/drivers/mct/<interface>/mct/` (the same
directory as `/sys/bus/usb/devices/<interface>/mct/`); `<interface>` is
e.g. `2-2:1.0`. One value per file, newline terminated.

| File | Devices | Access | Content |
|---|---|---|---|
| `chip` | all | ro | `t2` or `t6` |
| `model` | all | ro | the USB product string, e.g. `T6 USB Station` |
| `connector` | all | ro | DRM connector name, e.g. `card2-HDMI-A-3` (the compositor calls it `HDMI-A-3`) |
| `mode` | all | ro | current mode `WxH@Hz`, or `off` |
| `updates` | all | ro | frames sent since probe |
| `bytes` | all | ro | bytes sent since probe |
| `errors` | all | ro | transfer errors since probe |
| `pixel_format` | t6 | ro | format in use: `NV12`, `YUV444`, `RGB24`, `RGB32`, or `-` |
| `feature_level` | t6 | ro | number, `-1` if unknown |
| `vram_mb` | t6 | ro | number |
| `usb_speed` | t6 | ro | `usb2` or `usb3` |
| `recoveries` | t6 | ro | device restarts after transfer errors |
| `format` | t6 | rw | `auto`, `rgb24`, `rgb32`, `yuv444`, `nv12` |
| `all_modes` | t6 | rw | `0` / `1`; a change makes userspace re-probe the monitor |
| `lossy` | t2 | rw | `0` to `8` |
| `backlight` | t2 | ro | name of its backlight device in `/sys/class/backlight/` (brightness is set there, 0 = off, 1 to `max_brightness`) |

The module parameters (`format`, `all_modes`, `lossy`) are the values new
devices start with; after that each device has its own.

Devices appear and disappear with their USB interface: udev `bind` /
`unbind` events on subsystem `usb` with driver `mct`. Counters change all
the time; an app polls them (about once a second while visible).

## Writing without root

A udev rule makes the writable files group `video` and group-writable when
a device binds (chosen 2026-10-11); the NixOS module installs it, other
distributions copy it from the README.

## The app

- Qt 6 (PySide6): tray icon through StatusNotifierItem works on Wayland
  (Noctalia, Waybar, KDE), GTK 4 has no tray.
- Tray menu: each device with its mode and format, quick toggles, "Open".
- Window:
  - a list of connected devices (hotplug through udev)
  - per device: info (chip, model, feature level, video memory, USB link,
    connector, mode, format), settings (format, show all modes, brightness
    and lossy for the tablet), live stats (update rate, data rate, errors)
  - About: version, licence, links, credits
- Packaged in the flake; NixOS option `hardware.mct.app.enable`.

## Later

- The optional JPEG mode for the T6 (userspace encoder) would live next to
  the app (see roadmap.md).
- Mode selection stays with the compositor; the app can show the modes and
  link to the compositor's settings.
