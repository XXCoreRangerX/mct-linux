# mct compared with the vendor drivers

How mct compares with MCT's own drivers for the same devices: first what the
drivers do, from analysing them, then numbers measured side by side.

## What each driver does

**Trigger 6** (j5create JUA254)

| | mct | Vendor drivers (Windows / macOS) |
|---|---|---|
| Modes | from the monitor's EDID: 1080p up to 144 Hz, 720p up to 240 Hz, 3440x1440 at 60 Hz, 4K at 30 Hz | from the monitor's EDID, 60 Hz |
| Picture | exact RGB or YUV 4:4:4 at rest, NV12 during large changes; only changed rows are sent, uncompressed | JPEG (quality 95), partial updates as JPEG rectangles; Windows refreshes losslessly at rest |
| Hardware cursor | ✅ recent shapes kept on the adapter | ✅ recent shapes kept on the adapter (Windows) |
| HDMI audio | ✅ 48 kHz stereo, volume | ✅ |
| Gamma / night light | ✅ in software | ❌ on this adapter (Windows uses hardware gamma tables, which need a newer chip) |
| Second output (docks) | ❌ | ✅ |
| USB 2 links | ⚠️ works, slower for full-screen motion | ✅ JPEG keeps it fast |

**Trigger II** (Wacom DTU-1031X)

| | mct | Vendor drivers (Windows / macOS) |
|---|---|---|
| Picture | lossless by default (lossy only if turned on); tear-free, since updates go to the hidden buffer and then flip; only changed pixels are sent | slightly lossy (the encoder always merges values within ±4 into runs); partial updates go straight into the buffer on screen |
| Hardware cursor | ✅ | ✅ |
| Backlight (the only display setting Wacom offers) | ✅ standard backlight device | ✅ |
| Recovery from a confused chip | ✅ automatic | not known |

The analysis behind these tables:
[t6-vendor-comparison.md](research/t6-vendor-comparison.md),
[t6-vendor-review.md](research/t6-vendor-review.md),
[t2-vendor-driver-analysis.md](research/t2-vendor-driver-analysis.md).

## Measured side by side

Each driver ran in a virtual machine, all of them seeing the device through
the same USB redirection, with mouse moves injected the same way. That path
adds its own delay, so only the differences between drivers are meaningful.
MCT's Linux driver for the T6 is a userspace EVDI daemon; there is none for
the Trigger II. Method, all numbers, and how the devices were protected
(firmware writes blocked):
[vendor-vm-comparison.md](research/vendor-vm-comparison.md).

**Trigger 6** at 1080p60. Mouse move to USB with a 1000 Hz mouse, median /
95th percentile; mct with niri and the
[immediate cursor patches](research/cursor-latency.md):

| | mct | MCT Windows | MCT Linux (EVDI daemon) |
|---|---|---|---|
| Mouse move → USB | 0.44 / 0.50 ms | 0.39 / 0.48 ms | 10.1 / 14.9 ms (no hardware cursor; every move is a new frame) |
| Bytes per move | 8 | 8 | ~42 KB |
| 1080p50 video | 50 fps, raw, steadiest pacing | 50 fps, JPEG, bursty | 50 fps, JPEG |
| USB traffic at rest | none | none | none |

**Trigger II**:

| | mct | MCT Windows |
|---|---|---|
| USB traffic at rest | none | re-sends the 16 KB cursor image every 3 s |
| Video | 29.5 fps, steady | 28.8 fps, stalls up to 174 ms |

## Cursor times in the driver's own statistics

`tools/cursor_latency.sh` reports the time from the compositor's commit to
the device: about 0.1 ms for a move on the T6 and 0.03-0.06 ms on the
tablet, about 0.5-0.8 ms for a new cursor shape. The two chips count
differently: the T6 takes a move as a USB control request, done once its
firmware has confirmed it; the tablet as a 16-byte bulk write, done once
its USB endpoint has the bytes. Either is a small fraction of one refresh
(6.9 ms at 144 Hz, 16.7 ms at 60 Hz).
