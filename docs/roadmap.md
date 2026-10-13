# Roadmap and status (2026-10-13, mct_t6 0.29.3, mct_t2 0.3.31)

> **Status**: updated as work goes on.

Where the driver stands against the vendor drivers and the chips, and what is
left to do. Percentages are estimates, not a checklist score.

## Next up (2026-10-13)

In order. First the test queue (needs the user and the devices), then the
work from the research of 2026-10-12/13. Sources are lab reports in
`captures/`; details in the numbered items further down.

### 1. Test queue (built, not yet tested)

| # | Device | Test | Expect |
|---|---|---|---|
| 1 | both | rebuild, load with `insmod` from `/run/current-system` | mct_t6 0.29.3, mct_t2 0.3.31. **Done** |
| 2 | T6 + LG | 1080p144 (now 120 px blanking): picture, then `speaker-test` | stable picture, clean tone. **Done 2026-10-13: pass** |
| 3 | T6 + LG | volume: `amixer -c JUA254H set PCM 31 / 15 / 0`, mute/unmute | 15 clearly quieter, 0 silent (first `40 23` on the 5600). **Done 2026-10-13: pass** |
| 4 | T6 | long playback (an hour of music) | no crackle or growing delay (drift). **Done 2026-10-13: pass** |
| 5 | tablet | half-resolution motion: drag windows | no flickering band at the edges (0.3.31 fix). **2026-10-13: still flickers; 0.3.32 (hysteresis): gone, but stuck in half mode on a photo wallpaper; 0.3.33: fixed, but parts of translucent windows vanish near edges again. Half mode stays experimental and off; parked** |
| 6 | tablet | `tools/t2_probe.py --steps q,r,s,t,u,v,x`, then `--steps w` alone | **Done 2026-10-13 except w**: q no (and it upsets the chip until re-init), r scaler fine and no done-signal, s 16 MiB yes, t off works / on needs `fb60=01`, u clipping yes / second shape no, v yes, x no effect (protocol-t2.md) |

### 2. Then, by value

