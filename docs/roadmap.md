# Roadmap and status

> **Status**: updated as work goes on (last on 2026-10-14, at mct_t6 0.29.8 and mct_t2 0.3.36).

Where the driver stands against the vendor drivers and the chips, and what is
left to do. Percentages are estimates, not a checklist score. Finished items
are moved, unchanged, to
[history/roadmap-done.md](history/roadmap-done.md).

Never send: T2 `0x1a`, `0x1c`; T6 `0x01`, `0x13`, `0x30`, `0x15`, `0x33`,
`0x40`, bulk signature 5; no USB resets (see "Rules that stay").

## Status

### Wacom DTU-1031X (Trigger II backend, `mct_t2.c`)

| Compared to | Estimate | Notes |
|---|---|---|
| Wacom/MCT Windows and macOS driver | ~100% of what they offer | Display, cursor, backlight (the only display setting Wacom's control panel has for this model), suspend, replug. Ahead: tear-free updates, exact diffing, lossless by default (lossy opt-in), recovery from a stuck parser. |
| Trigger II chip in general | ~60% (rough) | The driver drives one fixed 1024x600 panel in one pixel format. The timing registers, the PLL, other source formats and scaling are known since 2026-10-12 ([protocol-t2.md](reference/protocol-t2.md)), but generic T2 dongles (any monitor, mode setting, EDID) need a plain T2 to test on. |
| Speed headroom | ~5-10% | USB 2 link ~40 MB/s, driver at ~38 MB/s. Queued URBs measured without gain; encoding is 2.3 ms per update; 512 KiB transfers would add ~3%. |

### T6 USB Station (Trigger 6 backend, `mct_t6.c`)

| Compared to | Estimate | Notes |
|---|---|---|
| Vendor drivers, same device | ~95%, ahead in most areas | Missing: JPEG (only matters for full-screen motion and USB 2). Same: error recovery, debounced hotplug, capability checks, USB 2 handling, cursor up to 128 px, HDMI audio (0.29: 48 kHz stereo, volume, ELD and jack controls). Ahead: up to 144 Hz and 3440x1440@60 / 4K30 (vendor 60 Hz, table modes), lossless / full colour at rest, adaptive format, gamma on any feature level, exact row diffing. |
| Trigger 6 family | ~80% | Not done: second output, the chip's JPEG decoder. The 5600 is a single-output "Super Lite". Up to 3440x1440@60 (NV12; 4:4:4 up to 51 Hz) and 4K30 (NV12) work. |
| Speed headroom | ~15-25% | Hard limits: ~350 MHz pixel clock (1080p144 max, no 1080p240), ~360 MB/s USB, per-format scanout bandwidth. Raw full-screen NV12 1080p ~98 fps. |

## Open

### Trigger 6

- **Test the cursor of mct_t6 0.29.8**: its requests no longer wait for
  `io_lock`, so it should stay smooth during large updates.
- **HDMI audio while the display is off**: playback runs but nothing is
  heard, since HDMI carries audio in the video signal.
- **Second output**: needs a 5601 or a dock. The protocol can be worked out
  offline from cyrozap's JUA365/JCD543 dual-output captures first (plan an
  idle keepalive, as two other projects needed for the 5601). With it: pick
  the output on a change event instead of once at probe
  ([t6-vendor-review.md](research/t6-vendor-review.md) item 11).
- **Probes not run yet**: windowed flip (header bytes 0x24-0x2b, letterbox
  without a mode set), the YUV 4:4:4 surface formats 0x11/0x12/0x15, and the
  timing entry's flag bit 0 "custom timing" (Tetra driver; mct sends 0).
- **Mode table read**: vendors read count x 32 bytes (count from `0x84`);
  mct reads exactly 960 ([t6-vendor-review.md](research/t6-vendor-review.md)
  item 9).
- **`hw[]` buffer copies** could be sized per format instead of 4 bytes per
  pixel.

### Trigger II (the tablet)

- **Rainbow lines** (analog sampling phase or a DDR/DVO clock phase, ~10-25%
  of inits): look for an auto-adjust/phase command for the RTD2280 scaler,
  reachable through the pen HID side (Wacom's driver has none,
  [t2-wacom-display-settings.md](research/t2-wacom-display-settings.md)); read the
  unknown pen feature reports (read-only, needs an OK); reverse the macOS
  driver's `T2_startDDRclock` and DRAM phase fallback
  ([t6-vendor-review.md](research/t6-vendor-review.md) item 16).
