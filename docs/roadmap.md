# Roadmap and status (2026-10-11, mct_t6 0.21.1)

Where the driver stands against the vendor drivers and the chips, and what is
left to do. Percentages are estimates, not a checklist score.

## Status

### Wacom DTU-1031X (Trigger II backend, `mct_t2.c`)

| Compared to | Estimate | Notes |
|---|---|---|
| Wacom/MCT Windows and macOS driver | ~100% of what they offer | Display, cursor, backlight (the only display setting Wacom's control panel has for this model), suspend, replug. Ahead: tear-free updates, exact diffing, lossless by default (lossy opt-in), recovery from a stuck parser. |
| Trigger II chip in general | ~60% (rough) | Only what Wacom's init uses is known: one fixed 1024x600 panel, one pixel format. Generic T2 dongles (any monitor, mode setting, EDID) need a plain T2 to work on. |
| Speed headroom | ~5-10% | USB 2 link ~40 MB/s, driver at ~38 MB/s. Queued URBs measured without gain; encoding is 2.3 ms per update. |

### T6 USB Station (Trigger 6 backend, `mct_t6.c`)

| Compared to | Estimate | Notes |
|---|---|---|
| Vendor drivers, same device | ~95%, ahead in most areas | Missing: JPEG (only matters for full-screen motion and USB 2). Same: error recovery, debounced hotplug, capability checks, USB 2 handling, cursor up to 128 px. Ahead: up to 144 Hz and 3440x1440@60 / 4K30 (vendor 60 Hz, table modes), lossless / full colour at rest, adaptive format, gamma on any feature level, exact row diffing. |
| Trigger 6 family | ~80% | Not done: second output, HDMI audio, the chip's JPEG decoder. The 5600 is a single-output "Super Lite". Up to 3440x1440@60 (NV12; 4:4:4 up to 51 Hz) and 4K30 (NV12) work. |
| Speed headroom | ~15-25% | Hard limits: ~350 MHz pixel clock (1080p144 max, no 1080p240), ~360 MB/s USB, per-format scanout bandwidth. Raw full-screen NV12 1080p ~98 fps. |

## Work items

Order agreed 2026-10-11:

1. ~~T6 frame-done pacing~~: closed (confirmed with raw frames: done events
   0.1 to 10 ms after the data, phase R = 0.03 against the refresh). Frame-done events mean "frame
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
- **Better T2 lossy mode**: the adaptive tolerance shipped in 0.3.19; left
  are the streaks during heavy motion (per-channel or column-aware
  tolerance).
- **T2 soak test** (reloads, interrupted frames, replug, suspend, DPMS,
  brightness, console, lossy video) and the **`0x0a` gamma table test**
  (`tools/gamma_test.py`, not run yet); see kernel-driver.md.
- **T6 above 1080p:** done (0.18.0): 2560x1440@60 in 4:4:4, 3440x1440@60
  with reduced blanking in NV12, 3440x1440 up to 51 Hz in 4:4:4 (the 4:4:4
  limit is 760 MB/s). The monitor's own 3440x1440@60 timing (319.75 MHz)
  stays off. Buffers are sized per mode
  (~41 MB at 1080p, ~99 MB at 3440x1440). 4K30 (3840x2160, NV12) works too.
- ~~Modes above the monitor's native size~~: hidden since mct_t6 0.22.0,
  `all_modes=1` (NixOS `hardware.mct.allModes`) shows them; tested.
- **Userspace companion app** (design: [companion-app.md](companion-app.md);
  needs per-device sysfs settings in the driver first) (like DisplayLink Manager or the vendors'
  tray apps): settings in one place (format/auto mode, T2 lossy, brightness,
  "show all modes" = all_modes plus a re-probe, modes), and optional extras
  the kernel driver can't carry:
  - **T6 JPEG** for full-screen motion above the USB limit. The chip only
    decodes; the host has to encode, which can't live in the kernel driver
    (no in-kernel JPEG encoder, CPU heavy, wouldn't be accepted upstream).
    MCT's Ubuntu driver does it in userspace: EVDI virtual display +
    libjpeg-turbo. Where it would help on USB 3: full-screen video/games at
    large sizes (3440x1440 NV12 full frames top out around 48 fps; JPEG
    could reach 60). Options: an EVDI-based mode like wacom-t2d, or a path where
    the kernel driver hands frames to a userspace encoder; a GPU JPEG
    encoder through VA-API where available.
  - On USB 2 links, JPEG is what keeps the vendor drivers usable.
- **Other compositors:** KDE/GNOME read the frame from VRAM (~5.8 ms per
  1080p frame). Test them; upstream the niri display-only allocation patch.
- **T6 second output and audio:** needs a 5601 or a dock.
- **Device profiles:** separate Wacom quirks from plain T2 once a plain T2
  dongle is available.

## From the code review of 2026-10-11 (not done yet)

Fixed in mct_t6 0.24.0 / mct_t2 0.3.21: sysfs access during unbind, workers
outliving the device, tiny modes in RGB24, shared T2 encoder scratch, T2
without EDID, runtime-added IDs, the 5601's output and flip type (untested),
hotplug vs. mode set, cursor state locking, events after a failed suspend
or resubmit, device I/O after unplug, unlocked counters, T2 resume and
resync after a failed frame, the pen lookup, backlight name, power flags,
endpoint checks, managed mutexes, DBLSCAN and >255 Hz modes. Left:

- T2 resync (up to 45 s) runs in probe and resume, blocking the USB hub
  thread; move it to a work item.
- Per-mode buffers are allocated in atomic_enable, so a failure can't be
  reported to userspace (display stays dark); allocate in atomic_check or
  preallocate. T6 hw[] copies could be sized per format instead of 4 B/px.
- T2 cursor partly off the left/top edge: whether the device clips or
  wraps is unverified.
- The debugfs inject hook should go behind a debug option; the sysfs files
  are documented in ABI-sysfs-mct.txt (Documentation/ABI format).
- The T2 backlight goes through the pen's HID function behind usbhid's
  back; upstream may want it in the wacom driver.
- Mode filtering removes entries from probed_modes in get_modes; mode_valid
  with a stored native size would be more usual.

## From the edge-case review of 2026-10-11 (not done yet)

Fixed in mct_t6 0.25.0: cursor re-upload after a monitor reconnect or
redraw, EDID read retried after wake-up, mode re-set only when a monitor
really came back (not on HPD blips), monitor re-read after resume, a
preferred mode when none the T6 drives is left (USB 2 + wide monitor, odd
panel sizes), minimum mode size, no recovery on an unplugged device, work
cancelled at release, cursor source offsets refused, software unbind
switches the output off first. Left:

- Per-device settings (format, all_modes, lossy) and console mappings
  start over on every replug, and on every T6 restart after a transfer
  error. The companion app (or a udev rule) could reapply them on bind.
- Connector names (HDMI-A-n) can change on replug while the compositor
  still holds the old device; compositor output settings are better
  matched by make/model/serial.
- T2 resync after a failed frame holds io_lock for up to 45 s, so cursor
  updates in commits and DPMS/suspend wait (same fix as the probe/resume
  item above: a work item).
- The T6 cursor update in a commit waits for io_lock while a frame is
  sent (~16 ms at 1080p on USB 3, longer on USB 2): cursor stutter during
  large updates.
- VT switches to a T6 console are mode sets (fbdev uses the preferred
  mode); with monitors that dislike signal changes, run the compositor at
  the preferred mode and use consoleblank=0.

`tools/mct_edge_test.sh` is the interactive test for these scenarios.

## Rules that stay

- Quality, the colour modes and the auto mode are not to get worse for speed.
- Mainline-friendly: no codecs in the kernel driver.
- One thing talks to a T6 at a time; never USB-reset a stuck chip.