| # | Work | From | Size |
|---|---|---|---|
| 1 | **Done 2026-10-13** (cursor clipping in 0.3.34; `0x16` flips add nothing, the `0x15` flip already goes in-band on EP2; the 16 MiB layout only mattered for half mode). Was: act on the probe results: 16 MiB layout (s: room for per-buffer scratch sources and staging), hardware cursor clipping (u3-u5, drop the shifted re-upload), in-band `0x16` flips (v), ~~timeout recovery (w, still to run)~~ (w: ends a stuck command in 307 ms at `0x1000` but leaves the stream out of step, so not a recovery on its own); ~~find the half-mode edge band~~ (parked 2026-10-13: half mode stays experimental and off); follow-ups: t1b with `fb60=01`, the second cursor shape's address unit | `captures/t2-research/scaler-sync.md`, `wacom-and-t2-devices.md` | M |
| 2 | **Done 2026-10-13 (untested):** T2 `5201`/`520f` with a Wacom-EDID check before the panel init; T6 IDs behind `untested_ids`; DTU-1031AX panel-only modes. Was: more USB IDs: T2 `0711:5200-520f` (DTU-1031X seen as `5201`, `520f`), identify by ID ROM/EDID; T6 PID range + interface class (`0711:5600-560b`, `19ff:560b-560e`, `03f0:0182`), untested ones opt-in; DTU-1031AX (`0711:5609`, maybe `056a:039f` interface 0) with its quirk (no rotation, EDID's own mode only) | `captures/scout/web-research-2026-10-13.md`, `captures/t2-research/wacom-and-t2-devices.md` | S |
| 3 | **Done 2026-10-13 (0.29.5, tested):** ELD and jack controls (LG: ELD names "27GN7", LPCM 2 ch 32/44.1/48 kHz; jack on). Was: HDMI audio extras: ELD and jack controls from the connector's EDID; optional short prefill on start | `captures/t6-research/audio-analysis.md` | S |
| 4 | **Done 2026-10-13 (0.3.36; the EDID was already used):** checksum, version, video memory, rotation and base USB ID logged and in sysfs/app. Was: T2: read the ID ROM properly (checksum bytes 4/5, version 0x16-0x18, GPIO map 0x1a-0x1f, 180° flag, EDID) | `wacom-and-t2-devices.md` | S-M |
| 5 | ~~T6 software rotation in the conversion step~~ **Dropped 2026-10-13**: compositors already rotate (niri renders the portrait T6 screen turned; the driver sends what it gets), so it would only matter for one that asks the display to rotate. Was: (no hardware rotation in this firmware) | `captures/t6-research/feature-gaps.md` | M |
| 6 | **Read-only part done 2026-10-13 (step F):** all answer, nothing new beyond what the driver reads (protocol-t6.md). Was: T6 probes: read-only info `c0 b2` (serial, MAC), `c0 8a`, `c0 83`, `c0 41`; windowed flip (bytes 0x24-0x2b, letterbox without a mode set); YUV444 surface formats 0x11/0x12/0x15; timing flag bit 0 "custom" | `feature-gaps.md`, web research | S each |
| 7 | **Done 2026-10-13 (to test):** cursor I/O moved to a worker, per-mode buffers allocated in atomic_check; damage and USB buffers checked (shared-core.md). Was: upstream readiness against what reviewers asked of drm/trigger5 and appletbdrm (no sleeping USB I/O in atomic paths, no allocations in the update path, damage clips in framebuffer coordinates, the damage-helper rework) | web research | M |
| 8 | T6 JPEG path, phase 0: measure the JUA254's decode speed from userspace; then decide (USB 2 / hubs only, unless it beats raw). Recipe: lvalen91/JUA254H---Fedora, Tetra (FB_DIRECT partials, not CLIP_PRIMARY); our NV12 UV plane must start at pitch x align16(h) for the decoder | `captures/t6-research/jpeg-path.md`, `third_party/t6-sources/web-2026-10-13/` | S, then L |
| 9 | Upstream submissions outside the kernel: libwacom DTU-1031X size 223 x 125 mm and a DTU-1031AX entry; usb.ids entries | `wacom-and-t2-devices.md` | S |

Never send: T2 `0x1a`, `0x1c`; T6 `0x01`, `0x13`, `0x30`, `0x15`, `0x33`,
`0x40`, bulk signature 5; no USB resets (see "Rules that stay").

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
| Trigger 6 family | ~80% | Not done: second output, HDMI audio (the 5600 most likely has it), the chip's JPEG decoder. The 5600 is a single-output "Super Lite". Up to 3440x1440@60 (NV12; 4:4:4 up to 51 Hz) and 4K30 (NV12) work. |
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
3. ~~Shared core refactor~~: done, see [shared-core.md](driver/shared-core.md).
   Left: the DTU-1031X profile struct in `mct_t2.c`.
4. **T2 rainbow lines** (analog sampling phase, ~10-25% of inits): look for an
   auto-adjust/phase command for the RTD2280 scaler, reachable through the pen
   HID side. Wacom's driver has none ([wacom-display-settings.md](research/wacom-display-settings.md));
   next is reading the unknown pen feature reports (read-only, needs an OK).
5. **T2 hardware features found in the vendor drivers (2026-10-12)**; all
   but the blit engine tested OK with `tools/t2_probe.py` the same day.
   Next: use them in mct_t2:
   - 512 KiB payload transfers like Windows (verified; the measured gain is
     ~3% and needs a 512 KiB DMA buffer, so not done yet);
   - ~~2x hardware upscale during heavy motion~~: done as `motion=half`,
     since 0.3.29 per tile, only for moving content (probe steps j and k
     passed);
   - YUV 4:2:2 RLE (a third less raw data; colour smears at edges) as
     another motion mode; RGB565 only helps noisy content;
   - the blit engine for scrolling and window moves (highest gain for
     desktop use, highest risk; the older vendor driver itself would not use
     it on this tablet, so firmware support is unknown; tested last, after
     the other probe steps, see protocol-t2.md).
   From other vendors' T2 drivers (2026-10-12, lab
   `captures/t2-research/other-t2-drivers.md`): read the 180 degree flag and
   the EDID source from the ID ROM instead of hard-coding them; use the
   16 MiB of video memory; the macOS driver's power states (`fcb0`,
   `fc6f`); several cursor shapes in VRAM (`fe30-33`) and its edge clipping;
   the EP2 timeout (`0x90` register 4) so a cut-off frame heals by itself;
   a background refresh like the macOS driver's (every tile once per 10 s);
   in-band `0x16` flips; the second address set (overlay?). Probe steps s
   onward test them.
   Also next (2026-10-12): a "scaler done" signal so half mode's edges stop
   glitching (wait before the flip), and the `0x0a` table as hardware gamma
   (it recolours all output when non-zero). Then decide which go into mct_t2. Also to test on the tablet: software
   gamma and the per-channel lossy tolerance (mct_t2 0.3.23).

## Candidates from the scout (2026-10-12)

