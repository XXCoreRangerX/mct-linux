# mct_t6 compared

How the Trigger 6 driver relates to the T2 driver, to MCT's own drivers and
to other USB display stacks (state of 2026-10-11, mct_t6 0.17.1). Details
and measurements are in [t6-lab-notebook.md](t6-lab-notebook.md) and
[protocol-t6.md](protocol-t6.md).

## mct_t2 and mct_t6

Same skeleton: a DRM/KMS driver with a primary and a cursor plane, a CRTC
paced by the vblank timer helpers, shmem GEM objects with a shadow copy, a
worker that sends updates (the newest image wins), partial updates, a
hardware cursor, debugfs statistics, suspend/resume. mct_t6 was written in
mct_t2's mould; the code is separate (mct_t2.c ~1360 lines, mct_t6.c ~2180).

| | mct_t2 (Wacom DTU-1031X) | mct_t6 (T6 USB Station) |
|---|---|---|
| Link | USB 2, ~38 MB/s | USB 3, ~330-400 MB/s |
| Picture data | own lossless planar RLE; required at that bandwidth | raw pixels (RGB, YUV 4:4:4, NV12); no compression needed |
| Display | fixed 1024x600 panel, scanned rotated by 180 degrees | any HDMI monitor: EDID, mode table, computed modes up to 1080p144 / 720p240 |
| Bring-up | 78-step register init, reverse engineered from captures | none; the firmware brings the chip up, a few requests set the mode |
| Buffers | two, explicit flip command | three, flip implied by each frame header |
| Partial updates | rectangles in 1024-pixel units, compressed | row bands limited to the changed columns, raw; two writes per band for 4:4:4 |
| Cursor | register lists on a second endpoint | control requests, 5 slots per output, alternating slots |
| Extras | backlight through the pen's HID function, recovery from a parser stuck mid-frame, optional lossy mode | format choice incl. adaptive mode, hotplug events, software gamma, queued USB transfers, exact row diffing |
| Hard parts | protocol and init reverse engineering, desync recovery | buffer addressing, the chip's scanout limits, CPU reads of GPU memory |

## mct_t6 and MCT's Windows / macOS drivers

### Ported (protocol facts from the vendor drivers)

- Frame framing and the three-buffer layout, with the exact header recipe
  (data length, plane addresses), from decompiling T6InDisp.dll.
- The PLL formula for the pixel clock (MCT's Ubuntu source).
- Set mode, "software ready" (0x31), output on/off.
- The rectangle write (session type 7) used for partial updates.
- Cursor image upload, slots and position.
- Interrupt events for monitor changes and frame done.

### Our own

- **Raw formats instead of JPEG.** The vendor drivers send JPEG at quality
  95 for 1080p; mct_t6 sends exact RGB or full-resolution YUV at rest.
- **Refresh rates above the vendor's 60 Hz.** Timings and PLL settings are
  computed per mode; the chip was found to drive up to ~350 MHz pixel
  clock (1080p144, 720p240), not just its 197 MHz table.
- **Format choice by measured scanout bandwidth** (RGB ~500 MB/s, YUV 4:4:4
  ~760 MB/s on the 5600).
- **Partial updates for YUV 4:4:4** with two NV12-style rectangle writes per
  band (the second plane part covers half of the U plane each time).
- **Exact change detection:** damaged rows are compared with the shadow
  copy instead of trusting compositor damage.
- **Software gamma**, since this chip (feature level 6) has no gamma tables;
  the vendor just doesn't offer gamma there.
- **niri patch:** frames for display-only devices are rendered into buffers
  allocated on the device (system memory) instead of the GPU's VRAM; the
  CPU read per 1080p frame went from 7 ms to 0.3 ms.
- **Rotation-safe damage handling** for smithay 0.7's untransformed damage
  clips on rotated outputs.
- **Queued USB transfers** (the vendor sends one at a time).

### Inspired by others

- **Adaptive colour** (exact/full colour at rest, NV12 during large
  changes): Microsoft RDP's AVC444 mode (4:2:0 for motion, colour detail
  added for still content) and the vendor's own idle refresh (JPEG quality
  100, or lossless where supported, ~50 ms after the last update).
