# Kernel driver, Trigger II backend (`mct_t2.c`, 2026-09-24)

> **Reference with dated measurements**: the design and known issues are current (mct_t2 0.3.21); measurement sections carry the date they were taken.

Since the Trigger 6 work the driver is one module, `mct`, with a backend
per chip; this file covers the Trigger II backend (DRM driver name `mct_t2`).
For the Trigger 6 see [protocol-t6.md](../reference/protocol-t6.md),
[t6-limits.md](../reference/t6-limits.md) and [t6-lab-notebook.md](../history/t6-lab-notebook.md).

> **Current state (2026-10-11, mct_t2 0.3.19):** the planes, CRTC, damage
> handling and workers now come from the shared core ([shared-core.md](shared-core.md),
> `kernel/mct_drv.h`); `mct_t2.c` keeps the protocol, init, RLE and lossy
> encoder, resync, backlight and cursor format. The init is the trimmed one
> (78 transfers, ~1 s). Updates go into the back buffer and flip
> ("Update strategy (driver 0.3.6)"). The counters (0.3.18) and adaptive
> lossy tolerance (0.3.19) are merged. The sections below keep the history
> in order; parts marked superseded describe earlier versions. Open work is
> in [roadmap.md](../roadmap.md).

`kernel/mct_t2.c` is a DRM/KMS driver for the T2. It replaces EVDI + `wacom-t2d`, and
because it's a normal DRM device with fbdev emulation, the **text console
works on the tablet** too.

Tested on Linux 7.2.6 with niri 26.04:
- niri output `USB-1` "Wacom Tech DTU-1031X 1", 1024x600 @ 59.986 Hz. The EDID is read
  from the device during init (`0x1d` reply, offset `0x80`).
- Cursor/window motion is smooth once damage clips are enabled (see below).
- `fb1` (`mct_t2drmfb`). `con2fbmap 2 1` + Ctrl+Alt+F2 shows tty2 on the tablet.

## Design

Modelled on `drivers/gpu/drm/tiny/gm12u320.c` (in-tree USB projector driver).

- USB driver for `0711:520d` interface 0 (since then also `0711:5209`, and
  the module is `mct`). `probe` runs the init sequence
  (`kernel/mct_t2_init.h`, generated from `wacom-t2d/init.bin` by
  `tools/gen_init_header.py`): 359 transfers, ~3 s, with 20 ms pacing on EP3/EP4
  (since trimmed to 78 transfers, ~1 s, see below).
- (superseded by the shared core) `drm_simple_display_pipe`, XRGB8888 only, GEM shmem + shadow-plane helpers,
  `drm_client_setup()` for fbdev/fbcon. Connector type `USB`.
- `drm_plane_enable_fb_damage_clips()` is required. Without it every commit is
  full-screen damage and the display feels choppy.
- Plane update: copy the merged damage rect from the mapped framebuffer into a
  persistent XRGB **shadow image**, union it into a pending rect, then queue a work item.
  Because the shadow is updated every commit, compositors flipping between
  several buffers still only send damage.
- (superseded by "Update strategy (driver 0.3.6)") Worker: pending rect > 1/3 of the screen (or the first frame after enable) -> full frame into
  the back buffer + `0x15` flip. Otherwise -> partial `0x13` into the front buffer
  (rotated 180 degrees, 64-px x alignment, `w*h % 1024 == 0`), the same policy as the vendor driver.
- All USB I/O is synchronous `usb_bulk_msg` through a 64 KiB kmalloc bounce buffer.
  Headers are their own transfer and payloads go in 19968 B transfers.
- Unplug: `drm_dev_unplug()` + `drm_atomic_helper_shutdown()`. Worker is guarded
  by `drm_dev_enter()`. Resume re-runs init.

## Build / load

```bash
nix-build nix/kmod-local.nix -o result-kmod        # against <nixpkgs> linuxPackages_latest
nix build .#mct-kmod                            # against the flake's nixpkgs
make -C kernel KDIR=/lib/modules/$(uname -r)/build # plain out-of-tree build
sudo modprobe mct                               # after make install; binds the tablet automatically
                                                # (insmod kernel/mct.ko needs the DRM helpers loaded)
```

