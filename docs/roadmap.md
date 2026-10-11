# Roadmap and status (2026-10-11)

Where the driver stands against the vendor drivers and the chips, and what is
left to do. Percentages are estimates, not a checklist score.

## Status

### Wacom DTU-1031X (Trigger II backend, `mct_t2.c`)

| Compared to | Estimate | Notes |
|---|---|---|
| Wacom/MCT Windows and macOS driver | ~95% | Display, cursor, backlight, suspend, replug all there. Ahead: tear-free updates, exact diffing, lossless by default (lossy opt-in), recovery from a stuck parser. Missing: the display settings of the Wacom control panel (not mapped yet). |
| Trigger II chip in general | ~60% (rough) | Only what Wacom's init uses is known: one fixed 1024x600 panel, one pixel format. Generic T2 dongles (any monitor, mode setting, EDID) need a plain T2 to work on. |
| Speed headroom | ~5-10% | USB 2 link ~40 MB/s, driver at ~38 MB/s. Queued URBs measured without gain; encoding is 2.3 ms per update. |

### T6 USB Station (Trigger 6 backend, `mct_t6.c`)

| Compared to | Estimate | Notes |
|---|---|---|
| Vendor drivers, same device | ~90%, ahead in many areas | Missing: JPEG (hardware rotation: the 5600 doesn't have it). Ahead: up to 144 Hz (vendor 60), lossless / full colour at rest, adaptive format, gamma, queued URBs, exact row diffing. |
| Trigger 6 family | ~75% | Not done: second output, HDMI audio, the chip's JPEG decoder. The 5600 is a single-output "Super Lite". Up to 3440x1440@60 (NV12; 4:4:4 at 55 Hz) and 4K30 (NV12) work. |
| Speed headroom | ~15-25% | Hard limits: ~350 MHz pixel clock (1080p144 max, no 1080p240), ~360 MB/s USB, per-format scanout bandwidth. Raw full-screen NV12 1080p ~98 fps. |

## Work items

Order agreed 2026-10-11:

1. ~~T6 frame-done pacing~~: closed. Frame-done events mean "frame
   processed", not vblank (vendor capture: phase spread R = 0.11), and with
   the driver's frame recipe the chip sends none at all
   (`tools/t6_pacing_test.py`: 0 events for 223 frames). Pacing stays on
   the timer.
2. ~~T6 hardware rotation~~ (`40 1f`): the 5600 stalls on the request, like
   on gamma (feature level 6), and the Windows driver never sends it. The
   driver code is kept on branch `t6-rotation` for a chip that has it.
3. ~~Shared core refactor~~: done, see [shared-core.md](shared-core.md).
   Left: the DTU-1031X profile struct in `mct_t2.c`.
4. **T2 rainbow lines** (analog sampling phase, ~10-25% of inits): look for an
   auto-adjust/phase command for the RTD2280 scaler, reachable through the pen
   HID side. Wacom's driver has none ([wacom-display-settings.md](wacom-display-settings.md));
   next is reading the unknown pen feature reports (read-only, needs an OK).

Later:

- **Wacom control panel display settings:** researched; brightness is the
  only one and the driver has it. Optional: start at level 6 like Wacom.
- **Better T2 lossy mode** (streaks during heavy motion; per-channel or
  column-aware tolerance).
- **T6 above 1080p:** done (0.18.0): 2560x1440@60 in 4:4:4, 3440x1440@60
  with reduced blanking in NV12, 3440x1440@50 in 4:4:4. The monitor's own
  3440x1440@60 timing (319.75 MHz) stays off. Buffers are sized per mode
  (~41 MB at 1080p, ~99 MB at 3440x1440). 4K30 (3840x2160, NV12) works too.
- **T6 JPEG** for full-screen motion above the USB limit: not in the kernel
  driver (no in-kernel encoder, would not be accepted upstream). Possible as a
  userspace program (EVDI + encoder, like t2d), optionally using a GPU JPEG
  encoder through VA-API. The chip only decodes.
- **Other compositors:** KDE/GNOME read the frame from VRAM (~5.8 ms per
  1080p frame). Test them; upstream the niri display-only allocation patch.
- **T6 second output and audio:** needs a 5601 or a dock.
- **Device profiles:** separate Wacom quirks from plain T2 once a plain T2
  dongle is available.

## Rules that stay

- Quality, the colour modes and the auto mode are not to get worse for speed.
- Mainline-friendly: no codecs in the kernel driver.
- One thing talks to a T6 at a time; never USB-reset a stuck chip.
