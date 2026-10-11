# Companion app: design (2026-10-11, not built yet)

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

## Writing without root

Either a udev rule that makes the writable attributes group-writable for
the seat user (uaccess-style), or a tiny polkit-checked helper that only
writes those attributes. The NixOS module installs whichever is chosen.

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
