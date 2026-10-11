# Trigger 6 lab notebook

Running log of the T6 work, newest at the bottom. Protocol facts that hold up
go into [protocol-t6.md](protocol-t6.md); this file keeps what was tried, how,
and what came out, including the dead ends. Raw material lives in the lab
directory (`captures/t6-device/`, `captures/t6-analysis/`,
`third_party/trigger6/`).

Ground rules: the T6 work must not change the T2 driver (`mct_t2.c`) until a
merge that is retested on the DTU-1031X. Only one thing talks to the T6 at a
time. Never send a USB reset to a stuck MCT chip (on the T2 it made the chip
drop off the bus until a power cycle).

## Setup

- Adapter: "T6 USB Station", USB `0711:5600`, bcdDevice 0x1010, SuperSpeed
  5 Gbit/s, 248 mA. One vendor interface: bulk OUT 0x02 and bulk IN 0x81
  (1024 B, bMaxBurst 15), interrupt IN 0x83 (64 B). No audio or network
  functions. Dump: `captures/t6-device/lsusb-v.txt`.
- Monitor: LG 27GN750 (EDID name "27GN7", made week 8/2020), a **1920x1080
  240 Hz panel**, on the adapter's HDMI port. EDID 1.3 with a CTA extension,
  `captures/t6-device/lg-27gn7.edid` and `lg-27gn7-edid-decode.txt`. HDMI VSDB:
  max TMDS clock 600 MHz (HDMI 2.0 class). Preferred timing 1920x1080 at
  239.76 Hz (577 MHz), also 1920x1080@120 (VIC 63) and @60. The EDID also
  lists 3840x2160 VICs (93-97): the monitor accepts a 4K signal and scales it
  down (common on LG gaming monitors for consoles); the panel is not 4K.
  Range limits 48-240 Hz.
- The T6's own mode table lets through only modes up to **1920x1080@60**
  (plus 50/59.94 Hz, 1280x1024, 1280x720, 1024x768, 800x600, 640x480 at 60/75).
  Nothing above 60 Hz or above 1080p. DRM state: `captures/t6-device/drm_info-t6.txt`.
- Host: NixOS, linux-cachyos 7.2.7, niri 26.04 (with our device-removal
  patch), two AMD GPUs (RX 9070 primary, iGPU), Mesa via nixpkgs.
- Build: kernel modules are built against the dotfiles flake's kernel
  (`builtins.getFlake "git+file:///path/to/system-flake"`, then
  `config.boot.kernelPackages.kernel`); `nix/kmod-local.nix` still targets
  `linuxPackages_latest` and doesn't match the CachyOS kernel.

## 2026-10-10

**First contact.** No driver bound. Built
[trigger6](https://github.com/JingYenLoh/trigger6) after reading it (about
800 lines, plain USB control and bulk, nothing odd). It only knows PID 0x5601;
added 0x5600.

**Connector read "disconnected".** Status request `0x87` returned 0 until the
LG was awake and on the right input, then 1. Output index 0 on the 5600.

**niri wouldn't enable the output** ("No supported plane buffer format
found"). Tried in order:
1. Frames went to bulk endpoint 4; the OUT endpoint is 2. Fixed (needed for
   any picture), not the niri problem.
2. Added `DRM_FORMAT_MOD_LINEAR` to the plane. Not it either; `drm_info`
   showed IN_FORMATS was already fine.
3. Ran `modetest -M trigger6 -s 35@38:1920x1080` from a text console: colour
   bars on the LG, so the raw BGR24 path works.
4. Read smithay 0.7 / niri 26.04 source (vendored in the Nix store): the error
   is the one for the last colour format tried (ABGR8888), not the real cause.
   Same kernel, GPU and niri as when the T2 worked, so compared the drivers:
   trigger6 overrides `gem_prime_import` with the old `drm_gem_prime_import_dev`
   USB workaround, while 7.2's shmem helpers use
   `drm_gem_shmem_prime_import_no_map`. Dropped the override: niri lit up the
   LG. Fixes on lab branch `third_party/trigger6` `t6-5600-fixes`.

