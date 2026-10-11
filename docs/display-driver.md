# Linux display driver: EVDI + t2d (2026-09-24)

The DTU-1031X works as a **real monitor** in niri. Output `DVI-I-1`,
"Wacom Tech DTU-1031X", 1024x600 @ 59.986 Hz, desktop shown upright, smooth.

## Architecture

```
niri (renders on amdgpu) --> EVDI virtual connector (card2, panel EDID)
                                   |  damage rects + XRGB8888 pixels (libevdi)
                                   v
                     t2d (userspace, libusb) --> USB 2.0 bulk --> T2-285A --> panel
```

This mirrors Windows (IddCx indirect display `TrgIdd.dll` + `T2Indisp.dll`).

- **EVDI** (DisplayLink's virtual DRM driver, v1.14.15) + `nix/evdi-linux-7.2.patch`.
  Linux 7.2 renamed `struct drm_atomic_state` -> `drm_atomic_commit` (and
  `_alloc/_clear/_put`); upstream EVDI doesn't build without the patch.
  EVDI ioctls are unlocked and card nodes are group `video`, so the daemon runs as the user.
- niri >= PR #2312 (Nov 2025; 26.04 has it) renders display-only DRM devices on
  the primary GPU. No config needed.
- **t2d** (`t2d/t2d.c`):
  1. `init.bin`: 359 transfers extracted from the Windows capture
     (`tools/gen_init_blob.py`), replayed with 20 ms pacing on EP3/EP4 -> 2.8 s.
  2. `evdi_connect2` with `data/panel_edid.bin`, register one XRGB buffer,
     `evdi_request_update` / `evdi_grab_pixels` loop (backpressure: the next
     update is requested only after the previous one has been sent).
  3. Damage > 1/3 of the screen (or first frame) -> full `0x13` into the back buffer + `0x15`
     flip. Otherwise -> per-rect partial `0x13` straight into the front buffer
     (same policy as Windows, see `captures/redir4.bin`).
  4. Partial rects: rotate 180 degrees into device space, x/w aligned to 64 px,
     h rounded so `w*h % 1024 == 0` (shifted up if it would cross row 600).
  5. RLE encoder in C, byte-identical to the Python/Windows encoder
     (tested on photo, noise, black: `t2d -t encode`).

Measured: full frame send + flip 14 ms (simple content). Peaks of ~80
updates/s at ~50 Mpx/s damage during window motion. USB 2.0 caps worst
case (noise-like content, 1.84 MB/frame) at ~20 fps.

## Build / run

```bash
nix build .#evdi                       # patched EVDI module + libevdi (linuxPackages_latest)
sudo insmod result/lib/modules/$(uname -r)/kernel/drivers/gpu/drm/evdi/evdi.ko initial_device_count=1
nix build .#t2d && ./result/bin/t2d    # full init + EVDI loop  (-n: skip init, -v: verbose)
./result/bin/t2d -t pattern            # USB-only test: pattern + partial rect, no EVDI
```

NixOS: `nixosModules.default` with `hardware.mct.enable = true;` loads the patched
EVDI with `initial_device_count=1`, adds a udev `uaccess` rule (which also starts the
user service on plug), and runs `t2d` as a systemd user service with `Restart=always`.

## Known limits / TODO
- On USB error (unplug) t2d exits; systemd restarts it (full init). If the
  tablet is absent it retries every 3 s.
- The hardware cursor (EP4 `05 0e 38..3b`) isn't used; EVDI composites the cursor into
  the framebuffer, so cursor moves cost a small partial update.
- DPMS off isn't forwarded (panel stays lit). Candidate: backlight/`0x14` regs.
- Init is still the full Windows replay. Minimisation is optional.
- Encoder is exact (lossless). Windows' +/-4 lossy merge could cut bandwidth
  for photos/video if ever needed.