- **Driver structure:** the in-kernel USB display drivers udl (older
  DisplayLink chips) and gud. Newer DisplayLink adapters use a closed
  userspace driver with bandwidth-adaptive compression, the same "quality
  follows bandwidth" idea.
- **Fast during motion, exact at rest:** mct_t2's lossy mode with lossless
  refinement.
- **The niri patch:** mutter solves the same VRAM read-back problem with a
  GPU copy into system memory ("primary GPU copy"); here the GPU renders
  into the device's buffers directly.

### Where the vendor drivers are still ahead

- **JPEG:** far less data for full-screen video. Out of reach for a kernel
  driver (below).
- **Dual-output models and HDMI audio** (not present on the 5600; a general
  MCT driver would need them).
- Hardware rotation (request 0x1f) only exists in MCT's Ubuntu driver; the
  5600 stalls on it and the Windows driver leaves rotation to the OS, as
  Linux compositors do.

## JPEG and Linux

- The kernel has no JPEG encoder. It has JPEG helpers (header parsing for
  camera and video drivers) and V4L2 interfaces for hardware codecs, nothing
  a display driver could use to compress frames. DRM drivers are expected to
  stay simple and lossless; a codec in one would not be accepted upstream.
- USB displays that compress on Linux do it in userspace: DisplayLink's
  official driver and MCT's Ubuntu driver both pair the EVDI kernel module
  (a virtual DRM output) with a userspace program that encodes frames
  (MCT's uses libjpeg-turbo) and talks USB. Our older t2d daemon for the T2
  works the same way.
- The RX 9070's video engine decodes JPEG but doesn't encode it; some Intel
  GPUs encode JPEG through VA-API, again only from userspace.
- On the T6 over USB 3 it would barely help: full NV12 1080p frames already
  go out at ~100-120 fps and desktop use is far below the link's capacity,
  while JPEG would add encode time per frame, latency and compression
  artifacts. It matters for the vendor because they also serve USB 2 hosts
  and multi-monitor docks. A JPEG mode would be a separate userspace
  project (an EVDI daemon like t2d), not part of mct_t6.

## Feature table (2026-10-11, mct_t6 0.20.0)

| | mct (Linux) | MCT Windows | MCT macOS | MCT Ubuntu (EVDI) |
|---|---|---|---|---|
| Frames | raw: exact RGB / YUV 4:4:4 at rest, NV12 in motion | JPEG (q95), lossless idle refresh | JPEG | JPEG, raw NV12/I420 |
| Refresh | up to 144 Hz (1080p), 60 Hz at 3440x1440 | 60 Hz | 60 Hz | 60 Hz |
| Sizes on the 5600 | up to 3440x1440@60, 3840x2160@30 | EDID modes (60 Hz) | EDID modes, CVT timings | the device table (2048x1152) |
| Monitors it can't fully drive | reduced blanking, lower rate, 1440p for 4K | | out-of-range fixes | |
| Partial updates | raw changed rows, tear-free (3 buffers) | JPEG rectangles | JPEG rectangles | JPEG rectangles |
| Hardware cursor | 64 / 96 px, negative positions | 64 / 96 / 128 px | yes | yes |
| Gamma | software, any feature level | hardware, level 8+ | | |
| Rotation | compositor | the OS | the OS | hardware (0x1f, not on the 5600) |
| Hotplug | events, debounced, re-set mode | events, debounced | events | events, re-init |
| Transfer errors | cancel (restarts the 5600), resend | cancel | clear stall, retry | cancel |
| Capability checks | feature level, VRAM, DISP flags | yes | yes | yes |
| USB 2 link | smaller writes, modes up to 2048 wide | same | smaller packets | JPEG |
| HDMI audio, second output | no (5600 has neither) | yes | yes | yes |
| Suspend / DPMS | yes | yes | yes | |