Result: the desktop shows, but it lags. trigger6 resends the whole screen as
6.2 MB of raw BGR24 per update and makes the commit wait for the previous
frame.

**Windows capture decoded** (subagent, `captures/t6-analysis/`): Windows
sends JPEG only, full (type 4) and partial (type 7) frames, a ring of
destination addresses from 0x03000000, frame-done notifications on EP 0x83,
and cursor/gamma/audio requests. Details in protocol-t6.md.

**mct_t6 0.1** (branch `t6`): own driver in the mct_t2 mould (worker, shadow,
latest image wins, stats), full raw frames with trigger6's header values,
synchronous sends. Builds clean, checkpatch clean. Live result: **bad**.
Glitches at start, the bottom of the screen cut off, worst with the output
rotated 90 degrees in nwg-displays. Mistake on my side: it went to a live
test as a first draft. Likely cause: one device buffer for every frame
(dest 0x30, start = end = 0x60) and slower synchronous sends, so the panel
shows half-written frames. Next steps: work out the ring addressing and test
it from userspace with still images first, keep trigger6 as the fallback.

Started two research subagents: a deep pass over the capture for the exact
addressing / partial update / timing rules, and a web search for other T6
work, OEM driver packages and the chip's formats.

**t6dev.py, first live session** (mct_t6 unbound, device node opened up with
chmod for the session).

- Status: output 0 = 1 (LG), output 1 = 0.
- Device info: `b0` index 0 `01000000`, 1 `99280616`, 2 `01140917`,
  3 "JUA254H" (chip/board name; the 5601 in the capture says "A8613HV"),
  4 `06000000`; `b4` `04000000`; `88` `3a`.
- Mode table: 30 entries from 640x480@60 to **2048x1152@60**, highest pixel
  clock 197 MHz (1920x1200@60 193.25 MHz, 2048x1152@60 197 MHz). No entry
  above 75 Hz. 1080p120 would need 297 MHz. Besides the timings, each entry has
  three more 16-bit fields: `u1` is always 1000; `u0` and `u2` change per mode
  (e.g. 148.5 MHz: 699 / 285, 74.25 MHz: 699 / 541, 25.175 MHz: 140 / 20), probably
  clock generator settings, which a custom mode would have to get right.
  1360x768 and 1680x1050 have 512 in the last field, 1600x900 has 1.
- Full raw frames with trigger6's header values (`send_raw`): two still test
  images (yellow bars to tell them apart; the LG stands rotated 90 degrees, so
  the bars show at the top) arrived complete. 6.2 MB each in 17-18 ms, about
  350 MB/s, from Python.
- Continuous: a white block moving across 24 positions, sent back to back for
  6 s: **316 frames, 52.5 fps, 327 MB/s**. Frame-done notifications came back
  on EP 0x83 (`04 00 ...` with the frame counter at byte 12; the first one after
  mode set was `20 00 ...`, then `24 00 ... 05`).
  On the LG: motion fairly smooth, but **tearing at the sides of the block**.
  This confirms the single-buffer explanation for mct_t6 0.1's cut-off
  bottom: with one destination every frame overwrites the visible one. USB
  bandwidth is not the problem; full raw 1080p frames nearly reach 60 fps.

**Capture decoded properly** (subagent, full report kept in the session
scratchpad; the rules are now in protocol-t6.md): session `dest` is where the
USB data lands, video header `start`/`end` are the plane addresses the T6
scans out from (Y and UV for NV12), the "unknown" dword 3 is the surface
format (6 NV12, 9 BGR24), the 0x550 dword holds the two plane pitches.
Windows rotates full frames through three buffers (0x017ffe30, 0x01fffd90,
0x027ffcf0, 0x7fff60 apart) and writes partial (type 7) updates into the one
on screen, with the position encoded in the addresses. trigger6's dest 0x30 +
48-byte header puts the pixels exactly at start 0x60: it always writes into
the visible picture.

