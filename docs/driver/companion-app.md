# Companion app: design (2026-10-11)

> **Design** (2026-10-11): the plan for the app, with the identification and saved settings sections kept current. The sysfs files are listed in [ABI-sysfs-mct.txt](ABI-sysfs-mct.txt).

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

## The interface (fixed 2026-10-11, in the driver since mct_t6 0.23.0 / mct_t2 0.3.20)

Per bound device, in `/sys/bus/usb/drivers/mct/<interface>/mct/` (the same
directory as `/sys/bus/usb/devices/<interface>/mct/`); `<interface>` is
e.g. `2-2:1.0`. One value per file, newline terminated.

The files, their formats and which chip has which are listed in
[ABI-sysfs-mct.txt](ABI-sysfs-mct.txt) (the kernel's ABI format), which is
kept current with the code. Besides the counters and settings there is
read-only information: what the T6 reports at probe (platform, board,
firmware, serial, functions, mode table), its HDMI audio (`audio`,
`audio_volume`, `audio_sink`), and the T2's ID ROM (`vram_mb`, `id_rom`).

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

- Planned as Qt 6 (PySide6) for the tray; built instead with GTK 4,
  libadwaita and Relm4 for the GNOME look, the tray icon through ksni
  (StatusNotifierItem: Noctalia, Waybar, KDE). `--tray` waits for a tray host
  that starts after the app (2026-10-11: at login the icon was missing).
- Tray menu: each device with its mode and format, quick toggles, "Open".
- Window:
  - a list of connected devices (hotplug through udev)
  - per device: info (chip, model, feature level, video memory, USB link,
    connector, mode, format), settings (format, show all modes, brightness
    and lossy for the tablet), live stats (update rate, data rate, errors)
  - About: version, licence, links, credits
- Packaged in the flake; NixOS option `hardware.mct.app.enable`.

## Identification (2026-10-11)

Besides the driver's files the app reads the USB device (`idVendor`,
`idProduct`, `manufacturer`, `product`, `serial`, `bcdDevice`, `version`,
`speed`, `busnum`/`devnum`, `bMaxPower`), the monitor's EDID from
`/sys/class/drm/<connector>/edid` (maker, name, product code, serial, date,
size), for the tablet the Wacom pen digitizer on the same hub, and for the
T6 its HDMI audio (the driver's `audio`, `audio_volume` and `audio_sink`,
the card's id from `/sys/class/sound/cardN/id` and whether it plays from
`/proc/asound/cardN/pcm0p/sub0/status`). The
product name comes from the board name (table `BOARDS` in `app/src/ident.rs`)
or the USB IDs. All of it shows in the window's lower groups, in
`mctctl info -v` / `list -v`, and in the About dialog's Troubleshooting
report.

Technical details (`mctctl -vv`, the window's collapsed "Technical
details" group, always in the report): the adapter's functions, mode table
and clock limits; USB descriptors (classes, configuration, power
attributes, lanes, LPM, runtime power state, every endpoint); the monitor's
EDID decoded (input, preferred timing, range limits, HDMI TMDS clock, CTA
modes) and as hex; the DRM card, framebuffer, module defaults and
srcversion.

Known boards: `JUA254H` j5create JUA254 (USB 3.0 to HDMI; the 5600 here),
`A8613HV` MCT U3-A8613 (HDMI + VGA, the 5601 in the trigger6 capture),
`DTU-1031AX` Wacom DTU-1031AX (in MCT's firmware package). More board names
are in MCT's `*.t6img` firmware files (see the lab's dll-analysis notes).

## Saved settings (0.5.1)

`app/src/settings.rs`. After a successful change (window, tray,
`mctctl set` without `--temporary`) the display's settings (format,
all_modes, lossy, brightness) are written to `~/.config/mct/settings.toml`
(`$MCT_SETTINGS`; inside the fake tree when `MCT_SYSFS_ROOT` is set), one
TOML table per display. The key is `VID:PID/serial-S` with the USB serial,
the T6 chip serial or the DTU-1031X's pen serial, else `VID:PID/port-P`
with the USB port (the JUA254 has no serial). `Reapplier` runs in the
tray's one-second loop (or the window's when there is no tray): a display
whose mct directory is new (another inode: the driver probed it again)
gets the saved values that differ written back. `mctctl apply`, `forget`
and `saved` handle it by hand.

## Later

- The optional JPEG mode for the T6 (userspace encoder) would live next to
  the app (see roadmap.md).
- Mode selection stays with the compositor; the app can show the modes and
  link to the compositor's settings.