A read-only research pass (lab `captures/scout/todo-candidates.md`: every
candidate with evidence, effort S/M/L, hardware risk and whether it needs
hardware we don't have, plus a "not recommended" list: drm_panic, VRR, HDR,
writeback, a T5 backend now). Its top 10, not yet ordered with the user:

1. **T6 HDMI audio.** ~~The JUA254 most likely has it~~: it works
   (2026-10-12, `tools/t6_probe.py`, 48 kHz stereo heard on the LG's
   jack). ~~Next: ALSA playback in the module (L), sharing the bulk endpoint
   with the frames.~~ EDIT (2026-10-12): in mct_t6 0.29, working (~~clean up
   to 297 MHz pixel clock; above, it buzzes, which looks like a firmware
   limit~~ EDIT (2026-10-12): it needs 112 px of horizontal blanking; the
   driver's modes now keep that where they can).
   Left: ~~the volume control (node 0, `40 23`)~~ (EDIT 2026-10-12: in
   0.29.3, to test), ~~drift correction if the adapter sends audio events~~
   (EDIT 2026-10-12: it doesn't report progress, and no vendor corrects
   drift), and starting audio when the display is off. Possible: ELD and
   jack controls from the connector's EDID.
2. ~~**DDC/CI through the T6** (firmware I2C requests `0x81`/`0x02`): monitor
   brightness and input from ddcutil. Reads first, on the LG only (S to M).~~
   EDIT (2026-10-13): closed. The JUA254H firmware has no case for these
   requests (lab `captures/t6-research/feature-gaps.md`).
   ~~Instead: **software rotation** in mct_t6's conversion step (the T6
   firmware has no hardware rotation, and the vendors rotate in software
   or leave it to the OS), useful for portrait monitors (M).~~ EDIT
   (2026-10-13): dropped, compositors already rotate (the user's T6
   monitor runs in portrait through niri).
3. **Cursor independent of `io_lock`:** no cursor stutter during large
   updates (S to M; already in the edge-case list below).
4. **T2 resync as a work item** (M; below).
5. **T6 buffers allocated in `atomic_check`** (M; below; also what upstream
   review asked of drm/trigger5).
6. **Upstream readiness:** checkpatch, sparse, KUnit tests for the PLL,
   colour conversion and RLE, a CI build against drm-next (M). EDIT
   (2026-10-13): review points raised on drm/trigger5 and appletbdrm: no
   sleeping USB I/O in atomic_update, atomic_enable or commit_tail; no
   allocations in the update path; damage clips in framebuffer
   coordinates. Thomas Zimmermann's damage-helper rework may change how
   mct_damage_rows() must read clips (lab
   `captures/scout/web-research-2026-10-13.md`).
