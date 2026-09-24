# Native kernel driver: `mct_t2` (2026-09-24)

`kernel/mct_t2.c` is a DRM/KMS driver for the T2. It replaces EVDI + `t2d`, and
because it's a normal DRM device with fbdev emulation, the **text console
works on the tablet** too.

Tested on Linux 7.2.6 with niri 26.04:
- niri output `USB-1` "Wacom Tech DTU-1031X 1", 1024×600 @ 59.986 Hz. The EDID is read
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
- Worker: pending rect > 1/3 of the screen (or the first frame after enable) → full frame into
  the back buffer + `0x15` flip. Otherwise → partial `0x13` into the front buffer
  (rotated 180°, 64-px x alignment, `w·h % 1024 == 0`), the same policy as the vendor driver.
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

## Known issues / TODO

- **niri 26.04 does not release a removed DRM device.** After unbind/unplug
  it keeps its fds on the deleted `/dev/dri/cardN`, keeps listing the output, and
  retries rendering in a tight loop ("failed to import the dmabuf to gbm")
  until the output is turned off (`niri msg output USB-1 off`). The open fds pin
  the module (`rmmod: Module mct_t2 is in use`) until niri restarts. For
  development, building the module under another name (`DRIVER_NAME`) lets a
  new build bind while the old one is pinned. To report upstream.
- No DPMS / panel power control yet (backlight stays on when disabled).
- No hardware cursor (EP4 `05 0e 38..3b`). The cursor is composited, so moves cost a
  small partial update each.
- Only XRGB8888. RGB565 would need conversion in the shadow copy.
- The init is still the vendor sequence replayed verbatim.