Console on the tablet: `con2fbmap <tty> <fb>` (from `fbset`), or boot with
`fbcon=map:<digits>` (one fb index per console).
The NixOS option `hardware.mct.consoleMap` sets the latter. fbcon applies the
boot map when the tablet's framebuffer registers, which happens after the GPU's, so
the mapped consoles move over as soon as the driver has probed.

## Update strategy (driver 0.3.6)

The driver keeps a copy of what each device buffer (A, B) holds. Each update
takes a snapshot of the shadow image, diffs it against the back buffer (the
image from two updates ago), groups changed rows into up to 16 bands, sends
only those bands with partial `0x13` writes into the back buffer, and flips.

Compared with the vendor driver, which writes small changes straight into the
visible buffer and sends big ones as full frames, every update is tear-free and
the bytes sent follow the actual change, not the damage reported by the
compositor. Only commands the vendor driver also uses are involved. The cost is
one 2.4 MB memcmp/memcpy per update on the host.

Measured with the debugfs counters (`/sys/kernel/debug/dri/*/mct_t2_stats`, read
as root, e.g. `sudo sh -c 'cat /sys/kernel/debug/dri/*/mct_t2_stats'`), niri 26.04:

| Use | Updates/s | Pixels sent / damaged | USB average |
|---|---|---|---|
| desktop: typing, scrolling, moving windows (62 s) | ~17 | 16% | 0.57 MB/s |
| video playing (77 s) | ~27 | 93% | 16.3 MB/s |

niri tends to report most of the screen as damaged; for desktop use only a
sixth of that actually changes. With video most pixels really change, and the
link (~40 MB/s) still has headroom.

Stage timing (91 s of video, `ms diff/encode/usb` in the stats): diffing
0.13 s, RLE encoding 4.7 s (~2.3 ms per update), USB 49 s (~24 ms per update);
the worker was idle ~40% of the time, and USB ran at 38 MB/s while busy
(~43-44 MB/s is the link maximum). Keeping several URBs in flight (as udl does)
and overlapping encode with transfer would raise the full-screen ceiling by
roughly 15-20%, which only matters for content changing the whole screen at
more than ~38 fps.

Tried on branch `async-urbs` (8 URBs in flight, ~32-row strips so encoding
overlaps transfer): no gain. Per update 28.0 ms vs 26.4 ms for the same data,
37 vs 38 MB/s while busy. The T2 accepts RLE data at about 37-40 MB/s no matter
how the host queues it (the Python benchmark with whole-frame transfers topped
out at ~44 MB/s including idle-free pyusb writes), so the synchronous design
already runs at the device's limit. The branch is kept for reference, unmerged.

## Optional lossy mode (driver 0.3.9)

`lossy=N` (module parameter, runtime-writable in `/sys/module/mct/parameters/`,
NixOS `hardware.mct.lossy`, default 0): each rect is encoded losslessly
first; only if the update's total would exceed what the device takes in one
60 Hz frame (~600 KB) is it re-encoded with runs absorbing values within +-N
(the vendor uses 4). Since 0.3.19 N is the maximum: the tolerance is 1, 2 or
N depending on how far over budget the update is (backlog item 2). The encoder writes back what the device will show, so the
buffer copies stay exact; 150 ms after the last lossy update two lossless
passes (one per buffer) make the picture exact again.

Measured (197 s, mostly video, N = 4): 63% of updates lossy, ~17 ms per update
(vs ~26 ms lossless), sent/damaged 84%. Window openings and ordinary scrolling
stay lossless. A first version that went lossy whenever more than a third of the
screen changed was lossy for 83% of updates and showed "settling" shadows.
Artifacts: short horizontal streaks during heavy motion (the RLE is row-wise).

Since 0.3.23 the tolerance is per colour channel: green keeps N, red gets
1.5 N (rounded up) and blue 2 N, where the eye is less sensitive. Offline on
six real 1024x600 images (`captures/t2-research/per_channel_tol_result.txt`
in the lab): at the same payload size, luma PSNR is about 3.4 dB higher than
one tolerance for all channels; at the same luma error, payloads are 4 to
18% smaller. Red and blue can drift by up to 2 N, so smooth blue gradients
may band sooner; watch skies when testing.

