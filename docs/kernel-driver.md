# Native kernel driver: `mct_t2` (2026-09-24)

`kernel/mct_t2.c` is a DRM/KMS driver for the T2. It replaces EVDI + `t2d`, and
because it's a normal DRM device with fbdev emulation, the **text console
works on the tablet** too.

Tested on Linux 7.2.6 with niri 26.04:
- niri output `USB-1` "Wacom Tech DTU-1031X 1", 1024x600 @ 59.986 Hz. The EDID is read
  from the device during init (`0x1d` reply, offset `0x80`).
- Cursor/window motion is smooth once damage clips are enabled (see below).
- `fb1` (`mct_t2drmfb`). `con2fbmap 2 1` + Ctrl+Alt+F2 shows tty2 on the tablet.

## Design

Modelled on `drivers/gpu/drm/tiny/gm12u320.c` (in-tree USB projector driver).

- USB driver for `0711:520d` interface 0. `probe` runs the init sequence
  (`kernel/mct_t2_init.h`, generated from `t2d/init.bin` by
  `tools/gen_init_header.py`): 359 transfers, ~3 s, with 20 ms pacing on EP3/EP4.
- `drm_simple_display_pipe`, XRGB8888 only, GEM shmem + shadow-plane helpers,
  `drm_client_setup()` for fbdev/fbcon. Connector type `USB`.
- `drm_plane_enable_fb_damage_clips()` is required. Without it every commit is
  full-screen damage and the display feels choppy.
- Plane update: copy the merged damage rect from the mapped framebuffer into a
  persistent XRGB **shadow image**, union it into a pending rect, then queue a work item.
  Because the shadow is updated every commit, compositors flipping between
  several buffers still only send damage.
- Worker: pending rect > 1/3 of the screen (or the first frame after enable) -> full frame into
  the back buffer + `0x15` flip. Otherwise -> partial `0x13` into the front buffer
  (rotated 180 degrees, 64-px x alignment, `w*h % 1024 == 0`), the same policy as the vendor driver.
- All USB I/O is synchronous `usb_bulk_msg` through a 64 KiB kmalloc bounce buffer.
  Headers are their own transfer and payloads go in 19968 B transfers.
- Unplug: `drm_dev_unplug()` + `drm_atomic_helper_shutdown()`. Worker is guarded
  by `drm_dev_enter()`. Resume re-runs init.

## Build / load

```bash
nix-build nix/kmod-local.nix -o result-kmod        # against <nixpkgs> linuxPackages_latest
nix build .#mct-t2-kmod                            # against the flake's nixpkgs
make -C kernel KDIR=/lib/modules/$(uname -r)/build # plain out-of-tree build
sudo insmod mct_t2.ko                              # binds the tablet automatically
```

Console on the tablet: `con2fbmap <tty> <fb>` (from `fbset`), or boot with
`fbcon=map:<digits>` (one fb index per console).
The NixOS option `hardware.mct-t2.consoleMap` sets the latter. fbcon applies the
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

`lossy=N` (module parameter, runtime-writable in `/sys/module/mct_t2/parameters/`,
NixOS `hardware.mct-t2.lossy`, default 0): each rect is encoded losslessly
first; only if the update's total would exceed what the device takes in one
60 Hz frame (~600 KB) is it re-encoded with runs absorbing values within +-N
(the vendor uses 4). The encoder writes back what the device will show, so the
buffer copies stay exact; 150 ms after the last lossy update two lossless
passes (one per buffer) make the picture exact again.

Measured (197 s, mostly video, N = 4): 63% of updates lossy, ~17 ms per update
(vs ~26 ms lossless), sent/damaged 84%. Window openings and ordinary scrolling
stay lossless. A first version that went lossy whenever more than a third of the
screen changed was lossy for 83% of updates and showed "settling" shadows.
Artifacts: short horizontal streaks during heavy motion (the RLE is row-wise).

## Features added 2026-09-24 (driver 0.2)

- **Trimmed init**: 78 transfers, ~1 s from cold (see `docs/init-sequence.md`), black
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

`mct-t2-reload` (installed by the NixOS module) unbinds the tablet, unloads
`mct_t2` and loads the module of the current system generation. This needs a
compositor that closes removed DRM devices (niri needs `niriFix`).

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

## Known issues / TODO

- Hot-unplug/replug works on the kernel side (disconnect, re-probe and
  ~1 s init, no warnings), but **niri 26.04 does not clean up removed DRM
  devices**: `device_removed()` calls `DrmNode::from_dev_id()`, which reads
  sysfs entries that are already gone, logs "error creating DrmNode" and
  returns early. The stale output stays (niri keeps queueing flips on the
  deleted node, or renders in a tight loop after an unbind), its fds pin the
  module, and the returning device is "unnamed" as a duplicate ("Unknown
  Unknown Unknown"). A fix (look the node up in niri's own device map) is
  being proposed upstream.
- Display managers on a text console (ly, getty) show only on the
  framebuffer their VT is mapped to. fbcon can't mirror a VT, so the tablet
  shows ly only with `consoleMap = "1"`, and then the main monitor won't.
- Only XRGB8888. RGB565 would need conversion in the shadow copy.
