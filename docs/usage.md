# Using the driver

## Settings

Every device has its own settings in `/sys/bus/usb/devices/<interface>/mct/`,
with read-only information next to them
([all files](driver/ABI-sysfs-mct.txt)). The app and `mctctl` (below) set the
same files.

| File | Chip | Values |
|---|---|---|
| `format` | T6 | `auto` (default): exact RGB or YUV 4:4:4 while the screen is still, NV12 during large changes. Or a fixed `rgb24`, `rgb32`, `yuv444`, `nv12`; a fixed format stays fixed where the adapter can scan it out at the current mode, NV12 elsewhere. |
| `all_modes` | T6 | `1` also offers modes larger than the monitor's native resolution (off by default: monitors often list 4K and scale it down). |
| `lossy` | T2 | `0` (default) = always lossless. `1`-`8`: the maximum lossy tolerance for updates too big for one 60 Hz frame, refined to lossless once motion stops. Green keeps the tolerance, red gets 1.5 times and blue 2 times, where the eye notices less. |
| `motion` | T2 | `lossy` (default): the `lossy` level decides. `half` (experimental, can glitch at the edges of moving windows): full-screen motion too big for one frame is sent at half resolution and upscaled by the tablet, refined once motion stops. |

```sh
echo nv12 | sudo tee /sys/bus/usb/devices/2-2:1.0/mct/format
```

The module parameters of the same names (`/sys/module/mct/parameters/`,
`modprobe mct format=...`) are the values new devices start with.

To change settings without root, make the files writable for a group, as
the NixOS module does for group `video`:

```udev
ACTION=="bind", SUBSYSTEM=="usb", DRIVER=="mct", RUN+="/bin/sh -c 'cd /sys%p/mct && for f in format all_modes lossy motion; do [ -e $f ] && chgrp video $f && chmod g+w $f; done'"
```

**Brightness** of the DTU-1031X: `/sys/class/backlight/mct_t2-N` (0 = off,
1-9), e.g. `brightnessctl -d 'mct_t2-*' set 3`.

**Text console** on a display: map a VT to its framebuffer, e.g.
`con2fbmap 2 1`.

**Untested Trigger 6 devices** (see the README's device list) are only driven with the
module parameter `untested_ids`: `mct.untested_ids=1` on the kernel command
line, or `options mct untested_ids=1` in `/etc/modprobe.d/`, then replug. The
kernel log names any device it skipped. The other DTU-1031X IDs (`0711:5201`,
`0711:520f`) are only driven when the panel reports Wacom's EDID, because
plain Trigger II adapters use the same IDs.

## MCT Display Manager and mctctl

- **`mct-manager`:** one page per connected display with its mode, format,
  settings, live update and data rate, transfer errors, and everything that
  identifies it (product, chip and firmware, USB, driver, the monitor's
  EDID). It switches the DTU-1031X between the kernel driver and the legacy
  wacom-t2d daemon, and warns when a T6 runs over USB 2.
- **`mct-manager --tray`:** the same from a tray icon (StatusNotifierItem).
- **`mctctl`:** `list [-v|-vv]`, `info [-v|-vv] [DEV]`, `get`/`set [DEV] KEY
  [VALUE]`, `stats -w`, `driver kernel|legacy`; `--json` for scripts.

Settings changed in the app or with `mctctl set` are remembered in
`~/.config/mct/settings.toml` and restored when the display reconnects (by
the running app, or `mctctl apply`). Writing settings without root needs
the udev rule above and membership in group `video`. More in
[driver/companion-app.md](driver/companion-app.md).

## Debugging

```sh
sudo cat /sys/kernel/debug/dri/*/mct_t6_stats
sudo cat /sys/kernel/debug/dri/*/mct_t2_stats
```

These show the mode, the format in use, update rates, bytes sent, timings
and error counters ([fields](driver/debugfs.md)). The same basics, without
root, are in each device's `mct/` directory in sysfs and in `mctctl info -v`.
The T6 logs its board, firmware, feature level and video memory at probe.

`tools/cursor_latency.sh` and `tools/frame_latency.sh` measure cursor and
frame latency from these statistics while you use the display.