**Triple buffering, raw BGR24**: same moving block, frame n into Windows'
buffer n % 3, `dest = start - 0x30`, `start = end = buffer`. 315 frames in
6 s, 52.5 fps. On the LG: tearing **less often but sharper**, motion as
smooth, no garbage. Reading: the T6 now shows only complete frames, but it
switches to a new frame as soon as it has arrived, not at the start of a
refresh, so a switch in mid-scan is one hard cut between two whole frames.
Open: is there a vsync-synchronised flip (a header flag or a request)? Waiting
for the static analysis of T6InDisp.dll before trying unknown fields.

**NV12 works.** Still image: test card converted to NV12 (BT.601 limited
range, 4:2:0, Y plane then interleaved Cb/Cr), video header
`3, len, seq, 6, W | W << 16, start = buf, end = buf + W*H, 0, 6`, session
`dest = buf - 0x30`. Acked, and on the LG the picture and colours were
correct (dark blue, white border, gradient, yellow bars).

**NV12 motion, triple buffered**: the same moving block, frames precomputed,
3.1 MB each: **589 frames in 6 s = 98 fps, 305 MB/s**. On the LG: no tearing
at all, colours and motion right, smoother than raw, no ghosting or
artifacts. Raw BGR24 into the same three buffers tore (hard single cuts), so
the chip seems to flip NV12 surfaces properly and raw ones immediately; NV12
is the surface format the vendor driver decodes into.

Conclusion for the driver: convert to NV12 (half the data of BGR24) and send
full frames round robin into three buffers. 1080p60 needs ~187 MB/s, well
inside the measured ~300 MB/s. Cost: 4:2:0 chroma, so colour edges are
softer than raw (fine detail in coloured text). A lossless option would need
raw frames with proper flips, still open.

**Flip rules and the frame recipe.** Slow-sent test frames (one fragment every
100 ms): with the vendor driver's exact recipe (session `dest = fb`, video
header data_len = image bytes only, planes at fb + 0x30, the `more` flag, 0x31
"software ready"), a green frame sent over 3 s appeared all at once. 0.2 had
data_len 48 bytes too large; the chip then switches only when bytes of the
next frame arrive (cursor one update behind). Research reports saved in the
lab: `captures/t6-research/` (capture analysis, DLL analysis, web research)
and `third_party/t6-sources/` (MCT's Linux source, other drivers, macOS and
Windows packages).

**mct_t6 0.3.x live** (cachyos 7.2.7, niri, LG rotated 90 degrees):
- 0.3.0 (vendor recipe): half the screen still broken, cursor buggy.
- 0.3.1: copy the **whole** framebuffer instead of the damage clips. Whole
  screen right. With the rotated output, niri's damage clips did not cover
  what changed; to look into before using damage again.
- 0.3.1 stats: convert 1.66 ms, USB 8.85 ms per frame (351 MB/s while busy),
  yet only ~10 updates/s and laggy.
- 0.3.2 adds a timer around the framebuffer copy in the commit: **87.5 ms per
  copy** (8 MB at ~95 MB/s). amdgpu shares the buffer write-combined, where
  plain CPU reads crawl. That was the whole lag, and probably trigger6's too.
- 0.3.3 copies with `drm_memcpy_from_wc()` (SSE4.1 streaming loads):
  **5.85 ms per copy** (max 6.96), 45 updates/s while moving things, convert
  1.84 ms, USB 8.62 ms. "Finally good, really good now", some latency left.

Next: hardware cursor (requests 0x10/0x04/0x05 decoded), copy and send only
what changed (fix damage handling for rotated outputs first), overlap the
stages.

**Hardware cursor, userspace test.** Output 0: upload with request 0x10
(wValue slot 0, wIndex byte offset, 512-byte chunks) of an 8-byte header
`{u16 kind 1 = ARGB with alpha, u16 w 64, u16 h 64, u16 pitch 256}` plus B, G,
R, A pixels, show with 0x05 (wValue slot, wIndex 1), position with 0x04
(wValue x, wIndex y, top left of the box). A red square circling the screen
at 120 position updates per second: shown and moving smoothly. mct_t6 0.4
adds it as a 64x64 cursor plane (position clamped at 0 by shifting the image
instead, as on the T2).

