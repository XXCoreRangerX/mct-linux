# Trigger II lab notebook

> **History**: a chronological record. Entries are not rewritten; later findings are added after them. For the current state see the reference docs ([docs/README.md](../README.md)).

Running log of the Trigger II work, newest at the bottom. Protocol facts
that hold up go into [protocol-t2.md](../reference/protocol-t2.md), the
backend is described in [t2-driver.md](../driver/t2-driver.md), and open
work is in the [roadmap](../roadmap.md).

This notebook was started on 2026-10-14 by moving, unchanged, the dated lab
narratives that had grown inside the reference docs; each moved block says
where it came from. Moved sections keep their headings, so a reference such
as protocol-t2.md, "Tested on the tablet 2026-10-13", now means the heading
of that name here. The earlier history is in
[t2-first-light.md](t2-first-light.md) (the first picture, 2026-09-24) and
[t2-project-history.md](t2-project-history.md) (from the first probes to
the kernel driver, with the commit ledger).

## 2026-09-24

*(Moved from reference/protocol-t2.md, "Gaps (resolved by the usbredir capture, kept for history)", on 2026-10-14.)*

### Gaps (resolved by the usbredir capture, kept for history)

- `0x13` header tail (complen+magic, bytes 30-35), candidate: complen=rawlen,
  magic `30 05 28`; **verify live on tablet**.
- `0x22` tail (30/62 B) + `0x04` 43 B tail (11 B), skipped in first-light
  trial; reconstruct from driver if the panel needs them.
- RLE payload bytes, not needed if RGB-raw path works.

## 2026-09-25

### Recovery from an interrupted payload: soak (0.3.13 to 0.3.17)

*(Moved from driver/t2-driver.md, "Updating the module without a reboot", on 2026-10-14.)*

`tools/t2_recovery_test.py` repeats this: it unbinds the driver, sends a
full-screen noise frame header plus a random 10-90% of its payload from
userspace, binds again and checks the log and the connector. It always
rebinds on exit. Soak results on 0.3.13: 36 of 37 interrupted-frame cycles
recovered, and 8 manual padding replays from userspace saw no timeouts.
The one failure was a padding chunk that itself timed out: probe gave up with
-110 and the tablet needed a manual rebind. 0.3.14 treats a timed-out padding
chunk like a missing reply and keeps going, but its 15 s deadline was still
too short (third cycle of its soak).

Why chunks stall (verified 2026-09-25 from userspace): the zeros replace the
lost rest of the frame, and each zero decodes to one pixel. Unless the lost
part had no RLE runs at all (pure noise), the padded frame ends with the wrong
pixel count. The firmware then stops taking data and answers the queued read
only after an internal timeout of about 15-30 s. Padding with runs
(`30 fa 00`) instead of zeros stalls the same way, and sending exactly the
missing byte count still takes ~14 s. A USB reset while it is stuck is worse:
the chip ignores control requests too, enumeration fails, and only a port
power cycle or a replug brings it back. So 0.3.17 simply waits, with a 45 s
deadline. This only happens after a driver was stopped mid-frame; normal
frames of any length are unaffected.

### Development roadmap

*(Moved from driver/t2-driver.md, "Development roadmap", on 2026-10-14.)*

(As of 2026-09-25; the current list is in [roadmap.md](../roadmap.md).)

Only the Wacom DTU-1031X is presently available for live testing. Make its
driver reliable and keep unverified family support out of the USB ID table;
an INF entry or a shared DLL does not prove that another product has compatible
timings, memory layout or initialization.

Near-term work, in recommended order:

1. (0.3.14 recovery soak: see "Updating the module without a reboot".)
   Soak-test through ordinary use, repeated module reloads, deliberately
   interrupted frames, unplug/replug, suspend/resume, DPMS, brightness, cursor,
   console and optional lossy video. Record failures with the kernel log and
   `mct_t2_stats` rather than adding recovery guesses.
2. Done: `tools/t2_recovery_test.py` replaces the scratch reproducer
   `tools/desync_lab.py`, which is kept out of the repository.
3. Counters added in 0.3.18 (see the performance backlog). Measure with
   them, then investigate adaptive lossy encoding as described above.
4. Investigate command `0x0a` as a possible 256 x RGB hardware gamma table
   (`tools/gamma_test.py` is written but has not been run yet).
   Its 768-byte size is suggestive, not proof. Test controlled ramps on the
   real panel first; expose a standard DRM gamma LUT only if its semantics and
   persistence across flips, DPMS and resume are verified.
