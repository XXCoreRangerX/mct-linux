# Trigger 6 lab notebook

> **History**: a chronological record. Entries are not rewritten; later findings are added after them. For the current state see the reference docs ([docs/README.md](../README.md)).

Running log of the T6 work, newest at the bottom. Protocol facts that hold up
go into [protocol-t6.md](../reference/protocol-t6.md); this file keeps what was tried, how,
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
  functions. Dump: `captures/t6-device/lsusb-v.txt`. EDIT (2026-10-12): no
  USB audio *interface*, but T6 audio never uses one; see the 2026-10-12
  entry at the end.
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
- Build: kernel modules are built against the host's own NixOS kernel
  (the system flake's `config.boot.kernelPackages.kernel`); `nix/kmod-local.nix` still targets
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
checkout, which the system configuration builds, stays on main.

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
0.14 live at 120 Hz: drags snappy, text sharpens quickly after stopping.

**Packed formats** (`tools/t6_formats_test.py`, 1080p60, vendor recipe:
plane 0 = plane 1 = fb + 0x30, pitch in bytes, second pitch 0):
- RGB32 (8, B G R X, pitch w*4): image correct, no tearing, 40 fps full frames.
- BGR24 (9, B G R, pitch align32(w)*3): image correct, no tearing, 53 fps.
  (The early BGR24 tearing came from the wrong header length, not the format.)
- YUYV (0, Y0 U Y1 V, pitch w*2): broken; the bottom half of the detail image
  stayed plain background and the moving block glitched at top and bottom.
  The layout the chip expects for format 0 is unknown (the vendor uses it as
  a JPEG 4:2:2 decode target); left open.
BGR24 needs the same bandwidth as YUV 4:4:4 and is exact, so it is the
better full-quality format.

**mct_t6 0.15**: module parameter `format` replaces `yuv444`: auto (BGR24
at rest, NV12 during large changes; default), rgb24, rgb32, yuv444, nv12;
the full colour formats only where the scanout bandwidth allows (otherwise
NV12). Layouts carry bytes per pixel; packed RGB partial updates are
session 7 writes with both parts on the same plane (3n rows per write).

**0.15 live (format=auto)**: glitches at 120 Hz ("pretty bad"), at 100 Hz
"kinda eh", fine at 75 Hz; confirmed with format=rgb24: glitches from 100 Hz,
75 Hz fine. So RGB24 scanout tops out lower than YUV 4:4:4 at the same
3 bytes per pixel: ~470 MB/s fine, ~620 MB/s not. Also: switching between
RGB at rest and NV12 in motion made images "go grey then normal": the YUV
conversion was BT.601 limited range, the vendor converts full range (JFIF),
so NV12 had always been slightly washed out.

**mct_t6 0.16**: YUV conversion in full range (max error 1 against JFIF
floating point); per-format scanout limits (RGB 500 MB/s, YUV 4:4:4
760 MB/s, NV12 unlimited); auto picks the best full colour format that fits
the mode (1080p: RGB up to 75 Hz, 4:4:4 at 100-120 Hz, NV12 at 144 Hz) and
uses NV12 during large changes.
0.16 live at 1080p120 (auto: YUV 4:4:4 at rest, NV12 in motion): "works
great", smooth, no grey flash on switches, colours look richer.

**Robustness (0.16)**: niri monitor off/on, suspend/resume (deep; xHCI
reinit, USB reset of the T6, picture back) and unplugging/replugging the
adapter all worked; the log shows only two "frame update error: -19" at the
unplug (now not logged).

**mct_t6 0.17, less CPU per update**: only shadow rows changed since the
last conversion are converted (the commit's copy marks them), and each frame
buffer keeps a mask of rows that may differ from the image, so the diff only
looks at those.

**0.17 stats** (68 s at 1080p120, auto, niri with the display-only sysmem
patch): framebuffer copy **0.19 ms** per commit (was 7 ms with niri's
buffers in VRAM), USB queueing 2.19 ms and convert + diff 2.45 ms per
update, 51 updates/s, 27 switches to NV12 for motion, no stale areas. Rows
copied per commit still ~1037: with the output rotated, the clip workaround
marks nearly all rows. "0 frame done" events: the T6 sends frame-done only
for flips that carry image data, and the driver now only sends empty flips
after its rectangle writes (the Python partial-update tests saw no acks
either). Monitor change events are unaffected.

**0.17.1**: for framebuffers in our own (cached) memory, i.e. with the niri
patch, damaged rows are compared with the shadow and only rows that really
changed are copied and converted. Imported GPU buffers keep the plain copy
(reading them twice would be slow).
0.17.1 stats (46 s light use, 1080p120 auto): **65 rows changed per
commit** (was 1037), convert + diff **0.26 ms** per update (was 2.45), USB
1.49 ms and ~0.64 MB per update, framebuffer copy 0.29 ms. No stale areas.

**YUY2 / surface format 0, investigated** (`tools/t6_yuy2_hunt.py` and the
`tools/t6_yuy2_*.py` scripts; the LG stands rotated, so the user's "bottom"
is the right side of the panel):
- Byte order: packed Y0 U Y1 V is right (variant with Y in the odd bytes,
  and NV16/NV61 planar layouts, came out inverted/broken; YVYU swaps red
  and blue).
- At 1920 wide only about the first 60% of each row (~1150 px, ~2300 bytes)
  scans out right; beyond that the panel shows stale line buffer data, seen
  as content smeared along the full panel height (moving block). Not
  changed by plane 1 / pitch 1 settings, by the pitch field (ignored for
  this format), or by writing the frame with session 7 writes instead of
  the flip's payload.
- At 1024x768 (2048 bytes per row) the rows are fine, but motion tears
  badly, also across rows: flips of this format are not synchronised.
Conclusion: format 0 is not usable for scanout (the vendor only uses it
internally as a JPEG 4:2:2 decode target). Formats in the driver: auto,
rgb24, rgb32, yuv444, nv12.

**One module (mct 0.4.0, branch mct)**: the pending T2 branches (resync
0.3.17, counters 0.3.18, adaptive-lossy 0.3.19) merged cleanly into the T6
line; `mct_main.c` registers one USB driver for all four IDs and hands each
device to its backend (mct_t2.c / mct_t6.c, still separate DRM drivers with
their own parameters lossy / format). Live: tablet and T6 both running on
the one module at the same time, picture, cursor, brightness slider, window
drags and the T6's 120 Hz auto mode all fine.

## 2026-10-11: pacing, rotation, larger monitors

**Frame-done pacing, checked before building it.** The idea was to drive
the vblank timer from the chip's frame-done events. The vendor capture
already answers most of it (`captures/t6-research/capture-analysis.md`,
section 3): done events come 0.9-11 ms after the last data, depending on
decode work, and their phase against the 60 Hz refresh is spread out
(R = 0.11). They mean "frame processed", not "flipped at vblank", and empty
flips (all partial updates) send none. So there is no vblank signal to lock
to. `tools/t6_pacing_test.py` measures it with raw frames on our
side (rate when sending back to back, latency, phase R) to close it for
good. What remains possible without a vblank signal is small: the timer
already runs at the mode's nominal rate, and the PLL rounding (P / 1000
steps) is ~30 ppm, below a crystal's tolerance; a one-frame drift every
few minutes at 144 Hz is not visible.

Result (`tools/t6_pacing_test.py`, 1080p60, full NV12 frames with the
driver's header recipe): 223 frames in 4 s back to back, **0 frame-done
events**, and none for spaced frames either. With the current recipe the
chip doesn't report done even for flips with image data (the early lab
header with the image length counting the header did get them; the
vendor's JPEG frames do). Either way there is no vblank signal: pacing
stays on the timer. Item closed.

**Hardware rotation (0.18.0, branch t6-rotation, not usable here).** From
the vendor's Ubuntu driver (`evdi_t6_1/main.c`, `t6usbdongle.c`): `40 1f`
with wIndex 0..3 = 0/90/180/270 (xrandr's counter-clockwise rotations), the
host sends the image in the rotated (portrait) size with the pitch of that
width, and the mode entry keeps the monitor's timings but carries the
portrait size in the active width/height fields. Cursor positions and
images go to the chip unchanged in image coordinates, so the chip likely
turns the cursor with the picture. Driver: rotation property on the
primary plane, a new rotation is a mode set, portrait buffers allowed, the
cursor box and image are mapped from screen to image coordinates.
Userspace test first: the 5600 answers `40 1f` with a **stall**, already
for rotation 0. Same as the gamma requests on this feature level 6 chip;
the Windows driver's request table has no 0x1f either (rotation there is
the OS's job). So no hardware rotation on the 5600; the branch stays
unmerged for a chip that has it. Compositors rotate for free anyway.

**Above 1080p (3440x1440 monitor coming).** Found while reading the size
checks: the driver allows modes only up to the largest entry of the chip's
own table (2048x1152 on the 5600), so 3440x1440 is rejected today. What
the numbers say for 3440x1440@60 (CVT-RB ~319 MHz, below the 340 MHz cap):
- Frame buffer room: three buffers sit 0xfffec0 (~16.8 MB) apart. RGB32
  (19.8 MB) doesn't fit, NV12 (7.4 MB), 4:4:4 and RGB24 (14.9 MB each) do.
  The current check assumes 4 bytes per pixel and has to become per format.
- Scanout: NV12 446 MB/s fits; 4:4:4 892 MB/s and RGB24 892 MB/s are over
  the measured limits (760 / 500), so at 60 Hz only NV12, 4:4:4 up to
  ~50 Hz. Those limits were measured at 1080p and may differ.
- USB: full NV12 frames 7.4 MB, ~45 fps for full-screen motion; partial
  updates as now.
- Unknown: whether the scanout has a line length limit (format 0 broke
  after ~2300 bytes per row; 3440 px is 3440 bytes of Y in NV12). Test from
  userspace with a computed mode before changing the driver.

**3440x1440@60 on the Gigabyte MO34WQC2 (HDMI): monitor hung.**
`tools/t6_wide_test.py` set the monitor's own timing (DTD 1: 3600 x 1481,
319.75 MHz, PLL N 31 P 975 out 3). The monitor stayed dark and its
controller froze (no OSD, its USB hub/KVM dropped off and re-enumerated
after a replug). No picture, so the formats were never seen. Likely the
T6's HDMI transmitter misbehaves at this clock/width (MCT's own table
stops at 2048x1152). Not retried. Next, only with a spare monitor:
2560x1440@60 (241.5 MHz), closer to the proven 1080p144 (333 MHz).

**2560x1440@60 on the MO34WQC2 (241.5 MHz): works.** `tools/t6_wide_test.py
2560`: NV12 and YUV 4:4:4 pictures and full-width motion fine. RGB24
(~660 MB/s scanout, over the 500 MB/s seen at 1080p) maybe blacked out for
a moment, unconfirmed; the driver doesn't use RGB24 there anyway (auto
picks 4:4:4, forced rgb24 falls back to NV12). Driver 0.18.0 now allows
modes up to 2560x1600 beyond the chip's table, and computes extra refresh
rates only for sizes up to 1080p (tried); 3440 wide stays off.
Live with the driver (mct_t6 0.18.0, niri picked 2560x1440@60 by itself):
64 s of desktop use, YUV 4:4:4 at rest, 13 switches to NV12 for motion,
convert 0.70 ms and USB 2.06 ms per update, 471 MB/s while busy.

