# Kernel driver, Trigger II backend (`mct_t2.c`)

> **Reference**, kept current with the code (mct_t2 0.3.36). Measurements carry the date they were taken; the experiments behind them are in [t2-lab-notebook.md](../history/t2-lab-notebook.md).

Since the Trigger 6 work the driver is one module, `mct`, with a backend
per chip; this file covers the Trigger II backend (DRM driver name `mct_t2`).
For the Trigger 6 see [protocol-t6.md](../reference/protocol-t6.md),
[t6-limits.md](../reference/t6-limits.md) and [t6-lab-notebook.md](../history/t6-lab-notebook.md).
The Trigger II protocol is in [protocol-t2.md](../reference/protocol-t2.md),
the history of the experiments in
[t2-lab-notebook.md](../history/t2-lab-notebook.md).

> **Current state (mct_t2 0.3.38):** the planes, CRTC, damage handling and
> workers come from the shared core ([shared-core.md](shared-core.md),
> `kernel/mct_drv.h`); `mct_t2.c` keeps the protocol, init, RLE and lossy
> encoder, resync, backlight, cursor format and ID ROM. The init is the
> trimmed one (78 transfers, ~1 s). Updates go into the back buffer and flip
> ("Update strategy (driver 0.3.6)"). In since then: the counters (0.3.18),
> an adaptive (0.3.19) and per-channel (0.3.23) lossy tolerance, software
> gamma (0.3.23), cursor clipping by the chip (0.3.34), two more USB IDs
> (0.3.35), the ID ROM read (0.3.36), cursor latency statistics (0.3.37) and a
> 64x64 minimum framebuffer so niri uses the hardware cursor (0.3.38);
> half-resolution motion (0.3.24) is
> experimental and off. The sections below follow the versions in order;
> parts marked superseded describe earlier versions. Open work is in
> [roadmap.md](../roadmap.md).

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

Speed ideas tried and still open: [t2-optimization-ideas.md](../research/t2-optimization-ideas.md)
(2026-10-10) and the [roadmap](../roadmap.md).

## Optional lossy mode (driver 0.3.9)

`lossy=N` (module parameter, runtime-writable in `/sys/module/mct/parameters/`,
NixOS `hardware.mct.lossy`, default 0): each rect is encoded losslessly
first; only if the update's total would exceed what the device takes in one
60 Hz frame (~600 KB) is it re-encoded with runs absorbing values within +-N
(the vendor uses 4). Since 0.3.19 N is the maximum: the tolerance is 1, 2 or
N depending on how far over budget the update is (the offline numbers
behind it are in [t2-lab-notebook.md](../history/t2-lab-notebook.md),
"Performance research backlog"). The encoder writes back what the device will show, so the
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

It is **experimental and off by default** (since 2026-10-13): during
moves, parts of a translucent window near its edges can vanish for a
moment. How the mode got here, test by test (0.3.24 to 0.3.33), is in
[t2-lab-notebook.md](../history/t2-lab-notebook.md).

## Software gamma (driver 0.3.23)

The T2 offers a 256-entry gamma LUT on the CRTC, like the T6 (night light,
gammastep, wlsunset). The chip has no gamma tables, so a non-identity LUT
is applied to the update's snapshot before diffing: the device copies, the
lossy write-back and the refinement all work on the corrected image, and a
LUT change re-diffs the whole screen. With the identity LUT nothing extra
runs. Tested on the tablet 2026-10-12 (night light): works.

## Init, cursor, DPMS and vblank (driver 0.2, 2026-09-24)

- **Trimmed init**: 78 transfers, ~1 s from cold (see `docs/reference/t2-init-sequence.md`), black
  frame into buffer A right before `0x14`.
- **Hardware cursor plane** (`DRM_PLANE_TYPE_CURSOR`, ARGB8888, 64x64): converts
  to the T2's A,B,G,R format rotated 180 degrees and uploads with `0x12` only when the image
  changes. Moves send one 16-byte EP4 packet. Boxes hanging off the visible right/bottom
  edge are clipped by the chip (`fe44`/`fe45`, `fe36` `4f`/`47`, since 0.3.34);
  before, they got a shifted image, re-uploaded on every move there.
  Much smoother than composited cursors, and less USB traffic.
  Until 0.3.38 the driver's smallest framebuffer was the panel size
  (1024x600, `mode_config.min_width/height`), so DRM refused a compositor's
  64x64 cursor buffer and niri drew the cursor into the picture (a frame
  update per move, 16-26 ms). Since 0.3.38 it's 64x64, as on the T6.
  Measured on the host: moves 0.04 ms from commit to device, shape changes
  ~1.1 ms (`tools/cursor_latency.sh t2`).
- **Panel power / DPMS**: CRTC disable writes `fcf6=0` (backlight off), enable
  writes `fcf6=1`. niri's `power-off-monitors` turns the tablet off like the other
  monitors, and it comes back instantly.
- **Vblank timer** (`DRM_CRTC_VBLANK_TIMER_FUNCS` + `drm_crtc_vblank_atomic_flush`):
  page-flip events at 60 Hz, so compositors don't render unthrottled
  (niri warned "running faster than expected"). Verified after a reboot: no more throttling warnings.
- USB access is serialised by `io_lock` (EP2 command sequences: frames vs cursor uploads).

## ID ROM (driver 0.3.36)

The `0x1d` read at init is kept; its checksum, version, video memory,
180-degree flag and base USB ID are logged once and shown in sysfs
(`vram_mb`, `id_rom`) and the app. A wrong checksum or a ROM that doesn't
mark the panel as rotated is logged; the driver still uses the DTU-1031X's
fixed layout. The ROM's layout is in
[protocol-t2.md](../reference/protocol-t2.md#id-rom-from-j5creates-macos-driver-applied-to-this-tablets-rom).

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

A padded frame usually ends with the wrong pixel count, so the firmware
stops taking data and answers only after an internal timeout of about
15-30 s; the driver waits for it, with a 45 s deadline (since 0.3.17). A USB
reset of the stuck chip is worse (it stops answering control requests until
a power cycle or replug), so the driver never sends one.
`tools/t2_recovery_test.py` tests the recovery; the soak results and the
experiments behind it (0.3.13 to 0.3.17) are in
[t2-lab-notebook.md](../history/t2-lab-notebook.md).

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

`5201` and `520f` (other DTU-1031X units, untested; since 0.3.35) only go
past the `0x1d` ROM read, the 5th init record after four GPIO writes, if
the panel EDID's manufacturer is `WAC` (`t2_wacom_panel`), since plain T2
adapters such as the j5create JUA230 use `5200-520f` too.

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
