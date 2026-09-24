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
  (niri warned "running faster than expected"). *Built; live test pending.*
- USB access is serialised by `io_lock` (EP2 command sequences: frames vs cursor uploads).

## Known issues / TODO

- **niri 26.04 does not release a removed DRM device.** After unbind/unplug
  it keeps its fds on the deleted `/dev/dri/cardN`, keeps listing the output, and
  retries rendering in a tight loop ("failed to import the dmabuf to gbm")
  until the output is turned off (`niri msg output USB-1 off`). The open fds pin
  the module (`rmmod: Module mct_t2 is in use`) until niri restarts. For
  development, building the module under another name (`DRIVER_NAME`) lets a
  new build bind while the old one is pinned. To report upstream.
- Only XRGB8888. RGB565 would need conversion in the shadow copy.
- niri also names re-added outputs "Unknown Unknown Unknown" when it trips over
  its stale devices (EDID in sysfs is correct); re-login fixes it.