5. Gradually isolate known DTU-specific constants (USB IDs, mode/timings,
   rotation, framebuffer addresses and init sequence) behind a device profile,
   but avoid a large common-family abstraction until a second device reveals
   the actual variation.

For each additional MCT device, collect USB descriptors and endpoints, chip
markings, resolution, Windows INF mapping, an initialization USB capture,
EDID/device-info replies, pre/post-init register sweeps, and verified frame,
cursor, power and brightness behavior. Confirm protocol framing on hardware
before selecting a T2 profile. Other Trigger generations may need separate
drivers with only small shared helpers rather than one monolithic driver.

Pen input remains the responsibility of Linux's Wacom/HID driver. Arbitrary
modes, RGB565, front-buffer writes and more asynchronous USB work are not
current priorities: the panel has one native mode, modern compositors use
XRGB8888, front-buffer updates risk tearing, and queued URBs have already been
measured without a performance gain.

## 2026-10-11

### Performance research backlog

*(Moved from driver/t2-driver.md, "Performance research backlog", on 2026-10-14.)*

Keep proposed work distinct from the measurements above. In priority order:

1. Frame-latency histogram and payload-size/over-budget counters (0.3.18,
   merged; measured in the 2026-10-11 stats: all updates under 16 ms). `mct_t2_stats` gained two lines: `over budget`
   counts updates whose encoded payload exceeded ~600 KB, with the largest
   payload seen, and `latency ms` is a histogram of the time from the first
   damage of an update to its flip (<8, <16, <33, <50, <100, >=100 ms).