**3440x1440: 50 Hz, then 60 Hz with reduced blanking.**
- `t6_wide_test.py 3440-50` (the monitor's DP timing at 50 Hz, 266.58
  MHz): NV12 and 4:4:4 fine. A second try of the 60 Hz EDID timing
  (319.75 MHz): black again, without a hang this time.
- `tools/t6_clock_sweep.py` (interactive, step number as squares on
  screen): 3440x1440@55 (293.2 MHz) and @57 (303.9 MHz) with the monitor's
  timing, **3440x1440@60 with reduced blanking (3520 x 1481, 312.8 MHz)**
  and 2560x1440@80 with the driver's reduced blanking (316.0 MHz): all show.
  So not the width and not a clock ceiling below 316 MHz on this monitor;
  only its own 60 Hz timing (wide blanking, 319.75 MHz) fails. Why exactly
  is open (clock just above what works, or that timing).
- Driver (0.18.0): wide modes (> 2560) up to 316 MHz; when the preferred
  mode can't be driven, the same size and rate with the driver's reduced
  blanking is added as preferred, plus the monitor's timing at 50 Hz
  (room for 4:4:4). Frame buffer room is checked per format (RGB32 doesn't
  fit at 3440x1440). Live: niri picked 3440x1440@60 by itself, NV12 (4:4:4
  needs ~890 MB/s there), convert 1.54 ms, USB 4.01 ms per update, 461 MB/s
  while busy.