- **Resync as a work item**: the resync after an interrupted payload (up to
  45 s) runs in probe and resume, blocking the USB hub thread. After a
  failed frame it holds `io_lock` that long: commits no longer wait (the
  cursor goes out from the core's worker), but the cursor worker and
  DPMS/suspend do.
- **Soak test**: reloads, interrupted frames, replug, suspend, DPMS,
  brightness, console, lossy video.
- **Lossy streaks** during heavy motion: column-aware tolerance (the
  per-channel tolerance is in since 0.3.23).
- **Hardware features from the vendor drivers**, not used yet: 512 KiB
  payload transfers (~3%, needs a 512 KiB DMA buffer); YUV 4:2:2 RLE as
  another motion mode (a third less raw data, colour smears at edges); the
  blit engine for scrolling (highest gain, highest risk, untested); a
  background refresh like the macOS driver's (every tile once per 10 s,
  host side only); the second address set (an overlay?); a second cursor
  shape in VRAM (showed garbage on 2026-10-13, address unit not understood).
- **Cost-based damage-band merging** instead of the fixed 16-row gap
  (modest gain: desktop traffic is already low).
- **Device profile**: move the DTU-1031X's constants into a profile struct
  in `mct_t2.c`, and separate the Wacom quirks from plain T2 once a plain T2
  dongle is available.
- **Backlight**: it goes through the pen's HID function behind usbhid's
  back; upstream may want it in the wacom driver. Optional: start at level
  6 like Wacom.

### Core, upstream and compositors

- **Upstream readiness**: checkpatch, sparse, KUnit tests for the PLL,
  colour conversion and RLE, a CI build against drm-next. The review points
  raised on drm/trigger5 and appletbdrm are done (no USB I/O or allocations
  in commits; see [shared-core.md](driver/shared-core.md)). Thomas
  Zimmermann's damage-helper rework may change how `mct_damage_rows()` must
  read clips.
- **debugfs inject hook** behind a debug option.
- **Mode filtering**: entries are removed from `probed_modes` in
  `get_modes`; `mode_valid` with a stored native size would be more usual.
- **Trigger 5**: a driver is under review upstream ("drm/trigger5",
  2026-09); contact its author rather than duplicating it.
- **niri patches**: upstream the two (display-only allocation, removed
  devices).
- **Other compositors**: KDE and GNOME read the frame from VRAM (~5.8 ms
  per 1080p frame). Test them.
- **Replug**: console mappings start over. Connector names (HDMI-A-n) can
  change while the compositor still holds the old device, so compositor
  output settings are better matched by make/model/serial.
- **VT switches** to a T6 console are mode sets (fbdev uses the preferred
  mode); with monitors that dislike signal changes, run the compositor at
  the preferred mode and use consoleblank=0.
- **Outside the kernel** (prepared 2026-10-13, not sent): usb.ids entries
  (optional); a libwacom entry for the DTU-1031AX waits for someone with the
  device.

`tools/mct_edge_test.sh` is the interactive test for the replug, monitor and
console scenarios.

### Packaging

- **Distribution packages**: AUR, Fedora COPR, a .deb.

## Parked or dropped

- **T6 JPEG**: work in progress, put aside (USB 2 isn't being tested).
  Works from userspace, slower than raw frames on USB 3; see
  [t6-jpeg.md](research/t6-jpeg.md).
- **T2 half-resolution motion** (`motion=half`): experimental and off by
  default since 2026-10-13; the edges of translucent windows can still
  glitch.
- **T6 hardware rotation** (`40 1f`): the 5600 stalls on it; the code is on
  branch `t6-rotation`. Software rotation was dropped too: compositors
  already rotate.
- **DDC/CI through the T6**: the JUA254H firmware has no case for the I2C
  requests.
- **T6 frame-done pacing**: the events mean "frame processed", not vblank;
  pacing stays on the timer.
- **T2 `0x0a` table as hardware gamma**: no visible effect, and it upsets
  the chip until the next init (2026-10-13). Software gamma is in since
  0.3.23.
- **T2 EP2 timeout as a recovery**: it ends a stuck command but leaves the
  stream out of step (2026-10-13).
- **T2 queued URBs**: no gain (branch `async-urbs`).
- **libwacom size patch for the DTU-1031X**: pen configuration isn't this
  project's scope.

## Rules that stay

- Quality, the colour modes and the auto mode are not to get worse for speed.
- Mainline-friendly: no codecs in the kernel driver.
- One thing talks to a T6 at a time; never USB-reset a stuck chip.
- Never send requests that write flash or EEPROM or reset the chip: T2
  `0x1a`, `0x1c`; T6 `0x01`, `0x13`, `0x30`, `0x15` (test mode), `0x33`
  (serial write), `0x40` (internal message), bulk signature 5.
