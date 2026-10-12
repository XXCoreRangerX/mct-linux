# Changelog

The package version (nix/kmod.nix) counts releases of the module as a whole;
each backend has its own DRM driver version (`mct_t2`, `mct_t6`, shown by
drm_info and in the kernel log at probe).

## 0.5.2 (unreleased)

- Both backends list the modes they offer in sysfs (`modes`); the T2 shows
  its pixel format (`pixel_format`: RGB24, sent as planar RLE). mct_t6
  0.28.0, mct_t2 0.3.22.
- MCT Display Manager: a "Modes" list in the Display group; the T2's pixel
  format; the "Technical details" sections stay open (rows update in place
  instead of being rebuilt every second); USB classes written out
  ("Vendor specific (0xff)"). mctctl: `get modes`, modes in `info -v`.
- mct_t2 0.3.23: software gamma (night light) on the tablet; the lossy
  mode's tolerance is per colour channel (green N, red 1.5 N, blue 2 N),
  measured offline to give smaller updates at the same visible error.
- mct_t2 0.3.24: `motion=half` sends heavy full-screen updates at half
  resolution, upscaled 2x by the chip (blur instead of streaks), refined
  once motion stops; in the app as "Half resolution during heavy motion".
  0.3.25 fixes it staying pixelated (the motion check now compares with the
  last presented picture); 0.3.26 uses it only when at least 80% of the
  screen changes, so moving a window doesn't blur the still wallpaper;
  0.3.27 measures the real size of what changed instead of the full frame;
  0.3.29 blurs only moving content, per 128x16 tile, and sharpens tiles as
  soon as they stop moving, so still parts stay sharp and the blurred area
  can't grow during a drag; 0.3.30 decodes the half-resolution source in
  spare video memory, so a flip never shows it half-scaled.
- T2 protocol corrected and extended from the vendor drivers and captures;
  hardware upscale, RGB565, YUV 4:2:2 and 512 KiB transfers verified on the
  tablet with the new `tools/t2_probe.py`. Software gamma tested.
- T6 HDMI audio verified with `tools/t6_probe.py` (48 kHz stereo), and
  mct_t6 0.29 adds it as an ALSA sound card ("HDMI", 48 kHz, 16-bit
  stereo playback), tested with `speaker-test` and Spotify through
  PipeWire. It is heard only while the display is on, and only cleanly in
  modes up to 297 MHz pixel clock (1080p120 yes, 1080p144 buzzes; the
  driver logs a warning). `audio_lead_ms` (default 90) sets how far ahead
  the adapter is fed. Needs a kernel with ALSA PCM support; without it the
  module builds without audio.
- README: one status table for both chips; 720p at 240 Hz tested.

## 0.5.1 (2026-10-11)

- MCT Display Manager and mctctl remember each display's settings
  (`~/.config/mct/settings.toml`) and restore them when it reconnects or is
  probed again; displays are recognised by a serial number (the DTU-1031X
  by its pen's) or else the USB port. `mctctl set --temporary`, `mctctl
  apply`, `forget`, `saved`.
- README: "analysis of the vendor drivers for interoperability".

## 0.5.0 (2026-10-11): first public release

Includes MCT Display Manager and mctctl 0.5.0 (the app's version follows
the package from here on).

Driver:

- Linux 6.12 LTS and later: `kernel/mct_compat.h` maps the 7.2 DRM API
  onto older kernels where `Kbuild` finds it missing (atomic state rename,
  vblank timer, fbdev setup, buffer import DMA device, const `mode_valid`).
  Built in CI for 6.12, 6.18 and the latest kernel; tested on 6.12 in a VM
  with the JUA254 passed through (modes, console, mode set, 120 Hz vblank
  and page flips, unload). Nothing changes on 7.2.
- mct_t6 0.22.0: no modes above the monitor's native size unless
  `all_modes=1`.
- mct_t6 0.23.0 / mct_t2 0.3.20: every setting per device in sysfs
  (`format`, `all_modes`, `lossy`), with read-only information next to them;
  the module parameters are the defaults for new devices.
- mct_t6 0.24.0 / mct_t2 0.3.21: fixes from a full code review (sysfs during
  unbind, workers outliving the device, locking, endpoint checks, the
  5601's output, T2 resync and resume, and more; see the roadmap).
- mct_t6 0.25.0: fixes from an edge-case review (cursor after a monitor
  reconnect, EDID retries after wake-up, HPD blips, a preferred mode on USB
  2 with wide monitors, no recovery on an unplugged device); USB 2 links
  and the text console tested on hardware.
- mct_t6 0.27.0: the device's function descriptors (request `b1`), its
  mode table and the driver's clock limits in sysfs (`functions`,
  `device_modes`, `clock_limits`).
- mct_t6 0.26.0: reads the board name, firmware versions and chip serial
  (request `b0`) and shows them in sysfs (`platform`, `board`, `firmware`,
  `serial`, `display_flags`) and the probe log line; all devices get
  `driver_version`.
- MCT Display Manager: names products from the board name or USB IDs
  (j5create JUA254, Wacom DTU-1031X); lists every identifier (product, chip
  and firmware, USB, driver, monitor EDID, the tablet's pen) in the window,
  `mctctl info -v` / `list -v` and the About dialog's Troubleshooting report;
  About with the project's links and credits; the tray icon waits for a tray
  host that starts later. `-vv` (and the window's collapsed "Technical
  details") adds the adapter's internals, USB descriptors and endpoints,
  the decoded and raw EDID, and driver details.

App: MCT Display Manager (GTK 4 / libadwaita window and tray icon) and
`mctctl`: per-device settings, live status, warnings (T6 on USB 2), the
DTU-1031X's switch between the kernel driver and wacom-t2d.

Project: GPL-2.0-only code, CC BY-SA 4.0 documentation; `t2d` renamed
`wacom-t2d`; docs sorted into reference, driver, research and history with
a status per file; releases with the driver as DKMS source and the app for
x86_64 and aarch64; NixOS options `allModes`, `app.enable`, `app.tray`.

## 0.4.0 (2026-10-11): one module, mct

- mct_main.c matches all devices and hands them to a backend; a shared DRM
  core (mct_drv.c) does planes, CRTC, connector, damage copies, workers,
  stats, power management for both chips.
- mct_t6 0.17 to 0.21:
  - modes beyond the device's table: 2560x1440, 3440x1440@60 (reduced
    blanking), 3840x2160@30; fallbacks for monitors whose own timing is out
    of reach, 16:9 monitors too tall, no EDID; CVT reduced blanking where it
    fits; the monitor's pixel clock and line rate limits
  - buffers sized per mode
  - recovery from transfer errors (cancel request), debounced hotplug,
    redraw requests, event endpoint recovery, capability checks, USB 2
    links
  - cursor up to 128 px, native negative positions
  - YUV 4:4:4 scanout limit measured at 760 MB/s
- mct_t2 0.3.19 (Wacom DTU-1031X): unchanged protocol side; on the shared
  core it gets the row-exact damage copy.

## Earlier

The T2 driver's history (0.1 to 0.3.19: first light, trimmed init, partial
updates, cursor, backlight, lossy mode, recovery, counters) is in
docs/driver/t2-driver.md and docs/history/project-history.md; the T6's (0.1 to 0.17)
in docs/history/t6-lab-notebook.md.