**Refresh rates above 60 Hz** (`tools/t6_refresh_test.py`). The mode entry's
last fields are MCT's PLL settings: base 40 MHz on this "Super Lite" T6
(`b0` index 0 = 1); x = first of 4, 8, 16, 32 with f*x >= 800 MHz,
N = floor(f*x / 40), P = fraction * 1000, Q = 1000, OutputSelect = {3,1,2,0}[i].
Computing the 1080p60 entry this way reproduced the device's own entry byte
for byte. Custom entries sent with 0x12, then 0x31 and output on, then the
NV12 moving block for 8 s, LG OSD read back:

| Mode | Pixel clock | Result |
|---|---|---|
| 1080p120 (CTA VIC 63 timing) | 297.0 MHz | picture, OSD "1920x1080 120Hz", smooth, 97.5 fps sent |
| 1080p144 (reduced blanking, 2000 x 1157) | 333.2 MHz | picture, smooth, 94-98 fps sent |
| 1080p150 (RB, 2000 x 1161) | 348.3 MHz | picture, fine |
| 1080p160 (RB, 2000 x 1166) | 373.1 MHz | no signal |
| 1080p165 (RB, 2000 x 1169) | 385.8 MHz | no signal (OSD briefly "0Hz") |

Narrowed down afterwards:

| Mode | Pixel clock | Result |
|---|---|---|
| 1080p152 (RB, 2000 x 1162) | 353.2 MHz | picture, but thin black bars at fixed horizontal positions, moving block passes through them |
| 1080p155 (RB, 2000 x 1163) | 360.5 MHz | no signal |
| 1080p60 with wide blanking (5230 x 1125) | 353.0 MHz | same artifacts as 152 Hz |

So the limit is the **pixel clock, about 350 MHz**, independent of refresh
rate: clean at 348.3 MHz, artifacts at 353, nothing from 360. Either the PLL
(running at 4x the pixel clock in this range, ~1.4 GHz) or the HDMI
transmitter tops out there; not separable from the outside. Practical
maximum for 1080p: 150 Hz; 144 Hz (333 MHz) leaves margin. Product sheets only claim up to
2048x1152@60 (197 MHz). Reduced blanking used: 80 px horizontal blanking
(front 8, sync 32), vertical blanking >= 460 us (front from the rest, sync 8,
back 6), hsync +, vsync -. Back at 144 Hz afterwards, stable.

USB still limits full-screen raw motion to ~95 fps (NV12, 3.1 MB per frame);
higher refresh rates pay off with partial updates.