## Half-resolution motion (driver 0.3.24, per tile since 0.3.29)

`motion=half` (sysfs, or the module parameter `half_motion`) works on
128x16 device tiles (8 per row, 37 rows plus an 8-row strip that is always
sent exact). For each update the back buffer needs the tiles that differ
from its copy plus those it still holds at half resolution. If sending
them exactly would not fit one frame's budget, the needed tiles that
changed since the last frame (moving content) go out at half resolution
(2x2 blocks averaged, a `0x13` with scale `0x2000` decoded at `0x560000`
and upscaled by the chip into the buffer without touching the neighbours:
`tools/t2_probe.py` step k);
the rest, such as wallpaper a window just left, goes out exact. Runs of
tiles merge into rects (checked in userspace: exact coverage, no overlap,
at most 91 rects for random masks, limit 152). The half-resolution tiles
are kept per buffer as a mask and sent again with that buffer's next
update, exact once they stop moving, so nothing accumulates. Still parts
are never blurred; the refinement after motion restores everything. With
the mode off, a buffer still holding half-resolution tiles is resent whole.

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
least half the budget.

## Software gamma (driver 0.3.23)

The T2 offers a 256-entry gamma LUT on the CRTC, like the T6 (night light,
gammastep, wlsunset). The chip has no gamma tables, so a non-identity LUT
is applied to the update's snapshot before diffing: the device copies, the
lossy write-back and the refinement all work on the corrected image, and a
LUT change re-diffs the whole screen. With the identity LUT nothing extra
runs. Tested on the tablet 2026-10-12 (night light): works.

## Performance research backlog

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
   register batches); see protocol-t2.md, "Not yet tested on the tablet".

Do not prioritize asynchronous URBs, front-buffer writes, or diff-loop
micro-optimization: queued URBs were already slower, front-buffer updates risk
tearing, and measured diff time is negligible compared with USB transfer time.

## Features added 2026-09-24 (driver 0.2)

- **Trimmed init**: 78 transfers, ~1 s from cold (see `docs/reference/init-sequence.md`), black
  frame into buffer A right before `0x14`.
