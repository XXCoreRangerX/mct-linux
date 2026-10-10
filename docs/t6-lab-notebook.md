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