**720p at 240 Hz**: reduced blanking 1360 x 810, 264.4 MHz. Picture, LG OSD
"240Hz", smooth; **195 fps** of NV12 frames (1.4 MB) sent from Python. Since
the limit is the pixel clock, lower resolutions go faster: anything up to
~350 MHz works (1080p up to 150 Hz, 720p up to the monitor's 240).

**mct_t6 0.5**: mode entries are computed from the DRM mode (PLL formula
above, base clock from `b0` index 0) instead of looked up in the device's
table, capped at 350 MHz, widths a multiple of 16. Besides the EDID's
modes it offers reduced-blanking modes at 75, 100, 120, 144, 150, 165, 200
and 240 Hz for the monitor's preferred size and for 1280x720, where the
EDID's range limits and the clock cap allow (1080p: up to 150 Hz).

**0.5 at 120 Hz judders** (stats over 104 s): convert 1.92 ms + USB 8.65 ms per
full frame = ~10.6 ms against an 8.3 ms frame period; 68 skipped images.
USB is at its ceiling (359 MB/s while busy), so the fix is sending less:
partial updates.

**Partial update primitives** (`tools/t6_partial_test.py`), all confirmed:
1. full frame into buffer 0: blue; 2. full frame into buffer 1: red;
3. **empty flip** (video header type 3 with data_len 0, session total 48,
   dest = buffer 0): blue again;
4. **session type 7 rectangle write** into buffer 1, not on screen: 32-byte
   header `{7, data length, Y address, UV address, u16 Y pitch, u16 UV pitch,
   u16 w, u16 h, 0, 0}` + Y rows + interleaved UV rows (a green band, rows
   400-679, 806 KB in one session): screen unchanged (blue);
5. empty flip to buffer 1: red with the green band.
So a partial update = write the changed rows into the next buffer with
session 7, then an empty flip to it.

**Partial updates at full speed** (`tools/t6_partial_motion_test.py`, the
driver's scheme in Python: diff the new NV12 image against a copy of the
target buffer by row pairs, session-7 writes of the changed bands, at most
~100 KB each, then the empty flip, no pauses): a 300x300 block moving up
and down, **325 frames per second** (104 rows per frame on average), clean
on the LG twice. Flips sent right after the writes wait for them.

**mct_t6 0.6**: partial updates as above. NV12 copies of all three frame
buffers on the host (3 x 3.1 MB at 1080p), bands merged across gaps of up
to 8 row pairs, at most 16 bands. The first frame after a mode set is sent
whole. Stats gain rows and bands sent.

**0.6/0.7 in daily use.**
- 1080p 144 Hz was missing from the mode list: the LG's EDID has its own
  1080p 143.98 Hz timing (DTD 3) at 350.39 MHz, rejected by the clock cap
  but still counted as "already there", hiding the 333 MHz reduced blanking
  mode. 0.6.1 only counts modes the T6 can drive.
- 1080p 150 Hz (348.3 MHz): fine in the short test, but **tiny green pixel
  artifacts** in daily use. 0.7.1 caps at 340 MHz; 1080p144 (333 MHz) is
  the top mode and stays clean.
- Cursor sometimes scrambled after a shape change, fixed by moving the
  pointer to another monitor and back. Cause: the new image was uploaded
  into the slot on screen and no show request followed. 0.7 uploads into
  the other of two slots and then selects it (show request with that slot),
  like the vendor driver's slot handling.
- smithay 0.7 (niri 26.04) computes FB_DAMAGE_CLIPS for a rotated output in
  rotated coordinates (`PlaneDamageClips::from_damage` maps with
  `Transform::Normal`), the kernel clips away what lies outside the
  framebuffer, so half the screen stayed stale with damage-based copies.
  Upstream report to write. 0.7 marks the rows a raw clip covers under every
  rotation/flip. First stats at 150 Hz: still ~whole frames copied per
  commit (6.7 ms); 0.7.1 counts clips to see why.

**0.7.1 stats (light use, 144 Hz)**: 0 commits without clips, ~2 clips per
commit, ~1.14 M px of clips per commit (half the screen), 1057 rows copied
per commit, 513 rows sent per update. The LG stands rotated: a line of text
in portrait is a narrow column strip of the landscape framebuffer, covering
all rows, so row-based copying and sending can't save anything there.

**Column-limited rectangle writes** (`tools/t6_rect_test.py`): session 7 with
x offset 600 and width 300 (Y address = base + y*W + x, UV address = base +
W*H + (y/2)*W + x, pitch W) into the hidden buffer, then an empty flip: a
clean green rectangle.

**mct_t6 0.8**: the diff finds the first and last changed column per row pair
(rounded out to 16 pixels) and sends each band as a rectangle of just those
columns; the framebuffer copy does one `drm_memcpy_from_wc()` per run of rows
instead of per row (each call saves and restores FPU state).

**0.8 stats (31 s, 144 Hz, "works good")**: 57.8 updates/s, 6 skipped;
1.35 MB per update over USB (full NV12 frame: 3.1 MB), USB 4.49 ms and
convert 1.73 ms per update. niri's damage clips: ~1.35 M px per commit (two
thirds of the screen), so the copy still takes ~1063 rows per commit:
**7.34 ms** (one call per run of rows changed nothing, so it is bandwidth,
~1.1 GB/s). Likely the scanout buffer sits in the RX 9070's VRAM and is read
over PCIe. Ceiling for full-screen motion at 1080p144: ~135 fps.

