# mct-t2-linux

Linux display support for **MCT Trigger II (T2)** USB graphics, the chip
inside the **Wacom DTU-1031X** 10.1" pen display (`1024×600`, USB `0711:520d`).
The vendor only ships a Windows driver. Everything here came from USB
captures of that driver plus disassembly, then live testing on the hardware.

**Status:** the tablet works as a normal 60 Hz monitor under Wayland (tested with niri
26.04 on Linux 7.2). It shows up as a real DRM output, and only damaged regions are
streamed over USB 2.0. Window motion peaks at ~80 updates/s.

The pen digitizer (`056a:032f`) is a separate USB function and already works
with the in-kernel `wacom` driver.

## How it works

```
compositor ──► EVDI virtual connector (panel EDID, 1024×600@60)
                    │ damage rects + XRGB8888
                    ▼
               t2d (userspace, libusb) ──► USB bulk ──► T2-285A ──► panel
```

- [EVDI](https://github.com/DisplayLink/evdi) provides the virtual DRM connector.
  It needs `nix/evdi-linux-7.2.patch` to build on Linux ≥ 7.2.
- `t2d/` is the daemon. It replays the device init (359 USB transfers, ~3 s) and
  then encodes damaged regions with the T2 RLE codec: full frames go through double
  buffering and a flip, small damage becomes partial updates.
- Planned: a native DRM kernel driver (`kernel/`) replacing EVDI and t2d.

## Install

### NixOS (flake)

```nix
# flake.nix of your system
inputs.mct-t2.url = "github:<you>/mct-t2-linux";
# in your nixosSystem modules:
modules = [ inputs.mct-t2.nixosModules.default { hardware.mct-t2.enable = true; } ];
```

This loads the patched EVDI module, adds a udev rule giving the logged-in user
access to the tablet, and starts `t2d` as a systemd user service.

### Manual

```bash
# EVDI kernel module (patched for Linux >= 7.2), then:
sudo modprobe evdi initial_device_count=1
cd t2d && make EVDI_INC=/path/to/evdi/library && ./t2d
```

`./t2d -t pattern` sends a test pattern without EVDI. Use it to check the USB side.

## Protocol documentation

| Doc | Contents |
|---|---|
| [docs/protocol-t2.md](docs/protocol-t2.md) | Bulk protocol: commands, **transfer framing**, `0x13` frames, `0x15` flip, **RLE codec**, partial updates, HW cursor |
| [docs/init-sequence.md](docs/init-sequence.md) | The Windows driver's init sequence, step by step |
| [docs/device-dtu1031x.md](docs/device-dtu1031x.md) | Hardware: USB IDs, endpoints, EDID, registers |
| [docs/driver-analysis.md](docs/driver-analysis.md) | Vendor driver structure (`TrgIdd.dll` / `T2Indisp.dll`), function map |
| [docs/capture.md](docs/capture.md) | Getting complete USB traffic from a Windows VM (usbredir tee) |
| [docs/first-light.md](docs/first-light.md) | How the first image was reached: root causes, experiment log |
| [docs/display-driver.md](docs/display-driver.md) | EVDI + t2d design, measurements, limits |

Key facts, in brief:

- The firmware parses EP2 **per USB transfer**. Command headers must be their own
  transfer; payload goes in separate transfers.
- Frames go into one of two framebuffers (`0x00C000`, `0x1CE000`). A `0x15`
  register batch flips scanout.
- `0x13` payload: planar RLE in 1024-pixel units (R, G, B lines). A literal byte is one pixel;
  `30 NN VV` = `NN+1` × `VV`.
- The panel scans out **rotated 180°**.

## Repository layout

- `t2d/`: display daemon (C), init blob, panel EDID
- `nix/`: flake packages, NixOS module, EVDI patch
- `tools/`: Python protocol tools. `t2rle.py` (codec), `replay.py` (init replay + test
  frames), `show_image.py`, capture parsers (`tee.py`, `parse_redir.py`,
  `redir_ops.py`, `timeline.py`), RE helpers (`xref.py`, `locfunc.py`, `vft.py`)
- `captures/redir2.bin`: full usbredir capture of the Windows init (source of `t2d/init.bin`)
- `data/`: EDID, device info blob, register sweep
- `vm/`: Windows VM recipe for re-capturing (vendor driver not included)

Python tools: `nix develop` (or `pip install pyusb pillow`).

## Legal

Interoperability research on hardware I own. No vendor code or binaries are
included; the vendor driver is only referenced by function address.
GPL-2.0 (see `LICENSE`). The EVDI patch applies to MIT-licensed EVDI.