- **Hardware cursor plane** (`DRM_PLANE_TYPE_CURSOR`, ARGB8888, 64x64): converts
  to the T2's A,B,G,R format rotated 180 degrees and uploads with `0x12` only when the image
  changes. Moves send one 16-byte EP4 packet. Boxes hanging off the visible right/bottom
  edge get a shifted image (the position registers can't go negative).
  Much smoother than composited cursors, and less USB traffic.
- **Panel power / DPMS**: CRTC disable writes `fcf6=0` (backlight off), enable
  writes `fcf6=1`. niri's `power-off-monitors` turns the tablet off like the other
  monitors, and it comes back instantly.
- **Vblank timer** (`DRM_CRTC_VBLANK_TIMER_FUNCS` + `drm_crtc_vblank_atomic_flush`):
  page-flip events at 60 Hz, so compositors don't render unthrottled
  (niri warned "running faster than expected"). Verified after a reboot: no more throttling warnings.
- USB access is serialised by `io_lock` (EP2 command sequences: frames vs cursor uploads).

## Backlight (driver 0.3)

`mct_t2` registers a backlight device, `/sys/class/backlight/mct_t2-<minor>`,
brightness 0-9: 0 turns the panel off (pixel clock stopped, as for DPMS), 1-9
are report levels 0-8 (8 = level 7, the power-on default).
Turning back on from 0 (or from DPMS off) only re-enables the pixel clock;
video memory is kept and no init runs. The short delay before the light returns
is the panel electronics re-locking to the signal, like a monitor waking up. The driver sends HID GET/SET_REPORT for
feature report `0x06` as control transfers to the Wacom (`056a`) device on the
same hub as the T2. No HID driver is involved, so it works while
OpenTabletDriver or the wacom driver owns the pen. The backlight device is a child
of the DRM connector (registered in `late_register`), so desktop shells can tell
which output it belongs to (verified: Noctalia 5.1 shows the slider for USB-1). If one doesn't, Noctalia can be told explicitly:
`[brightness.monitor.USB-1]` with `backlight_device = "mct_t2-2"` in its config.toml. Use it with `brightnessctl`,
a desktop brightness slider, or brightness keys.

## Updating the module without a reboot

`mct-reload` (installed by the NixOS module) unbinds the devices, unloads
`mct` and loads the module of the current system generation. This needs a
compositor that closes removed DRM devices (niri needs `niriFix`).

Since 0.3.13, probe and resume also recover a T2 left halfway through a
framebuffer payload by an interrupted older driver. The driver drains stale
replies, detects the stalled command parser with a register-read timeout,
pads the unfinished payload with zeros, and then runs the normal init. It
checks `feb0=40` and the pixel-clock register `fcf6=01` afterward and retries
the init once if either value is wrong.

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

## Suspend / resume

Verified 2026-09-24 (deep suspend, driver 0.3.7): the USB core resets the tablet on
wake, so `reset_resume` runs; it re-runs the init (writing the last image into
buffer A) and `drm_mode_config_helper_resume()` restores the state. Picture,
hardware cursor and backlight all come back without errors.

## USB IDs

The DTU-1031X's T2 enumerates as `0711:520d` or `0711:5209` depending on the
power-up (seen 2026-09-24: 20 times `520d` over three days, then `5209`, `5209`,
`520d` on three consecutive replugs). The ID ROM holds `0711:5200` and is
unchanged; the low bits are decided at power-up. MCT's Windows INF maps both
IDs to the same driver, and so does `mct_t2`. The pen function can enumerate
after the display, so the backlight looks the pen up on each access.

## Development roadmap

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

## Known issues / TODO

- **Faint rainbow lines after some inits (hardware).** On roughly 10-25% of
  inits, the picture comes up with faint greyish RGB vertical lines over an
  otherwise readable image, and it stays that way until the next init. This
  is the classic analog VGA sampling-phase artifact: the T2 drives the panel
  over an analog link (the EDID reports a VGA input), and on those inits the
  panel samples between pixels. Investigated 2026-09-25 with someone watching
  the panel:
  - It happens with the trimmed init and with the vendor's full init, from
    the driver and from userspace. It may have been seen on macOS too.
  - Not detectable: a full dump of the 8051's address space (0x0000-0xfeff)
    is identical in good and bad states apart from free-running counters.
    `fdb2`/`fdb3` change with frame writes and flips and are not a phase
    indicator.
  - Not fixed by: new frames or flips, switching the pixel clock off and on
    (backlight 0, `fcf6`), retuning the PLL (`f4` 2d -> 28 for 2.5 s and back),
    or replaying the PLL, timing, mode or buffer parts of the init.
  - Fixed by another full init, which is again random. After a bad load,
    `mct-reload`, a replug or a suspend/resume cycle gives another try.
  - A USB reset of the chip is not a way out: in the stuck resync case it
    stopped answering control requests, and only a port power cycle or
    replug brought it back.
- Hot-unplug/replug works on the kernel side (disconnect, re-probe and
  ~1 s init, no warnings), but **niri 26.04 does not clean up removed DRM
  devices**: `device_removed()` calls `DrmNode::from_dev_id()`, which reads
  sysfs entries that are already gone, logs "error creating DrmNode" and
  returns early. The stale output stays (niri keeps queueing flips on the
  deleted node, or renders in a tight loop after an unbind), its fds pin the
  module, and the returning device is "unnamed" as a duplicate ("Unknown
  Unknown Unknown"). A fix (look the node up in niri's own device map) is in
  `nix/niri-device-removed-fix.patch` (NixOS: `hardware.mct.niriFix`).
- Display managers on a text console (ly, getty) show only on the
  framebuffer their VT is mapped to. fbcon can't mirror a VT, so the tablet
  shows ly only with `consoleMap = "1"`, and then the main monitor won't.
- Only XRGB8888. RGB565 would need conversion in the shadow copy.