**Why the framebuffer copy costs 7 ms** (`tools/gbm_read_bench.c`: allocate a
1920x1080 XRGB8888 LINEAR buffer through GBM the way niri does for a
display-only device, mmap the dma-buf, time CPU reads; no device or root
needed):

| GPU (render node) | plain memcpy | SSE4.1 streaming loads |
|---|---|---|
| RX 9070 (renderD128), RENDERING\\|SCANOUT | 109.5 ms (0.08 GB/s) | 7.0-13.0 ms (0.6-1.2 GB/s) |
| Raphael iGPU (renderD129), same flags | 10.2 ms | **0.77 ms (10.7 GB/s)** |

The RX 9070 buffers sit in VRAM and are read over PCIe; that is exactly the
87 ms (plain) and 7 ms (streaming) seen in the driver. System memory reads
10x faster.

amdgpu (amdgpu_dma_buf.c, 7.2): `begin_cpu_access` would move a buffer to GTT,
but only if it isn't pinned (the shadow plane's vmap pins it where it is)
and only within the display domains (VRAM only on a dGPU). An importer that
attaches and maps without peer-to-peer access gets the buffer pinned in GTT
instead (`amdgpu_dma_buf_pin` drops VRAM for non-p2p attachments).
`drm_gem_prime_import_dev()` does that, but needs the driver's
`gem_prime_import_sg_table` hook: trigger6 overrode `gem_prime_import` with
it but never set the hook, so every import failed with -EINVAL, which is
why niri couldn't use the T6 at first.

**mct_t6 0.9**: imports through a DMA attachment as the USB host controller
(`sysmem` module parameter, default on, runtime writable; falls back to the
no-map import if the attachment fails), `gem_prime_import_sg_table` set to
the shmem helper. Stats count attached vs. mapped imports. Untested live.

The rotated damage clips are a bug in smithay 0.7.0 (vendored by niri 26.04):
`PlaneDamageClips::from_damage` maps damage with `Transform::Normal`. smithay
master already takes `dst_transform` / `damage_transform` there, so it is
fixed upstream; note in the lab's `upstream/smithay-rotated-damage-clips.md`.

**Code review of 0.9** (subagent, read-only). Fixed in 0.9.1:
- The default commit tail updates planes before enabling the CRTC; the
  enable resets the frame buffer and cursor bookkeeping, so the first frame
  and the cursor after a mode set or resume were dropped (idle screens stayed
  black or garbage after resume). Own commit tail: disables, enables, planes.
- Framebuffers larger than the mode, panned (src x/y) or with an offset were
  accepted but never shown; now rejected in atomic_check.
- A failed `begin_cpu_access` lost that commit's damage for good; the shadow
  is now marked invalid so the next commit copies everything.
- `drm_kms_helper_poll_init` before a failing `drm_dev_register` left the poll
  work running on freed memory; now `drmm_kms_helper_poll_init`.
- Hardening: mode table entries above 4096x2304 ignored and the band step
  can't be 0 (a bogus table could spin the worker); reduced blanking clock
  computed in u64; the worker takes the USB lock before the image lock so a
  commit never waits behind a cursor upload or a connector poll; mappings
  handled as iosys_map (I/O memory on a plain VRAM import); no attached
  imports through host controllers in removable docks (USB4/Thunderbolt,
  the mapping could outlive the controller).
The attached import itself checked out (references, fallback, release order).

