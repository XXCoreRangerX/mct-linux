# Optimization ideas not tried yet (2026-10-10)

> **Research list** (2026-10-10, Trigger II): ideas and what came of those tried; current measurements are in t2-driver.md.

Measured so far (see t2-driver.md): the T2 takes RLE data at about
37-40 MB/s no matter how the host queues it, a lossless full-screen update
costs ~26 ms, desktop use sends ~16% of the damaged pixels, encoding is ~9%
of the time. Already tried without a gain: async URBs with striped encoding,
larger USB transfers (~3%, and very large ones corrupt frames). Ready on
branches but not yet tested live: latency histogram and payload counters
(`counters`), adaptive lossy tolerance (`adaptive-lossy`). (Both merged
since, in 0.3.18 and 0.3.19.)

Ideas, roughly by expected payoff:

1. **Measure latency first.** The `counters` branch adds a damage-to-flip
   histogram. Without it, "laggy" can't be split into USB time, waiting for
   the worker, and compositor/vblank pacing. Check whether the vblank timer
   adds up to a frame of delay on top of the transfer and whether niri waits
   for a flip completion that only comes after the whole USB transfer.

2. **A smaller pixel format on the device.** The framebuffer is 3 bytes per
   pixel (stride 3072). The panel is fed through a VGA scaler (RTD2280) and
   the LVDS panel is likely 6 bits per channel, so a 16-bit mode would lose
   almost nothing visible while cutting raw data by a third. Look in
   T2Indisp.dll for other 0x13 header flags (`004000400000003900`,
   `300528`) and mode setups with 2 bytes per pixel. Unknown whether the T2
   has such a mode at all.

3. **Per-channel lossy tolerance.** If lossy stays, let blue (and red) absorb
   more than green, where the eye is most sensitive. Same encoder, a
   tolerance per plane. Combine with the adaptive tolerance branch.

4. **Hardware copy/scroll.** The DLL has transfer/stretch operations that
   aren't understood. A VRAM-to-VRAM copy would turn scrolling into a tiny
   command plus the newly exposed rows, the biggest win for desktop use.
   Needs reverse engineering in the DLL and captures of Windows scrolling.

5. **Cost-based damage band merging.** Decide per gap whether spanning
   unchanged rows is cheaper than another header. Small expected gain, since
   desktop traffic is already low.

6. **Front-buffer writes for tiny updates.** A few changed pixels (a blinking
   caret, a clock) could go straight into the visible buffer and skip the
   flip, saving the copy to keep both buffers in sync. Risks tearing, so only
   for updates a few rows tall.

7. **Panel side.** The vendor's Windows control panel may have display
   settings beyond brightness (auto adjust, phase, sharpness). The image is
   oversharpened by the RTD2280, and the rainbow-line issue is a scaler
   sampling-phase problem. Brightness goes through the pen's HID feature
   report 0x06; the pen has more feature reports with unknown meaning
   (0x02-0x0d, 0x10, 0xb0, 0xcc; 0x40 is the pen data encryption handshake,
   never write it). Update 2026-10-11: Wacom's driver has no setting beyond
   brightness for this model, see wacom-display-settings.md. Find their use statically: unpack WacomTablet_6.4.9-2.exe
   (7z works) and search the control panel's strings and resources, then
   capture any setting found in the Windows VM like brightness was. Wine
   can't help here: the control panel needs Wacom's kernel drivers to see
   the tablet.

Not worth more effort: async URBs, bigger USB transfers, diff loop tuning,
refresh rates above what the link carries (a full lossless frame alone takes
~26 ms, so complex full-screen content tops out around 38 fps; only lossy or
a smaller pixel format can raise that).