2. Investigate adaptive lossy encoding: choose the smallest tolerance that
   brings a large update near the device's roughly 600 KB-per-60-Hz-frame
   budget, instead of applying one fixed tolerance. Benchmark its extra encode
   cost and inspect gradients and motion before considering it for the driver.
   Do not revive the earlier damage-size/lossy-streak heuristic unchanged; it
   produced settling shadows and horizontal/CRT-like artifacts.

   Offline numbers from a Python model of the encoder (full 1024x600 frame,
   runs absorb values within +-tol of the run's first value; payload / PSNR):

   | image    | tol 0        | 1           | 2           | 4           | 8           |
   |----------|--------------|-------------|-------------|-------------|-------------|
   | photo    | 817K / exact | 599K / 54dB | 519K / 49dB | 443K / 44dB | 294K / 36dB |
   | gradient | 737K / exact | 374K / 53dB | 253K / 48dB | 156K / 42dB | 91K / 36dB  |
   | desktop  | 75K / exact  | 72K         | 70K         | 68K         | 64K         |
   | noise    | 1799K        | 1799K       | 1799K       | 1799K       | 1799K       |

   So the smallest tolerance that fits is usually 1 or 2, well below the
   fixed 4, and loses far less (54 or 49 dB against 44). Noise-like content
   does not compress at any tolerance, so a search should give up early there
   and keep the lossless encoding. Desktop content is always within budget.
   Each tolerance tried costs one more encode (~2.5 ms full screen), so try
   at most two, e.g. 2 then N, and not a full search.

   Branch `adaptive-lossy` (0.3.19, merged; not tested live with lossy on) picks the tolerance
   from how far the lossless encoding overshoots, with no extra encode: up to
   4/3 of the budget uses 1, up to twice the budget uses 2, anything more the
   `lossy` value, which becomes a maximum. The photo (817K) gets 2 and fits at
   519K, the gradient (737K) gets 1 and fits at 374K. `lossy updates` in
   `mct_t2_stats` now counts updates per tolerance used.
3. Evaluate cost-based damage-band merging. Estimate the encoded/USB cost of
   spanning unchanged rows rather than always using the fixed 16-row gap. This
   may help scattered desktop damage, although measured desktop traffic is
   already low, so the expected benefit is modest.
4. If useful after the above, visually validate intermediate USB payload sizes
   such as 32 or 64 KiB. The measured throughput gain over the ~~vendor's~~ 19968 B
   chunks is only about 3%, while very large writes have corrupted frames.
   EDIT (2026-10-12): 19968 B is QEMU's split of Windows' 512 KiB transfers,
   not the vendor's size; the corruption with one huge transfer is more likely
   a host-side issue. 512 KiB transfers are a test step in `tools/t2_probe.py`.
5. Treat the DLL's undocumented transfer/stretch operations as longer-term
   reverse-engineering candidates for hardware-assisted moves or scaling, not
   as a known optimization. EDIT (2026-10-12): now understood from the older
   vendor kernel driver: `0x11`/`0x13` can scale (2.14 factors) and take
   RGB565/YUV sources, and the chip has a blit engine (`0x14` or `0x15`
   register batches); see protocol-t2.md, "Tested on the tablet 2026-10-12".

Do not prioritize asynchronous URBs, front-buffer writes, or diff-loop
micro-optimization: queued URBs were already slower, front-buffer updates risk
tearing, and measured diff time is negligible compared with USB transfer time.

## 2026-10-12

### Tested on the tablet 2026-10-12 (`tools/t2_probe.py`)

*(Moved from reference/protocol-t2.md, "Tested on the tablet 2026-10-12", on 2026-10-14.)*

Each one drawn in the hidden buffer next to a reference box drawn the known
way, then shown with a flip; the user saw the expected picture every time,
and the chip answered a register read right after every command (log in
the lab: `captures/t2_probe-20261012-142605.log`). Verified:

- **512 KiB payload transfers** (a 1,082,400 B full frame as 524288 +
  524288 + 33824 B), like Windows.
- **`0x11` raw RGB24**, payload B, G, R.
- **2x hardware upscale** with `0x11` (64x32 source, 128x64 target, scale
  `0x2000`, payload = source size) and with `0x13` (same, source = target
  address, raw length = source pixels x 3).
- **RGB565 through `0x11`** (format `04`, 2 bytes per pixel, little-endian
  5-6-5): colours as expected.
- **`0x13` decoding to a separate address** (`0x3a0000`), stretched 1:1 into
  the framebuffer.
- **Partial 2x upscale without touching the neighbours** (steps j and k,
  2026-10-12): a 64x32 source scaled into a 128x64 box inside a plain grey
  band, decoded in place (source = target) and from scratch VRAM: the band
  stayed clean in both cases.
- **YUV 4:2:2 RLE through `0x13`** (format `8a`, mode 6, Y/U/V planes of
  1536/768/768 per 3 KiB unit): flat colour bands as expected.

Also on 2026-10-12 (steps l, m, n, p; log
`captures/t2_probe-20261012-165352.log`):

- **`0x11` 32-bit BGRX** (format `0e`, 4 bytes per pixel, X = 0): correct.
- **`0x11` YUV 4:2:2 YUY2** (format `8a`) and **packed 4:1:1** (format
  `96`, `Y0 U V Y1 Y2 Y3`): correct colours, shades as expected for YUV.
- **8-bit palette: not as expected.** With the `0x0a` table loaded (B, G, R
  entries; a few colours, the rest zero) the **24-bit** background and
  reference box changed colour too (purple background, the box reduced to
  two flat blocks), the 8-bit `0x11` box was wrong, and a strip of
  black/white/grey lines ~12 px high ran across the top. Restoring the 768
  zero bytes brought the normal picture back; the chip kept answering. So
  `0x0a` is an output lookup table that acts on everything; an all-zero
  table (what Windows loads) apparently means "off". Untested: whether an
  identity or gamma ramp gives a usable hardware gamma (instead of the
  software gamma), the channel order, and how the 8-bit source is really
  selected (format `00` with a w*h length was read as something else).

### Half-resolution motion: history of the mode (2026-10-12 and 2026-10-13)

*(Moved from driver/t2-driver.md, "Half-resolution motion", on 2026-10-14.)*

History of the mode:

First test (0.3.24, 2026-10-12): the picture stayed pixelated, also when
still. The motion check compared against the back buffer's copy, which is
unknown right after a half frame, so every later update to that buffer
counted as a full-screen change and went half again, and each half frame
re-armed the refinement timer, so with anything updating it never ran.
0.3.25 compares with the last presented picture instead.

Second test (0.3.25, 2026-10-12): works well, but moving a window over the
still wallpaper blurred and re-sharpened the still parts too, which looks
unsteady ("like under a microscope"). Half mode replaces the whole screen,
so 0.3.26 uses it only when at least 80% of the screen changed (full-screen
video, workspace switches); smaller changes stay exact or use the lossy
level. Blurring only the changed area would need the chip to scale a
partial rectangle without touching the pixels next to it: decoding the
source elsewhere (scratch VRAM) and scaling into the rectangle, a
combination the probe has not tested yet.

0.3.27 corrects the size test: it measured the exact *full frame* (a
detailed wallpaper alone is ~837 KB, over the ~600 KB budget), so any large
change "did not fit". It now encodes what changed, aligned as it is sent,
and goes half only if that exceeds the budget.

0.3.28 replaces the whole-screen half frame by half-resolution rects of
just the changed areas (after steps j and k passed), drops the 80% rule
and the last-picture copy, and tracks the blurred areas per buffer. Test
(2026-10-12): window drags got choppier and choppier. The blurred areas
were added to the buffer's next update and sent at half resolution again
together with the new movement, so the blurred area grew along the drag
path. 0.3.29 uses tiles: only tiles that changed since the last frame go
half; previously blurred tiles that stopped moving go exact. A model of a
60-frame drag kept the blurred area at ~80 tiles instead of growing.
Test of 0.3.29: smoother, but on quick moves parts of a (translucent)
window near its edges vanished or looked torn out for a moment. The half
rects were decoded in place: the chip first writes the packed source into
the target area, then scales over it, and a flip before the scaler
finishes shows the packed data. 0.3.30 decodes at `0x560000` (past the
staging area of a worst-case full frame) and scales from there, as in
probe step k. Test of 0.3.30: the edges still glitch, also on small
moves. ~~Remaining suspect: the scaler still running when the `0x15` flip
is executed (the vendor's blit path never waits either, but it only wrote
the front buffer). Next: a probe experiment for a "scaler done" signal
(register `fe70`/`fc59` reads after a large scaled rect), then wait for it
before flipping. Until then half mode has this artifact.~~ EDIT
(2026-10-12): a code review (lab `captures/t2-research/scaler-sync.md`)
found a definite cause. A half rect an odd number of tiles wide needs an
even number of half-resolution rows (the decode works in whole 1024-pixel
lines), so one an odd number of tile rows high grew by a tile row
(about a quarter of half rects in a model of window drags). That row was
drawn blurred in this buffer only and marked stale, so it alternated
blurred and sharp with each flip. 0.3.31 redraws every tile a half rect
grew over exactly, in the same frame. No vendor waits between a scaled
`0x13` and the flip; the scaler-race probes (step r and the report's
P1-P4) stay as follow-ups if the edges still glitch. EDIT (2026-10-13): 0.3.31 tested, the band at the
edges still flickers, so the grown row wasn't the (only) cause. Probe step
r then ran a full-screen 2x scale from `0x560000` cleanly three times and
found no "scaler done" register (protocol-t2.md, "Tested on the tablet
2026-10-13"), so neither the scaler nor the scratch source is broken in
general. Left to look at: the tile bookkeeping across the two buffers
(stale tiles after a flip), edge taps of the scaler (report probe P1), and
whether the band is the half rect's own edge. Probe step y (2026-10-13): a plain grey
source decoded inside magenta-filled scratch and scaled 2x into a grey
band showed no line at all, so the scaler doesn't read past the source
(top/bottom; left/right taps would read the neighbouring packed rows, also
grey). That leaves the driver's own per-update decisions: a tile at the
edge of a moving area that changes only every other update alternated
blurred/sharp, and a drag near the budget switched the whole moving area
between half and exact. 0.3.32 adds hysteresis: half-resolution tiles stay
half for one more update, and half mode stays on while the update is at
least half the budget. Test of 0.3.32: the flicker is gone. But with a
photo wallpaper the tablet stayed in half mode with nothing moving: the
hysteresis kept the blurred tiles half, and re-sharpening a photo costs
more than half the budget, so it never left. A black wallpaper was cheap
enough to get out. 0.3.33 keeps only the tiles that actually **moved** in
the last update half, and stays in half mode only while something moves;
a still picture is always sharpened. Test of 0.3.33: the wallpaper and
edge-flicker problems are gone, but parts of a translucent window near its
edges vanish again during moves (as with 0.3.29, before the scratch
decode). **Half mode stays experimental and off by default** (the user,
2026-10-13); no more work on it planned for now. Ideas if it's picked up
again: a debug tint of half areas (lab `captures/t2-research/scaler-sync.md`
P5) to see whether the vanishing parts are half tiles or exact ones, and
the moved/half bookkeeping of tiles that change only in one buffer.

## 2026-10-13

### Tested on the tablet 2026-10-13 (logs `captures/t2_probe-20261013-141457.log`, `-142450.log`)

*(Moved from reference/protocol-t2.md, "Tested on the tablet 2026-10-13", on 2026-10-14.)*

- **`0x0a` as hardware gamma (q): no.** Identity, red-only and gamma 2.2
  tables showed no visible change. Afterwards the chip was in a bad state:
  a later full-screen 2x and plain 1:1 pictures came out wrong (reddish,
  washed-out background, boxes doubled, a white/red line across), even
  though the zero table had been loaded back. A re-init cleared it. The
  same steps run again without q were all correct.
- **Full-screen 2x from scratch VRAM (r): works.** 512x300 decoded at
  `0x560000` (mct_t2's half-mode source) and scaled to 1024x600 into the
  hidden buffer, three times, with a 1:1 run in between: the expected
  picture. **No "scaler done" signal**: of 58 registers read in a tight
  loop for ~40 ms after each payload (`fe70`, `fc59`, `fc34`, `fe57`,
  `fe36`, `fe71/72`, `fc50-5d`, `fe60-67`, `fe78-7b`, `fe80-89`,
  `fb60-6e`), only `fc57` changed, and it changes at rest too. The first
  read answers 1.2 ms after the payload (1.6 ms for a 1:1 frame).
- **16 MiB of VRAM (s): yes.** `0x13` staged at `0xD37000` (the Mac
  driver's address), sources at `0xE00000` and `0x800000 + x`: all drawn
  where expected, with no wrap at 8 MiB.
- **Mac "monitor off" (t1: `fb60=00 fcb0=04 fcb0=20`): the backlight goes
  fully off.** Its "on" (`fcb0=04 fcb0=09`) brings back a wrong picture:
  buffer A's colour and a garbage band ~20 px high at the bottom, because
  `fb60` stays `00` (the skipped check t1c would have restored it to `01`).
  A re-init fixes it. `fcf6 = 00/01` (below) already gives a clean
  off/on, so this adds nothing for now.
- **Mac "monitor on" with `fb60=01` restored too (z1b, log
  `captures/t2_probe-20261013-150137.log`): backlight on, no picture**, and
  the video engine stalled: each 16 KiB `0x12` cursor upload afterwards
  took ~30 s instead of milliseconds, and `fc6f` read back `00` after `30`
  was written. Only the init recovered it. The probe applied it wrongly:
  the Mac driver calls monitorPowerOnOff (`FUN_100030350`) only inside its
  mode-setting job queue (`MCTT2Device::addModeSettingJobsToQueue`: off as
  the first job, on as the last, `FUN_1000763c4`), around the timing and
  PLL jobs. So "off" resets the display pipeline for a mode set, and "on"
  needs that mode set in between; it's not a screen switch. For screen
  off/on, `fcf6 = 00/01` stays the clean way. The
  second cursor slot's address unit (z2) couldn't be judged without a
  picture; not worth more probing.
- **EP2 timeout (w, log `captures/t2_probe-20261013-150844.log`):**
  `0x91` reads MC8032 register 4 (4 bytes, big-endian; the init leaves
  `0x61a80`). With `0x90` setting it to `0x1000`, a `0x13` cut off after
  555 of 1111 payload bytes was ended by the chip itself after **307 ms**,
  with no padding: about 75 µs per unit, so the init's `0x61a80` is about
  **30 s**. That explains step z's ~30 s per command after the Mac
  monitor-off: the chip took no payload and each command ended by
  timeout. But after the timeout the stream was **out of step**: the next
  complete `0x13` (with `0x61a80` set back) didn't draw where it should,
  the flip back was missed, and the box turned up later in buffer A's
  bottom-right corner. So a short timeout ends a stuck command but doesn't
  resync the parser; the padding recovery stays needed.
- **Cursor (u): edge clipping in hardware works** (u3-u5: negative x, y
  and both via `fe44/fe45` and `fe36 = 4f/47`), so the driver's shifted
  re-upload for cursors past the top/left edge could go. A **second shape
  at `0x4000` (u1) showed blue garbage**, although `fe30-33` read back as
  sent (`00 10 00 00` = `0x4000 >> 2`): the address unit or the upload is
  not understood yet.
- **In-band flip with `0x16` lists on EP2 (v): works**, both directions
  (`16 fb 01 65 <sel>` and `16 fc 06 b4 11 b1 .. b2 .. b3 .. b4 15 b4 11`),
  so register writes can go in order with the frame data.
- **ID ROM "light" GPIO (x, `feb1` bit 6 low): no visible effect.** Not
  the backlight on this board.

Still untested:

- ~~the `0x0a` table as hardware gamma (identity ramp first, then a curve)~~ (no, above)
- ~~a "scaler done" signal (half mode's edge glitch, see t2-driver.md)~~ (none found, above)
- ~~the EP2 timeout (`0x90` register 4, probe step w)~~ EDIT (2026-10-14): tested the same day, see "EP2 timeout (w)" above
- the blit engine (`0x14`, or `0x15` register batches on `fc50-5d`, `fc34`,
  `fc59`, `fe60-89`, `fe70`), which could make scrolling a short command
  instead of resending the rows; the riskiest, since a wrong command may hang
  the chip until the tablet is power-cycled.

## 2026-10-14

**niri doesn't use the tablet's hardware cursor.** `tools/cursor_latency.sh
t2` counted 0 cursor commits with the mouse on the tablet (mct_t2 0.3.37),
and with `drm.debug=0x10` every commit to the tablet touched only plane 0
(the picture): niri never even test-commits the cursor plane, so it draws
the cursor into the picture. Each move then costs a frame update (16-26 ms
over USB 2) instead of a 0.1 ms register write. The driver side looks
right: the cursor plane (64x64, ARGB8888 linear, CRTC 0) is offered, and
the same core code is used by niri on the T6. Ruled out: cursor size
(Bibata 24 px at scale 1), output scale/transform, niri's flags (cursor
plane allowed). The skip happens in smithay's DrmCompositor before any
test commit (only trace-level logs there). Suspect: the display-only
rendering path of our niri patch, which for some devices renders on the
primary GPU and copies, possibly with the cursor baked in. To find out:
run niri once with `RUST_LOG=smithay::backend::drm=trace` (needs a session
restart, or a VM with the tablet passed through).

EDIT (2026-10-14): found in a VM with the tablet passed through (lab
`captures/scout/t2-cursor-plane-vm-2026-10-14.md`; niri rebuilt with trace
logs, which release builds strip). The suspect was wrong: our driver set
the smallest framebuffer to the panel size (`mct_t2.c` `.min_w = T2_WIDTH,
.min_h = T2_HEIGHT` -> `mode_config.min_width/height` = 1024x600), so DRM
refused niri's 64x64 cursor buffer (`drm_framebuffer.c`, "bad framebuffer
width"); smithay logs that at debug level ("failed to export framebuffer
for cursor plane") and draws the cursor into the picture, before any test
commit. With 64x64 (as the T6 has) niri assigned the cursor to the plane
and the driver got cursor commits. Fixed in mct_t2 0.3.38.

**Test of mct_t2 0.3.38 on the host** (`tools/cursor_latency.sh t2`, mouse on
the tablet; video phase skipped): niri now uses the cursor plane.

| Phase | Commits | Replaced | Move average | Shape changes | Shape average |
|---|---|---|---|---|---|
| still desktop | 597 | 0 | 0.04 ms | 0 | - |
| dragging a window | 596 | 0 | 0.04 ms | 1 | 1.14 ms |
| changing shapes | 592 | 0 | 0.04 ms | 81 | 1.10 ms |

Maximum since load: 1.64 ms for a move, 8.85 ms for a shape (an upload on
EP2, which waits for a frame being sent). Commits come at the panel's
60 Hz. Before, every move was a frame update of 16-26 ms.

## 2026-10-15

**Immediate cursor (niri patches, mct_t2 0.3.42) on the real desktop.**
`cursor_latency.sh t2 10`: desktop 10418 commits, 9575 between frames, 7792
sent, 0.03 ms commit -> device; fullscreen video 6806 / 6270 / 5441, 0.06 ms;
hovering 2362 between frames, 96 shapes at 0.79 ms. One shape took 28.8 ms
during a (poorly done) window drag: shape uploads (0x12 on EP2) take io_lock
and wait for a frame in progress; moves (EP4 lists) don't. Interleaving a
cursor upload into a frame's payload would desync the chip, so this stays.