**0.9.1 live: grey screen, attached import is a dead end with amdgpu.** niri's
page flips failed with EINVAL, and the kernel warned in `dma_buf_vmap`
(called from `drm_gem_fb_vmap` in `prepare_planes`): amdgpu's vmap pins the
buffer first, and `amdgpu_bo_pin()` narrows the requested domains to the
buffer's preferred ones (VRAM for niri's scanout buffers); the attachment had
already pinned it in GTT, so the pin returns -EINVAL. In short: through the
regular interfaces an importer can't have an amdgpu dGPU buffer pinned in
system memory *and* vmapped. Mapping the GTT pages from the sg table
ourselves would work but breaks the dma-buf rules (importers must not use
the pages), so not for a driver meant to go upstream. 0.9.2 drops the
attached import (back to the no-map import) and keeps the review fixes.
What remains for the 7 ms: smaller copies (damage), or the compositor
rendering into system memory to begin with.

## 2026-10-11

**Gamma** (`tools/t6_gamma_test.py`): feature level (`b0` index 4) is 6 on
the 5600, and the vendor driver only uses gamma from 8. The first `40 1d`
gamma table request **stalls** (EPIPE): no hardware gamma on this chip. Plan:
apply the DRM gamma LUT in software during the NV12 conversion.

**Monitor change events** (mct_t6 0.10, interrupt transfer kept queued on EP
0x83, byte 0x13 bits 0/1 -> connector check): unplugging and replugging the
LG's HDMI cable was noticed by niri "pretty much instantly" both ways (the
10 s polling stays as a fallback).

**Software gamma** (mct_t6 0.11: DRM GAMMA_LUT, 256 entries, applied per
pixel during the NV12 conversion): Noctalia's night light works on the LG,
no noticeable slowdown.

**mct_t6 0.12, queued bulk transfers**: a pool of 16 URBs (100 KB coherent
buffers each); band data is copied straight into a free URB and submitted
without waiting, so the worker converts and diffs the next frame while the
previous one is still going out, and the endpoint never idles between
transfers. The bulk endpoint keeps the order (writes before their flip).
A failed transfer invalidates all three buffer copies (next update sends
whole frames); disable/suspend/unplug wait for or kill what is in flight.
"usb" time in the stats is now the time spent queueing (backpressure).

**YUV 4:4:4** (`tools/t6_yuv444_test.py`; surface format 11, planes Y, U, V
at full size, pitch align32(width), each following the previous): a detail
image (1-pixel coloured lines, red strokes on blue) looked crisper than NV12,
"especially green and blue"; a moving block at full speed (6.2 MB frames,
52.8 fps) showed **no tearing**, like NV12.
- Partial writes: the video command type 7 (FB_DIRECT) with raw 4:4:4 data
  through a staging address (0x3000000) did nothing (likely JPEG only).
  Session 7 does two strided copies (h rows to the "Y" address, h/2 rows to
  the "UV" address), so two writes per band cover three planes: Y rows with
  the first half of the U rows, V rows with the second half. A green
  rectangle written that way into the hidden buffer appeared on the flip.

**mct_t6 0.13**: image layout abstraction (NV12 or 4:4:4), module parameter
`yuv444` (default on, runtime writable; switching resends whole frames).
Development now happens in a worktree (`../mct-t2-linux-t6`) so the main
checkout, which the dotfiles build, stays on main.

**4:4:4 live (0.13)**: at 1080p144 thin, sparse rainbow vertical lines on a
still image; at 120 and 60 Hz clean. Scanout reads the buffer from the T6's
memory every refresh: 4:4:4 1080p144 is ~900 MB/s, 120 Hz ~750 MB/s, NV12
1080p144 ~450 MB/s. 0.13.1 uses 4:4:4 only up to 760 MB/s. In 4:4:4 window
moves felt a bit laggy (twice the data per changed area over USB).

**mct_t6 0.14, adaptive colour** (`yuv444=2`, default): 4:4:4 at rest,
NV12 while updates are large (diff covers more than a quarter of the screen,
measured only against buffers whose content is known, so refilling after a
switch doesn't count), back to 4:4:4 150 ms after the last large update
(timer resends the image). `yuv444=1` forces 4:4:4, `0` NV12; the bandwidth
guard applies to both 4:4:4 settings.
