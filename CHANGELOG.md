# Changelog

The package version (nix/kmod.nix) counts releases of the module as a whole;
each backend has its own DRM driver version (`mct_t2`, `mct_t6`, shown by
drm_info and in the kernel log at probe).

## Unreleased

- T6 frame latency in debugfs: from a frame's first commit until the
  worker starts, and until its flip transfer has completed on the adapter;
  `tools/frame_latency.sh` measures it while something runs (mct_t6
  0.29.11).

## 0.6.1 (2026-10-14)

Driver versions: mct_t6 0.29.10, mct_t2 0.3.37.

**A faster cursor on the Trigger 6**
- Cursor moves no longer wait for a frame being sent: they go over USB's
  control channel with their own buffer and lock. Measured from the
  compositor's commit to the adapter: 0.12-0.4 ms per move, also during
  video and window drags, a small fraction of one refresh at 144 Hz
  (6.9 ms).
- Recent cursor shapes stay in the adapter's cursor slots, so going back to
  one (arrow, text beam, hand) is a single small request instead of a new
  upload of up to 64 KiB: 0.5 ms instead of ~11 ms per shape change.
- debugfs shows the cursor's latency (moves and new images), how many
  updates were replaced before they went out, and the slot cache's hits;
  `tools/cursor_latency.sh` measures it per test phase (both chips).

**Docs**
- The roadmap lists what's open per chip, parked or dropped; finished
  items moved to `docs/history/roadmap-done.md`.
- A Trigger II lab notebook (`docs/history/t2-lab-notebook.md`) holds the
  dated experiments that were in `t2-driver.md` and `protocol-t2.md`.
- Other drivers per chip: `docs/research/t6-other-drivers.md` and a new
  `docs/research/t2-other-drivers.md`.
- Outdated statements fixed across the driver, protocol and comparison
  docs; one sysfs reference (`docs/driver/ABI-sysfs-mct.txt`); the docs
  index lists every page; README: tested modes and other devices up to
  date. T6 JPEG: work in progress, put aside (USB 2 isn't being tested).
- Renamed, so the chip comes first: `reference/init-sequence.md` ->
  `t2-init-sequence.md`, `reference/device-dtu1031x.md` ->
  `t2-device-dtu1031x.md`, `research/driver-analysis.md` ->
  `t2-vendor-driver-analysis.md`, `research/optimization-ideas.md` ->
  `t2-optimization-ideas.md`, `research/wacom-display-settings.md` ->
  `t2-wacom-display-settings.md`, `research/other-linux-drivers.md` ->
  `t6-other-drivers.md`, `history/project-history.md` ->
  `t2-project-history.md`, `history/first-light.md` -> `t2-first-light.md`;
  and `t6-comparison.md` -> `research/t6-vendor-comparison.md`,
  `driver/display-driver.md` -> `driver/wacom-t2d.md`,
  `research/capture.md` -> `research/usb-capture-setup.md`.

**Release workflow**
- One run per tag (the v0.6.0 tag push started two).

## 0.6.0 (2026-10-13)

Driver versions: mct_t6 0.29.7, mct_t2 0.3.36.

**HDMI audio on the Trigger 6** (mct_t6 0.29)
- The adapter is an ALSA sound card ("HDMI", 48 kHz, 16-bit stereo
  playback), tested with `speaker-test` and Spotify through PipeWire, with
  no drift over long playback. Heard while the display is on. Needs a
  kernel with ALSA PCM support; without it the module builds without audio.
- HDMI audio needs at least 112 px of horizontal blanking. The tight
  timings the driver makes for high refresh rates keep as much as fits
  (1080p144: 120 px instead of 80, which buzzed), and the driver warns in
  modes with less.
- The adapter's volume is the mixer control "PCM Playback Volume" with a
  "PCM Playback Switch". "ELD" and jack controls give PipeWire the
  monitor's name and audio formats and whether it is connected.
- `audio_lead_ms` (default 90) sets how far ahead the adapter is fed.
- MCT Display Manager and mctctl show it: sound card and ALSA device,
  state, format, volume and what the monitor takes (sysfs `audio`,
  `audio_volume`, `audio_sink`).

**Trigger 6**
- More USB IDs, untested ones (other adapters, docks, the Wacom
  DTU-1031AX) only with the new module parameter `untested_ids`.
- JPEG: works from userspace (full frames and partial updates) but is
  slower than raw frames on USB 3; not in the driver, work in progress
  ([docs/research/t6-jpeg.md](https://github.com/XXCoreRangerX/mct-linux/blob/main/docs/research/t6-jpeg.md),
  `tools/t6_jpeg_test.py`).

**Trigger II (the tablet)**
- Software gamma (night light).
- The lossy mode's tolerance is per colour channel (green N, red 1.5 N,
  blue 2 N): smaller updates at the same visible error.
- The chip clips the cursor at the right and bottom edges (no re-upload
  of a shifted image on every move there).
- The ID ROM is read: checksum, version, video memory and orientation in
  the kernel log, sysfs (`vram_mb`, `id_rom`) and the app.
- Two more DTU-1031X USB IDs (`0711:5201`, `520f`), driven only after the
  panel's EDID shows it's the Wacom panel, since plain T2 adapters share
  them.
- Experimental, off by default: `motion=half` sends heavy motion at half
  resolution, upscaled by the chip, per 128x16 tile, sharpened when the
  motion stops. Several fixes on the way (stuck blur, still parts
  blurring, a flickering edge band, a photo wallpaper keeping it in half
  mode); it can still glitch at the edges of translucent windows.

**Core**
- No USB I/O and no allocations in atomic commits: the cursor goes out from
  its own worker, and the per-mode buffers are allocated in atomic_check,
  so a lack of memory rejects the mode instead of leaving the display dark.
- Both backends list their modes in sysfs (`modes`); the T2 shows its pixel
  format.

**MCT Display Manager and mctctl**
- A "Modes" list; the T2's pixel format; the "Technical details" sections
  stay open (rows update in place); USB classes written out. mctctl:
  `get modes`, modes in `info -v`.

**Research and tools**
- T2 protocol corrected and extended from the vendor drivers and captures;
  `tools/t2_probe.py` checked upscaling, RGB565, YUV, 512 KiB transfers,
  16 MiB of video memory, cursor clipping and in-band flips on the tablet.
- `tools/t6_probe.py` (audio, info requests), `tools/t6_audio_clock.sh`
  (found the blanking limit), `tools/t6_jpeg_test.py`.
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