**3440x1440 limits** (`tools/t6_clock_sweep.py more` / `color`):
- Refresh: 62 Hz with reduced blanking (323.7 MHz) shows; 64 Hz (334.3 MHz)
  made the MO34WQC2 drop the signal and fall back to its DP input (the LG
  takes 1080p144 at 333 MHz, so this is the monitor). The driver only adds
  the 60 Hz reduced blanking mode for it.
- YUV 4:4:4 scanout: clean at 52 and 55 Hz (~817 MB/s), lines/flicker at 57
  (~847). Together with 1080p120 clean (~746) and 1080p144 bad (~896), the
  limit is ~820 MB/s, not 760. Driver: 4:4:4 up to 820 MB/s; the extra
  mode next to the reduced blanking 60 Hz one is now the monitor's timing
  at the highest rate 4:4:4 fits (3440x1440@55, was 50).

**Other monitors, simulated** (`tools/edid_gen.py` makes EDID 1.4 blocks for
typical monitors, `tools/t6_edid_sim.sh` feeds them to the T6 connector via
DRM's `edid_override` with the output off in niri and saves what the
driver offers, `captures/t6-edid-sim/`). Driver 0.18.0 after two fixes
(fastest common reduced blanking rate below the monitor's when its own is
too fast; 2560x1440@60 preferred for 16:9 monitors too tall for the T6):

| profile | preferred | also |
|---|---|---|
| 1440p 144 / 120 Hz | 2560x1440@74, 4:4:4 (~310 MHz) | 2560x1440@60 |
| 1440p 60 Hz | 2560x1440@60 | |
| 4K 60 Hz | 2560x1440@60 (reduced blanking) | 1920x1440@60 etc. |
| 1080p 240 Hz | 1920x1080@131, 4:4:4 | 144 / 120 / 100 Hz reduced blanking (NV12 above ~130) |
| 3440x1440 100 Hz | 3440x1440@55, 4:4:4 | 3440x1440@60 reduced blanking, @50 |
| 2560x1080 75 Hz | 2560x1080@75 | |
| MO34WQC2 (real) | 3440x1440@60 reduced blanking | @55 4:4:4 |

One run showed 0 modes for the 4K profile; a rerun gave the table above
(snapshot taken while the probe was still running, no kernel messages).
4K itself (3840x2160@30, 297 MHz, NV12 12.4 MB fits a buffer) is untested:
needs height 2160 allowed and buffers sized per mode first.

**4K30**: `t6_clock_sweep.py 4k` (3840x2160@30, CTA timing, 297 MHz, NV12
12.4 MB per buffer) shows on the MO34WQC2 (scaled down by the monitor), so
the scanout takes 3840-pixel rows. Driver: sizes up to 3840x2160; 4K runs
in NV12 only (a 4:4:4 frame, 24.9 MB, doesn't fit a 16.8 MB frame buffer),
and the full colour fallback mode now also checks that a 4:4:4 frame fits.
Live through the driver (`tools/t6_modes.sh`): 3840x2160@30 and back to
3440x1440@60 fine. Simulated 4K60 monitor: 2560x1440@60 preferred,
3840x2160@30 offered.

**Error recovery (mct_t6 0.19.0, `tools/t6_cancel_test.sh`).** debugfs
`mct_t6_inject` announces a 64 KiB rectangle write and sends 256 bytes:
- With the cancel request (`40 14`): the T6 leaves the bus right away and
  re-enumerates ~3 s later; the driver probes it again, picture back.
- `40 14` alone, nothing broken: the same restart. So on the 5600 it is a
  device restart, not a soft cancel.
- Cut transfer without the cancel: the next transfers time out (-110, then
  -71), i.e. the chip is wedged waiting for the missing bytes; the second
  recovery attempt (with `40 14`) restarted it.
So `40 14` is the right last resort after a real transfer error (the
vendor drivers do the same), at the price of ~3 s without picture.

Hotplug with debounce (0.19.0): monitor power button off/on, picture back
at the same mode, 3 change events, no errors. **Frame-done events came back
after the 0x14 restarts:** 1712 of them in the stats, where every run
before had 0 with the same frame recipe. So something in the chip's state
after a restart (or after the first cancel) makes it report frame done;
to be looked into (pacing, buffer reuse).

**Windows' mode flag (byte 31 = 2) for 3440 wide** (`t6_clock_sweep.py flag`):
the MO34WQC2's own 3440x1440@60 timing (319.75 MHz) with flag 2, and with
flag 2 plus Windows' sync start + 1 / width - 1: black both times. No
change; reduced blanking stays the way to 60 Hz there.
Pacing test again after the 0x14 restarts: still 0 frame-done events from
userspace (pyusb reads with 1 ms timeouts may drop them), while the driver
counted 1712.

**Event endpoint after userspace tests.** pyusb reads with timeouts on EP
0x83 can leave the endpoint out of step: the driver then saw 11 errors and
gave up after 10 retries, still trusting the last monitor state. Now every
error resets the endpoint (clear halt, i.e. data toggle, like the vendor's
pipe reset), and if it gives up the connector falls back to polling. Test:
1 error (-71) after the pacing test, events flowing again (142 frame done).

**Frame done, measured with raw frames** (pacing test, this time the events
came): back to back 396 frames, 134 done events in 4 s; spaced frames: done
0.09 to 10.0 ms after the last write (median 1.9), phase against 60 Hz
R = 0.03. Not tied to the refresh, as the vendor capture said; whether the
chip sends them at all varies between runs. Pacing stays on the timer.

**0.20.0 (cursor, capabilities, USB 2, timings).**
- Cursor (`tools/t6_cursor_test.py`): negative positions sent as v - 1 work
  (cursor slides off the top and left edges); 96 px works with 4096-byte
  uploads; 128 px doesn't (its 65,544 bytes need upload offsets past the
  16-bit wIndex). Driver: up to 96 px, the 64 px image unless something
  shows outside it, 4096-byte chunks, native negative positions. Live:
  edges smooth, shapes right.
- Capabilities at probe: `Super Lite, feature level 6, 58 MiB, display
  flags 0x0`.
- Modes: generated ones use CVT-RB (160 px blanking) where it fits; on the
  simulated monitors only 1080p at 100/120 Hz changed (99.92 / 119.91 Hz),
  1440p, ultrawide and 4K stay on the tighter timing (clock limits).
- The MO34WQC2 hung (OSD dead) after the EDID simulation toggled the T6
  output and EDID; stopping the T6 signal brought it back, then 2560x1440
  and 3440x1440@60 were fine again. Run simulations without that monitor.

