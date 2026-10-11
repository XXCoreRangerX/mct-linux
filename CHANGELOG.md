# Changelog

The package version (nix/kmod.nix) counts releases of the module as a whole;
each backend has its own DRM driver version (`mct_t2`, `mct_t6`, shown by
drm_info and in the kernel log at probe).

## Unreleased

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
