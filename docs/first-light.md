# First light (2026-09-24)

The DTU-1031X panel shows solid colours sent from Linux: red, green, blue,
white, full screen (`tools/replay.py`).

## Symptom before
The init replay turned the backlight on, but every `0x11` fill and `0x13` frame
we sent left the panel blank: red fills to 4 candidate addresses, RGB-raw frames,
and a verbatim Windows RLE frame. All transfers completed OK (no stalls).

## Root causes (both needed)

1. **Transfer framing.** The T2 firmware takes a command from each USB transfer
   boundary. Windows always sends the 21/36/23-byte header as its own
   transfer, then the payload in chunks. We concatenated header+payload
   into one `write()`, and the firmware silently dropped it.
   - Also: `tools/gen_trial2.py` (script not kept) dropped the `0x13` headers but still sent
     their payloads (as zeros), which desynced EP2 further.
2. **Missing flip.** Scanout only changes on a `0x15` batch on EP2 that
   rewrites `fcb1..b3` (scanout addr >> 2) and toggles `fb65`/`fcb4`.
   Windows sends one after every frame (double buffering A/B).

The init sequence itself was correct all along.

## How it was found
- Full timeline of `captures/redir2.bin` including *all* EP2 commands (the old
  timeline only printed `0x11`/`0x13`) -> exposed the `0x15` flips.
- Hot-attached the tablet to the still-running Win11 VM through
  `usbredirserver` + `tools/tee.py` (QEMU monitor `chardev-add` +
  `device_add usb-redir`) -> `captures/redir4.bin` with a real desktop window on
  the tablet. EP3 had nothing new after `0x14`, so the difference had to be in how
  EP2 was sent -> transfer framing.
- Detaching the tablet from the VM (killing usbredirserver) drops it out
  of scanout (backlight off, writes crawl ~38 s). Full re-init is required after that.

## Experiment log
| Trial | What | Result |
|---|---|---|
| trial_init / trial2 | init + `0x11` red fill (merged writes) | lit, blank |
| replay3 v1 | verbatim redir2 + `0x11` fills + flips (merged writes), no power-cycle | lit, blank |
| rle_test v1 | RLE R/G/B/W + flips, merged writes | blank |
| rle_test v2 | split writes, after VM detach | backlight **off** (device lost state) |
| **replay3 v2** | power-cycle, verbatim redir2 (per-transfer), RLE R/G/B/W split + flips | **RGB flash, black, R, G, B, W (OK)** |

## Colour order
RLE channel-lines per row are **R, G, B** (ch0 = red verified).

## Arbitrary images (same day)
- The RLE encoder was located in `T2Indisp.dll` (`0x180013278`) and read from disassembly: a literal byte is a pixel,
  `30 NN VV` is a run. `tools/t2rle.py` encode/decode re-creates Windows' frames byte for byte.
- Decoding a captured desktop frame gave the Windows wallpaper rotated 180 degrees.
  A corner test pattern confirmed that the **panel is rotated 180 degrees**.
- `tools/show_image.py <photo.png>` -> photo shown upright and
  correct on the tablet. **Windows is no longer needed**: init comes from the
  saved capture and the frames are encoded natively on Linux.

## Next
- Minimise init; write a streaming daemon (screen grab -> RLE -> `0x13` + `0x15`).