**NV12 sharpness, offline** (`tools/nv12_eval.py`: coloured text on light
and dark backgrounds, Lab dE against the original):

| method | mean dE | px dE > 5 |
|---|---|---|
| 2x2 average (driver) | 1.33 | 5.8% |
| linear-light average | 1.36 | 5.4% |
| average + luma correction | 1.36 | 5.8% |
| linear + luma correction | 1.39 | 5.5% |

(nearest chroma upsampling; bilinear ranks the same.) With one chroma per
2x2 block and free luma per pixel, the plain average already minimises the
RGB error; the other methods move the error around. The fringes at coloured
text are NV12 itself. No driver change; full colour at 3440x1440 means 55 Hz.

**128 px cursor (0.21.0):** `t6_cursor_test.py 128` with the upload stopped at
0x10000 bytes (16-bit offset; the last two pixels never sent) shows a clean
128 px cursor. Driver: the smallest of 64 / 96 / 128 that holds everything
visible, 128 cut at 0x10000. Live: pointer, shapes, edges and the tablet fine.

**4:4:4 limit back to 760 MB/s (0.21.1).** With 820 MB/s the driver offered
1080p131 in 4:4:4 (~815 MB/s); in daily use it showed thin white lines (1 px
wide, up to ~100 px tall) whenever a lot changed on screen. The short sweep
step at 3440x1440@55 (~817) had looked clean with a moving block, but
heavier USB traffic during real use leaves less scanout bandwidth. 1080p120
(~746) has been clean for days. Full colour now up to 122 Hz at 1080p,
68 Hz at 2560x1440, 51 Hz at 3440x1440; a longer test under load could
raise it again.

**USB 2 link (mct_t6 0.24.0).** The T6 in a USB 2 port (5-1, high speed):
the driver logs `USB 2 link: modes up to 2048 wide`, sysfs `usb_speed`
says `usb2`, the LG ran at 1920x1080@122, picture correct, moving windows
works (slower, as expected at ~35 MB/s).

**Text console on the T6.** `con2fbmap 3 2` (fb2 = mct_t6drmfb, from the
fbset package) and Ctrl+Alt+F3: tty3 shows on the T6's monitor, login
prompt fine; back to niri with its VT.

**Edge-case test (mct_t6 0.25.0, mct_t2 0.3.21, `tools/mct_edge_test.sh`):**
all 13 steps ok, no BUG/Oops/WARNING/hangs: sysfs vs. niri modes, cursor
at every edge and after leaving/returning, format and lossy switching
under motion, all_modes toggle, text consoles on the tablet and the T6
with VT switches, USB unplug/replug of the tablet (back after 8 s) and
the T6 (3 s) during motion with sysfs readers running (reads failed
cleanly, none slow), niri DPMS off/on, the monitor's power button,
suspend/resume. Log kept in the lab directory (captures/edge-tests/).

**Device information (mct_t6 0.26.0 / 0.27.0, 2026-10-11).** Read at probe
and shown in sysfs: platform 1 ("Super Lite"), boot code `16062899`, image
code `17091401` (both look like BCD dates: 2016-06-28 and 2017-09-14),
board "JUA254H" (j5create JUA254), no serial (8 zero bytes). Function
descriptors (`b1`): index 0 is function 0x5620 "Trigger 6 External
Graphics", index 3 is 0x5640 "USB Audio Device". ~~The 5600 enumerates with
no audio interface, so the chip has the audio function but this board
doesn't expose it on USB~~ (the 5601 in the capture reported 0x5621 for its
display function). Reading `b1` at probe caused no problems.
EDIT (2026-10-12): wrong conclusion. T6 audio goes over the vendor bulk
endpoint (session signature 3, after requests `a0`-`a5`, `23`, `24` in MCT's
daemon), not a USB audio interface, and j5create's spec sheet lists HDMI
audio for the JUA254. So the 5600 most likely has HDMI audio; untested.

**Older kernels (2026-10-11).** Built against 6.18.53 and 6.12.111: 6.18
lacks only the 7.2 rename of `struct drm_atomic_state` to
`drm_atomic_commit`; 6.12 also lacks the vblank timer helpers, the
`drm_client_setup` fbdev path for shmem, `drm_dev_set_dma_dev`,
`hrtimer_setup` and the const `mode_valid` argument. `kernel/mct_compat.h`
covers them, selected by header checks in `Kbuild` (the vblank helper turned
up in 6.18.53 although it is not in v6.18, so versions would have been
wrong). Tested in a NixOS VM (QEMU, the JUA254 passed through with
`guest-reset=off`, the LG 27GN750 on HDMI), 6.12.111 against 7.2.7 as the
reference, same script: probe and sysfs identical; EDID modes (36, up to
1080p144); fbdev `fb1`; `vbltest` 119.99-120.02 Hz on 6.12 (the compat
hrtimer) vs 120.00 on 7.2; `modetest -v` page flips 119.5-120.4 Hz vs
119.98-120.02, ~1600 updates in 15 s on both; clean `rmmod`, no warnings.
`vbltest` and `modetest` exit at once when stdin is /dev/null (a systemd
service): feed them `sleep 30 |`. QEMU resets the device when it lets go
of it (each VM exit): the T6 came back on the host every time.

## 2026-10-12

**720p240 (2026-10-12).** 1280x720 at 240 Hz (279.94 MHz, reduced
blanking) tested by the user on the LG 27GN750: works well.

**Scout of future work (2026-10-12).** A read-only research pass over the
docs, the vendor drivers, the other projects and the web (lab
`captures/scout/todo-candidates.md`). For the T6:

- HDMI audio is likely available on the JUA254 (see the EDIT above); MCT's
  daemon (`t6.h`, `t6auddef.h`, `t6usbdongle.c`) shows the requests and the
  audio session. A read-only probe first.