7. **Distribution packages:** AUR, Fedora COPR, a .deb (S to M).
8. **More T6 USB IDs behind feature checks** (untested ones opt-in), and
   the Wacom DTU-1031AX, a T6 device with ~~an unknown USB ID~~ EDIT
   (2026-10-13): USB ID `0711:5609` (MCT's firmware image and `t6sta.inf`;
   T6-688SL in the FCC photos; pen `056a:039f`). j5create's macOS driver
   turns off rotation and the EDID's extra modes for it (lab
   `captures/t2-research/wacom-and-t2-devices.md`) (S).
   EDIT (2026-10-13), more IDs (lab `captures/scout/web-research-2026-10-13.md`,
   checked against the sources):
   - T2: the DTU-1031X is also `0711:5201` and `0711:520f` (Linux Hardware
     Database); mct_t2 matches only `5209`/`520d`. The PID comes from the
     ID ROM, so match `0x5200-0x520f` and identify the device by ROM/EDID.
   - T6: the firmware sets the PID and it changes with reflashing (the
     U3-A8613 is `5606` with 2019 firmware, `5601` with 2024). Seen:
     `0711:5600-5609`, `0711:560b` (Mobile Pixels Trio Max, Tetra),
     `19ff:560b-560e` (Insignia, Platinum), `03f0:0182` (probably the
     HP Engage 6.6" display). The DTU-1031AX's display driver binds
     `056a:039f` interface 0 (TrgIdd64.inf), so the retail tablet may be
     a single Wacom-ID device. Match a PID range plus interface class.
   - Experiment: timing entry flag bit 0 = "custom timing" (Tetra driver,
     MIT); mct sends 0. Possibly relevant for modes not in the chip's
     table.
9. **Upstream the two niri patches** (S, review time).
10. **The userspace JPEG path** (L): measure decode speed first; it helps
    ~~full-screen motion at large sizes and~~ USB 2 links (and shared hubs).
    EDIT (2026-10-12): research (lab `captures/t6-research/jpeg-path.md`):
    larger modes are blocked by the ~340 MHz pixel clock, not USB; the
    decoder looks like ~100-135 Mpx/s (Windows capture of a 5601), slower
    than raw NV12 over USB 3 at large sizes. Plan: off-by-default helper
    interface, kernel keeps policy and falls back to raw; phase 0 is a decode
    speed measurement on the JUA254.

Also new: the T6 second output can be worked out offline from cyrozap's
JUA365/JCD543 captures (plan an idle keepalive, as two other projects needed
for the 5601), and a Trigger 5 driver is under review upstream ("drm/trigger5",
2026-09); contact its author rather than duplicating it.

Later:

- **Wacom control panel display settings:** researched; brightness is the
  only one and the driver has it. Optional: start at level 6 like Wacom.
- **Better T2 lossy mode**: the adaptive tolerance shipped in 0.3.19; left
  are the streaks during heavy motion (per-channel or column-aware
  tolerance).
- **T2 soak test** (reloads, interrupted frames, replug, suspend, DPMS,
  brightness, console, lossy video) and the **`0x0a` gamma table test**
  (`tools/gamma_test.py`, not run yet); see t2-driver.md.
- **T6 above 1080p:** done (0.18.0): 2560x1440@60 in 4:4:4, 3440x1440@60
  with reduced blanking in NV12, 3440x1440 up to 51 Hz in 4:4:4 (the 4:4:4
  limit is 760 MB/s). The monitor's own 3440x1440@60 timing (319.75 MHz)
  stays off. Buffers are sized per mode
  (~41 MB at 1080p, ~99 MB at 3440x1440). 4K30 (3840x2160, NV12) works too.
- ~~Modes above the monitor's native size~~: hidden since mct_t6 0.22.0,
  `all_modes=1` (NixOS `hardware.mct.allModes`) shows them; tested.
- ~~Userspace companion app~~: built (`app/`: MCT Display Manager window
  and tray, `mctctl`; [companion-app.md](driver/companion-app.md)), on
  per-device sysfs settings (mct_t6 0.23.0). It shows every identifier
  (board name, firmware, USB, monitor EDID) since mct_t6 0.26.0, and
  restores saved settings on replug since 0.5.1. Left: optional extras the
  kernel driver can't carry:
  - **T6 JPEG** for full-screen motion above the USB limit. The chip only
    decodes; the host has to encode, which can't live in the kernel driver
    (no in-kernel JPEG encoder, CPU heavy, wouldn't be accepted upstream).
    MCT's Ubuntu driver does it in userspace: EVDI virtual display +
    libjpeg-turbo. ~~Where it would help on USB 3: full-screen video/games at
    large sizes (3440x1440 NV12 full frames top out around 48 fps; JPEG
    could reach 60).~~ EDIT (2026-10-12): probably not on USB 3: the
    decoder (~100-135 Mpx/s estimated) is slower than raw NV12 there at
    large sizes, and modes above ~340 MHz are out of reach anyway; see
    item 10. Options: an EVDI-based mode like wacom-t2d, or a path where
    the kernel driver hands frames to a userspace encoder; a GPU JPEG
    encoder through VA-API where available.
  - On USB 2 links, JPEG is what keeps the vendor drivers usable.
- **Other compositors:** KDE/GNOME read the frame from VRAM (~5.8 ms per
  1080p frame). Test them; upstream the niri display-only allocation patch.
- **T6 second output:** needs a 5601 or a dock; the protocol can be worked
  out offline from cyrozap's dual-output captures first.
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

- ~~Per-device settings start over on every replug~~: the companion app
  saves them and restores them when a display appears (0.5.1). Console
  mappings still start over.
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

Fixed in mct_t6 0.26.0: a monitor that stops answering EDID reads while
still connected (seen once with the LG: the mode list fell back to the
adapter's 60 Hz table) keeps the modes of the last EDID read; the read is
tried 5 times, 200 ms apart, and a failure is logged.

## Rules that stay

- Quality, the colour modes and the auto mode are not to get worse for speed.
- Mainline-friendly: no codecs in the kernel driver.
- One thing talks to a T6 at a time; never USB-reset a stuck chip.
- Never send requests that write flash or EEPROM or reset the chip: T2
  `0x1a`, `0x1c`; T6 `0x01`, `0x13`, `0x30`, `0x15` (test mode), `0x33`
  (serial write), `0x40` (internal message), bulk signature 5.