- The firmware has I2C requests (`0x81` read, `0x02` write in MCT's
  `t6.h`), so DDC/CI to the monitor (brightness, input) looks possible;
  neither vendor driver uses them. Reads first, on the LG only (the
  MO34WQC2's controller hangs easily). Never send `0x01`/`0x13` (ROM writes)
  or `0x30` (hardware reset).
- cyrozap's captures include a JUA365 with both HDMI ports active and JCD543
  sessions: the second-output protocol can be worked out offline. Two other
  projects saw a 5601 drop off the bus when idle and added a keepalive.
- The Wacom DTU-1031AX is a T6 device (its board name is in MCT's firmware
  list); its USB ID is unknown.
- A Trigger 5 driver ("drm/trigger5") is under review on dri-devel (v2,
  2026-09); its review comments match mct's open items.

**HDMI audio works (2026-10-12, `tools/t6_probe.py`, lab
`captures/t6_probe-20261012-*.log`).** Descriptors (step A): one playback
engine, one format, 48 kHz 16-bit stereo (fs 128, BCLK 1.536 MHz), one
volume node (0-31, at 15 on all channels), cyclic buffer 128 KiB, jack
state `0x05` (speaker 1, DVO HDMI 0, LVDS HDMI 1). Playback (step D):
`40 24` with Activity 1, format 0, ReturnSize 9600, cyclic buffer from `a4`,
then signature-3 bulk sessions (a 9600 B prefill, then 1920 B = 10 ms
packets): a 5 s 440 Hz tone at about -30 dBFS was heard on headphones in
the LG 27GN750's jack, while the picture kept running. The first run (no
restart flag, 1.5 s) was not heard, but the user had no headphones on then;
the second run set the "restart engine" flag (byte 24 of the first audio
packet). ~~Whether the flag is needed is open~~ EDIT (2026-10-12, third
run, `--no-restart-flag`): heard as well, so the flag is not needed; the
first run was silent only because no headphones were on. The engine state read (`a4`) said
"stopped, position 0" before, during and after playback in both runs, so
it doesn't reflect playback on this firmware. I2C (step B): no variant of
`0x81` returned the monitor's EDID, so DDC/CI (step C) was not tried; the
I2C request probably reaches the board's own bus to the HDMI transmitter.

**ALSA support: plan and the audio clock (2026-10-12).** The user wants real
ALSA playback in the module. MCT's daemon (triggerdm `main2.c`
`audio_capture_process`) reads 10 ms blocks from an ALSA loopback and sends
each at once (`t6_libusb_SendAudio`): it is paced by the host clock only and
never looks at the device, so drift between the host and the adapter's
48 kHz crystal is ignored (the 128 KiB cyclic buffer, ~680 ms, hides it for
a while). MCT's `T6INTERRUPTDATA` (`t6bulkdef.h`) defines the 64-byte
EP 0x83 event: byte 0 function mask (0x04 video, 0x20 audio), 0x13 display
event, and at 0x20 a u32 with the **audio render DMA pointer** (bits 0-23)
and jack bits (24 jack 0, 25 DVO HDMI, 26 LVDS HDMI); `ReturnSize` (9600)
is "the interrupt interval in bytes" for playback. If the chip sends these
events, the driver can follow the adapter's real clock. `tools/t6_probe.py`
step E plays 20 s while logging every event, their cadence and the
pointer's rate against the host clock (ppm). Design notes for the driver:
a sound card per T6 device (48 kHz, S16_LE stereo playback, a volume
control on node 0 via `40 23`), audio sessions queued on the same bulk
endpoint as the frames (a large frame can delay a packet; the device
buffer gives room), paced by the device's events if they exist, else by a
host timer with a fill target and drift correction.

**Step E results (2026-10-12, lab `captures/t6_probe-20261012-17*.log`).**
Two runs were silent because the headphone plug wasn't fully in. With it
in: audio plays only when the probe sets a mode and the output is on (with
the output off after the kernel driver was unbound, nothing): HDMI audio
rides on the video signal, so the driver can play only while the CRTC is
on. The event reader (pyusb, a thread reading EP 0x83 with 100 ms
timeouts) got one frame-done event (`04 ... 04`) and then only I/O errors
(EIO) for the rest of each run, so no conclusion about audio events; the
kernel driver, which already reads EP 0x83 reliably, will count events
with the audio function bit (0x20) and their DMA pointer instead.


**ALSA playback in the driver (2026-10-12, mct_t6 0.29.0, not yet tested).**
Written as planned, with the host clock pacing (see
[t6-driver.md](../driver/t6-driver.md#audio)). Choices:
- The work runs under `io_lock`, so audio waits at most one frame. The
  120 ms lead covers that, and the device's 128 KiB buffer has room for
  much more.
- The lead is sent only from data the application has written. A start
  with a small buffer (2 x 10 ms) doesn't send stale buffer contents or
  make ALSA report an underrun at once.
- A stop followed by a start before the work runs (an xrun restart) resets
  the timing (`restart` flag), so there is no catch-up burst.
- `t6_ctrl_out` returns the byte count; the first version took that as an
  error and would never have marked the engine on. Found in review before
  testing.
- Not done yet: the volume node (`40 23`), drift correction, and a stream
  that starts while the display is off (it plays into nothing).

To test: `aplay -l` should list the board's name ("JUA254H"). Then
`speaker-test -D hw:<card>,0 -c 2 -r 48000 -F S16_LE -t sine -l 1` with the
display on, then a PipeWire sink for a few minutes (drift shows as growing
delay or glitches). Afterwards check the debugfs `audio` line: if the
event count grows during playback, the DMA pointer can drive the clock.

**First ALSA test (2026-10-12, mct_t6 0.29.0).** The card registered as
"JUA254H" (`aplay -l`, card 5). `speaker-test -D hw:JUA254H,0` (1 s
buffer, 4 periods) ran with exact period timing. Spotify through PipeWire
sounded "like buzzing falling rain instead of music".

~~Likely cause~~ EDIT (2026-10-12): not the cause of the buzz, which was the 1080p144 mode (below), but real problems all the same, so the changes stay. Suspected cause: 0.29.0 reported the ALSA position as what was sent. It
jumped in 10 ms steps and ran 120 ms ahead of what the device played.
PipeWire, which keeps little queued and paces by that position, let the
buffer run dry. The driver then sent the "due now" part anyway, from
buffer contents not yet rewritten (stale audio). The device plays its
128 KiB ring regardless, so when it runs dry it replays old contents too.
0.29.1 changes:
- The position moves smoothly with the host clock plus the lead.
- Only written data is sent, with silence when the device has less than
  30 ms left (counted as "silence fills" in debugfs).
- The PCM is marked BATCH.
- Drain pads the last packet.

**Second ALSA test (2026-10-12, 0.29.1): still buzzing.** debugfs: 2
starts, 2448 packets, 2 silence fills, 1 audio event (pointer word
0x45000000: jack 0 and LVDS-HDMI bits, DMA pointer 0). Pacing therefore
isn't starving. The user clarified that the probe's tones (step D/E)
were clean but every ALSA playback, `speaker-test` included, buzzed. The
header and format match MCT's daemon exactly (`t6_libusb_SendAudio`:
`{3, len, 0, len, 0...}` then the PCM, 1920 bytes, S16_LE stereo).
Remaining differences from the probe:
- The lead: driver 120 ms against the probe's ~40-90 ms and Windows'
  ~50 ms. A buzz is a discontinuity every 10 ms packet, which fits a
  device FIFO smaller than the 128 KiB it reports overflowing.
- Video sessions interleaved with the audio.
- Bursts on the 5 ms tick rather than one packet per 10 ms.
~~0.29.2~~ EDIT (2026-10-12): 0.29.1 (one commit with the pacing changes) makes the lead a runtime module parameter (`audio_lead_ms`,
default 40, read at stream start) to test the first.

**Found: the mode, not the driver (2026-10-12).** The LG was at 1080p144
through the T6; the probe's clean tones had been at 60 Hz (the user had
noted that audio worked "on 60hz mode"). Same driver and stream, by mode:

| Mode | Pixel clock | Audio |
|---|---|---|
| 1080p60 | 148.5 MHz | clean (`speaker-test` and Spotify through PipeWire) |
| 1080p75 | 174.6 MHz | clean |
| 1080p120 | 297.0 MHz | clean |
| 1080p144 | 333.2 MHz | buzzing |

~~297 MHz is the top HDMI 1.4 clock (4K30). The firmware sets up the HDMI
audio clock (N/CTS); it apparently doesn't handle clocks above that,
although video runs to 340 MHz.~~ EDIT (2026-10-12): wrong, the 144 Hz mode had only 80 px of horizontal blanking; see "Not the clock but the horizontal blanking" below. Not between 297 and 333 MHz yet (e.g.
1080p121.99 at 293.7 doesn't narrow it). The lead isn't the cause (20-90 ms
all buzzed at 144 Hz); the default goes to 90 ms, the value of the clean
Spotify run. The driver logs a warning once per mode when a stream starts
above 297 MHz or with the display off. Both 0.29.1 and the lead
parameter are in one commit (0.29.1).

**Not the clock but the horizontal blanking (2026-10-12).**
`tools/t6_audio_clock.sh` (niri modelines, a tone, y/n per step):
- `clock`: 333.216 MHz with the CTA 1080p timing (2200 x 1125, 280 px
  horizontal blanking, 134.6 Hz) played clean, twice. The clock isn't the
  limit.
- The driver's 1080p144 was its tight fallback: 80 px horizontal blanking
  (2000 x 1157), because CVT-RB (160 px) at 144 Hz needs more than the
  T6's 340 MHz.
- `hblank` at 1080p 120 Hz, vtotal 1125, sync 8 px after the active area
  and 32 px wide: 80 px buzzing, 176 clean, 128 clean, 104 buzzing, 112
  clean.

HDMI sends audio in data islands inside the blanking. Below ~112 px the
adapter evidently can't fit them and audio breaks up.

The fix (mct_t6 0.29.2): the tight timing now tries 152 down to 80 px and
keeps the widest that fits. 1080p144 becomes 2040 x 1157 = 339.88 MHz,
120 px, with the same line rate and vertical timing as before. The audio
warning checks the blanking (< 112 px) instead of the clock.

~~"Distorted above 297 MHz"~~ EDIT (2026-10-12): the 297 MHz claim
earlier today was wrong. The real cause is the blanking, as above.

**Vendor audio analysis (2026-10-12, lab
`captures/t6-research/audio-analysis.md`, static analysis by a research
agent).**
- Windows' audio is in `t6sta.sys`; the Mac has a HAL plugin
  (`MCTT6Audio.driver`) and the dext.
- Volume `40 23`: 40 bytes `{mask, 7 x 0, 8 x s32}`, 0-31, linear
  (Windows' table is exactly 256·20·log10(i/31)).
- No mute node. Both vendors set 31 at start.
- No vendor uses the DMA pointer or drift correction. The 1.8 s Windows
  capture has no audio events either, so the plan to pace by events is
  dropped.
- The firmware does the HDMI audio setup itself (InfoFrame, N/CTS) from
  the EDID. The vendors' table modes all have wide blanking, which is why
  they never hit the 112 px limit.
- Unknown: whether `40 31` wIndex 3 ("software ready (audio)", sent by the
  Mac at start) matters on the 5600.

mct_t6 0.29.3 adds "PCM Playback Volume" and "PCM Playback Switch". It
reads the current value at probe and does not force 31. Not yet sent to
the 5600: test with `amixer -c JUA254H`, comparing 31, 15 and 0 against
a tone.

## 2026-10-13

**Research round 2 (2026-10-13), Wacom and T2 devices** (lab
`captures/t2-research/wacom-and-t2-devices.md`).
- The DTU-1031AX is a T6 device, `0711:5609`: a T6-688SL with a Realtek
  RTD2281L in the FCC photos, firmware image `DTU-1031AX-IC21062801.t6img`
  with that device descriptor and a 1024x600 Wacom EDID.
- The DTU-1031X has 16 MiB of video memory (EtronTech EM6A9320BIB-5H).
- A probable backlight GPIO, `feb1` bit 6, is untested.
- libwacom: the DTU-1031X size should be 223 x 125 mm (it says 229 x 127),
  and there's no DTU-1031AX entry.
- usb.ids lacks `0711:5209/520d/5600/5601/5609`.

**Research round 2 (2026-10-13), T6 feature gaps** (lab
`captures/t6-research/feature-gaps.md`, decompiled firmware image
JUA254H-IC24030502 with its request dispatcher).
- Little device-side functionality is left that mct_t6 lacks.
- No hardware rotation in this board's firmware, and no I2C, so DDC/CI is
  closed.
- Three more requests are never to be sent (`0x15` test mode rewrites
  flash, `0x33` serial write, `0x40` internal message); they are added to
  `tools/t6_probe.py`'s forbidden list.
- Windows auto-updates the firmware on start.
- Untried read-only info requests: `b2`, `8a`, `83`, `41`.
- Windowed flips (bytes 0x24-0x2b) could letterbox without a mode set.

**Research round 2 (2026-10-13), the web** (lab
`captures/scout/web-research-2026-10-13.md`; downloads in
`third_party/t6-sources/web-2026-10-13/`). Checked against the sources:
- **USB IDs:** more T2 and T6 USB IDs (see roadmap item 8). The T6 PID
  comes from the firmware image (a table of board name and IDs for 39
  images, `t6img_ids.txt`).
- **JPEG:** a public-domain userspace JPEG driver for the same JUA254H
  board exists ([lvalen91/JUA254H---Fedora](https://github.com/lvalen91/JUA254H---Fedora)).
  Its findings:
  - full JFIF needed;
  - 4:2:0 works;
  - two bulk writes plus at least 1 KiB of zero padding;
  - flag 0x80 on early frames and on ring wrap;
  - ~30 fps at 1080p.
- **Tetra driver** ([Gadgetoid](https://github.com/Gadgetoid), MIT):
  - partial updates through FB_DIRECT;
  - CLIP_PRIMARY ignores its x/y;
  - a "custom timing" flag (bit 0) in the timing entry.

**Tests of mct_t6 0.29.3 (2026-10-13).**
- 1080p144 with the new 120 px blanking (339.88 MHz): stable picture,
  clean tone, no blanking warning in the log.
- Volume: 31, 15 (quieter) and 0 (silent), mute and unmute all behave as
  expected; the first `40 23` writes to the 5600 work. The adapter was at
  31/31 at load (`a3`).
- One run "at 31" was silent: setting 31 when it is already 31 sends
  nothing, and the repeat played. Most likely that `speaker-test` didn't
  get the device (PipeWire holds it while its sink is active; use
  `pactl suspend-sink … 1` for `hw:` tests).

**Long playback (2026-10-13, mct_t6 0.29.3).** Music through PipeWire on
the JUA254H for a long session: no drift, delay or crackle, so host-clock
pacing is enough (as the vendor analysis suggested). Bit-perfect: the
path carries 48 kHz S16 unchanged at volume 31 (0 dB), so PipeWire needs
48 kHz/S16 content and no resampling for that; 44.1 kHz content is always
resampled, since the 5600 offers only 48 kHz.

**ELD and jack controls (2026-10-13, mct_t6 0.29.5).** On the LG:
`HDMI/DP,pcm=0 Jack` = on, and `ELD` = 28 bytes:
- version 2;
- manufacturer `1e 6d` (GSM, LG);
- monitor name "27GN7";
- one short audio descriptor `09 07 07`: LPCM, 2 channels, 32/44.1/48 kHz,
  16/20/24-bit.

**JPEG path, phase 0 (2026-10-13, `tools/t6_jpeg_test.py`, LG at 1080p60,
logs ~~`captures/t6_jpeg-20261013-*.log`~~ EDIT (2026-10-13): moved to
`captures/t6-jpeg/`, with an index of the runs).**

Full-frame JPEGs work on the 5600 (firmware 17091401):
- Command 3, 4:2:0, Pillow's standard tables, no restart markers.
- Planes at fb + 0x30, UV at pitch x align16(h), pitch align16(w).
- Payload is the 48-byte header, the JPEG and 1024 zero bytes.
- Test cards, flat grey, a green card and noise all showed correctly. Q75
  looks blocky, Q95 clean.

Frame-done events (`0x13` bit 2, fence at `0x0c`) come for every frame.
They seem to repeat the last finished fence, so a time is "decode plus
the next report", 10-12 ms at least.

Transport rules found the hard way:
- **No "more" flag on JPEG fragments.** With it, nothing displayed. Raw
  frames are the other way round.
- **No ring that wraps back with `FLAG_RESET_JPEG`** (the Ubuntu daemon's
  way). At the first wrap the event endpoint went into I/O errors. In one
  run the JPEG path was dead until a power cycle; in another, only the
  events recovered later. Three fixed 3 MiB slots used in turn (at most 2
  frames in flight, the flag only on the first frame) had no errors at
  all until the noise frames.

Decode time at 1080p (frames one at a time, and back to back with at most
2 in flight):

| Content | JPEG | Per frame | Back to back |
|---|---|---|---|
| flat grey, Q75/Q95 | 32 KiB | 11.9 ms | ~100 fps |
| green test card, Q75 | 94 KiB | 11.7 ms | 99 fps |
| green test card, Q95 | 153 KiB | 19.5 ms | 50 fps |
| noise, Q75 | 1.2 MiB | **326 ms** | ~1 fps |
| noise, Q95 | 2.4 MiB | no event within 1 s | - |

So the decoder is bound by the compressed data, not by pixels:
- From ~100 fps for simple pictures it drops to ~3 fps at 1.2 MB of noise
  (~3.7 MB/s of bitstream).
- During the slow noise frames the pyusb event reads failed again (695
  errors). The kernel reads events fine, so this is the userspace reader.
- J3 (command 7 into a hidden buffer at up to 3840x2160) left the picture
  alone, but its timings were lost to the broken event reads.

Verdict, against the research estimate (100-135 Mpixel/s):
- Ordinary pictures reach ~100-200 Mpixel/s.
- Detailed or noisy ones fall far below.
- On USB 3, raw NV12 already gives ~95-100 fps at 1080p whatever the
  content, so JPEG would be slower and content-dependent.
- On USB 2 (raw ~11 fps at 1080p), JPEG at a moderate quality would be
  ~50-100 fps for desktop content but collapse on photos and grain unless
  the quality drops as the frame size grows.

So JPEG is worth building only for USB 2 links, with a size budget per
frame and raw as the fallback.

**After the JPEG test (2026-10-13).** With the driver bound again, the LG
showed a garbage band ~50 px high at the bottom. After a mode change to
120 Hz there was a green tint in the middle; back at 144 Hz the band was
purple/green and the whole picture looked blocky and rainbow-coloured.
The adapter's state from the JPEG frames (decoder or colour conversion)
outlives the driver rebind, so the script now ends by telling the user to
replug the T6 (confirmed: only the replug brought the clean picture back). If the driver ever uses JPEG, it has to know how to bring
the adapter back to its raw state (untested: `0x86` "reset JPEG engine").

**JPEG, second round (2026-10-13, `captures/t6-jpeg/t6_jpeg-20261013-190320.log`,
after a replug).** Event reads in a background thread with 2 s timeouts:
21 read errors in the whole run instead of hundreds.

- **Partial JPEG updates work.** Command 7 with a 256x128 JPEG at (832,
  448) decoded into the hidden buffer:
  - plane 0 = base + y·1920 + x;
  - plane 1 = base + 1920·1088 + (y/2)·1920 + x.

  The empty flip that mct_t6 uses (NV12, UV at 1920·1088) then showed the
  box in place and the rest unchanged.
- **Decode time grows faster than the size** (one test card per size,
  Q95, command 7):

  | Size | JPEG | Decode | Throughput |
  |---|---|---|---|
  | 1920x1080 | 203 KiB | 29 ms | 71 Mpixel/s |
  | 2560x1440 | 343 KiB | 71 ms | 52 Mpixel/s |
  | 3440x1440 | 459 KiB | 120 ms | 41 Mpixel/s |
  | 3840x2160 | 752 KiB | 329 ms | 25 Mpixel/s |

  At the MO34WQC2's 3440x1440 that's ~8 fps, far below raw NV12 there
  (~45 fps). The large JPEGs showed up garbled because the script decoded
  them into the buffer P had just put on screen (fixed).
- **The clean check after JPEG was clean.** RGB24 and NV12 pictures drawn
  exactly like mct_t6 (session 7 rectangle writes, empty flip) looked
  right, so this run left no bad state. The tint after the first run
  probably came from the wrapping ring, not from JPEG itself.

**JPEG, third round (2026-10-13, `t6_jpeg-20261013-190616.log`).**
- J3 into the buffer that is really hidden left the screen unchanged, and
  the timings repeated exactly (29 / 71 / 120 / 329 ms).
- Request `40 86` ("reset JPEG engine" in MCT's t6.h) **stalls on firmware
  17091401**: it doesn't exist here, as in the newer image's dispatcher.
  The decoder worked normally right afterwards, and the driver-style clean
  check was clean.
- With fixed bitstream slots and event reads in a thread (5 read errors
  in the whole run), the JPEG path behaves.

What's known for a driver now:
- full frames (command 3) and partial updates (command 7 + empty flip),
  3 fixed slots, no "more" flag;
- the decode costs: ~12 ms (simple) to ~30 ms (detailed) at 1080p, and
  much more at large sizes or for very detailed content;
- no reset request: recovery has to avoid wedging (no ring wrap with the
  reset flag), or the device has to be power-cycled.

## 2026-10-14

**Cursor latency (mct_t6 0.29.9, `tools/cursor_latency.sh`, LG at 1080p144).**
With the cursor's requests off `io_lock` (0.29.8), time from the cursor
commit to the end of its USB transfer, 10 s per phase:

| Phase | Commits | Replaced | Moves | Move average | New images | Image average |
|---|---|---|---|---|---|---|
| still desktop | 1421 | 0 | 1419 | 0.12 ms | 2 | 12.9 ms |
| fullscreen video | 1380 | 0 | 1370 | 0.24 ms | 10 | 3.0 ms |
| dragging a window | 1368 | 0 | 1360 | 0.38 ms | 2 | 10.8 ms |
| changing shapes | 1407 | 16 | 1264 | 0.56 ms | 127 | 11.3 ms |

Maximum since load: 10.3 ms for a move, 20.6 ms for an image. Moves come
at the refresh rate (~140/s) and take well under a millisecond even with
frames going out. New images are the slow part: a full upload of up to
64 KiB in 4 KiB control transfers, and moves queued behind it are what
got replaced. Next: keep recent shapes in the adapter's spare cursor slots
(5 per output per the firmware), so a known shape costs one request.

**Cursor slot cache (mct_t6 0.29.10, ~~to test~~ EDIT (2026-10-14): tested, below).** The driver keeps a copy
of what each of its 5 cursor slots holds. A shape already in a slot is
shown with one select request; a new one is uploaded into the least
recently used slot that isn't on screen. Slots 2-4 per output weren't used
before: the firmware's dispatcher says 10 slots shared by both outputs
(lab feature-gaps analysis).

Test of 0.29.10 (`tools/cursor_latency.sh`, LG at 1080p144; the video
phase was skipped):

| Phase | Commits | Replaced | Move average | Shape changes | Shape average |
|---|---|---|---|---|---|
| still desktop | 1428 | 0 | 0.12 ms | 1 | 0.19 ms |
| dragging a window | 1393 | 0 | 0.36 ms | 2 | 5.89 ms |
| changing shapes | 1381 | **0** | 0.12 ms | 150 | **0.47 ms** (was 11.3) |

157 shapes came from the slot cache and 4 were uploaded (each shape's
first appearance, the 20 ms maximum). Slots 2-4 work.

**Frame latency (mct_t6 0.29.11, `tools/frame_latency.sh`, LG at 1080p120,
fullscreen video at 50 fps, 20 s).** 1001 updates, none replaced:

| From the frame's first commit | Time |
|---|---|
| until the worker starts | 0.45 ms |
| conversion (average since load) | 0.95 ms |
| until the last transfer is queued (USB queueing, average since load) | 2.77 ms |
| until the flip's transfer has completed (frame on the adapter) | **9.42 ms** |

Since load: 12.5 GB in 11 560 updates (~1.1 MB per update, 460 of 1080
rows on average). During the video the auto format sends NV12, ~3.1 MB for
a near-full 1080p frame, ~8.9 ms at the adapter's ~350 MB/s: the frame
latency is almost all USB transfer. What's left to gain without loss is
the ~1 ms of conversion, by converting band by band and sending each band
at once (overlap); less data would need JPEG (lossy, and slower to decode).

## 2026-10-15

**Frame latency baseline before tuning (mct_t6 0.29.11, same fullscreen
video, 20 s):** 1000 updates at 50 per second, none replaced; commit ->
worker 0.32 ms, commit -> on device 9.14 ms (9.42 ms on 2026-10-14);
conversion 0.31 ms and USB queueing 1.81 ms per update since load. What's
left that the driver can shorten without loss: the worker start (normal-
priority workqueue) and the ~1 ms conversion of a full frame; the rest is
the USB transfer of the frame.

**High-priority update worker: no gain, taken out again.** mct_t6 0.29.12
ran the update worker on a per-device WQ_HIGHPRI | WQ_UNBOUND queue. Same
video, 20 s: 960 updates at 48 per second, commit -> worker 0.43 ms,
commit -> on device 9.69 ms (baseline 0.32 / 9.14). Not comparable
one-to-one: the T6 had moved from USB port 2-2 to 7-3 in the replug, and the
system load (~4) and CPU clocks (powersave governor, balanced profile) vary.
But "commit -> worker" starts when the commit begins copying the
framebuffer into the shadow (`dirty_since` is taken before the row compare
and copy, ~8 MB for a full 1080p frame), so it is mostly that copy, which no
queue priority shortens. Removed in mct_t6 0.29.13 / mct_t2 0.3.40.
