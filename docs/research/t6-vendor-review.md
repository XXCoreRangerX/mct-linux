# Vendor drivers vs mct: open differences (review of 2026-10-11)

> **Research snapshot** (2026-10-11), with a status per item: what became of each difference.

A second pass over MCT's Ubuntu source (`third_party/t6-sources/linux/evdi_t6_1`),
the decompiled Windows T6InDisp.dll (`captures/t6-research/dll-analysis-files/drv2.c`;
both in the local lab directory, not published),
the Windows capture and the macOS driver binaries, looking for what they do that
mct doesn't. Things already covered in [t6-vendor-comparison.md](t6-vendor-comparison.md)
(JPEG, audio, dual output, rotation, idle refresh, full-range YUV) are left out.
Ranked by expected value.

Status (mct_t6 0.21.1): done 1 (`0x14` turned out to restart the 5600), 2, 3,
4, 5, 6, 8, 10 (read, logged, feature level and memory gated), 12, 14 (128 px
with the upload cut at 0x10000). Tried and dropped: 7 (flag 2 doesn't
fix the 3440x1440@60 monitor timing), 15 (frame done isn't tied to the
refresh). Open: 9, 11, 13, 16.

## Robustness

1. **Recovery after a failed or stuck bulk transfer.** Ubuntu sends `40 14`
   (cancel bulk out) after every failed bulk write and drops the frame
   (`t6usbdongle.c`, `t6_libusb_donglereset`); Windows does the same on a 10 s
   write timeout; the macOS dext clears a stalled pipe and retries. mct only
   records the error and invalidates its buffers; URBs never time out, and a
   session cut halfway leaves the chip's parser waiting for the rest of the
   declared length. Proposed: on a failed URB or a URB wait timeout, kill the
   anchored URBs, `usb_clear_halt` on -EPIPE, send `40 14`, resend whole frames.
   No USB reset. Test by killing a URB mid-frame, as done for the T2.
2. **Redraw request event (byte 0x13 bit 0x10)** is only counted. Windows sets
   "force full frame" for every head and re-sends the cursor on it. Proposed:
   invalidate all three buffers, mark all rows changed, re-upload the cursor.
3. **Hotplug debounce.** Windows re-reads `0x87` every second after an event
   and reports a change only after 4 equal readings; Ubuntu re-inits the device
   (incl. set mode) after an unplug. mct reports right away and also polls
   every 10 s; `0x87` reads 0 while a monitor sleeps or shows another input, so
   outputs can flap, and a fast replug or monitor swap never re-sets the mode or
   re-reads the EDID. Proposed: debounce in `hpd_work` (3 to 4 equal reads
   250 ms to 1 s apart), re-send mode, `0x31`, output on, cursor and a whole
   frame on a 0 -> 1 change while active.
4. **Interrupt endpoint** stops for good on -EPROTO/-EILSEQ/-EPIPE; Windows
   resets the pipe and keeps reading, macOS clears the stall. Both check byte 0
   bit 2 (video function) before byte 0x13. Proposed: clear halt from a work
   item and resubmit with backoff; check the function bit.

## Modes

5. **Generated timings.** macOS builds reduced blanking v1 (hblank 160) first
   and v2 (hblank 80) only when the clock exceeds the port limit, and checks the
   EDID range descriptor's pixel clock and line rate (with a fix-up for EDIDs
   whose own modes exceed it). mct always uses an 80 px hblank and checks only
   the vertical range. Proposed: v1 via `drm_cvt_mode(..., reduced)` where it
   fits, v2 only when needed; filter added modes by the monitor's max pixel
   clock and line rate.
6. **No or invalid EDID** (KVMs, switches): Windows allows table modes up to
   1920x1080, Ubuntu uses a built-in 1080p EDID; mct gets the DRM default
   1024x768. Proposed: add the device table modes up to 1080p, 1080p60 preferred.
7. **Mode entry flags.** Windows sets flag byte 2 = 2 for widths 1360, 1680 and
   widths above 1920 that are not a multiple of 32 (3440 is one), and writes
   sync start + 1 / sync width - 1 for EDID-derived modes; Ubuntu and macOS send
   the table's own entry when a mode matches one. mct sends flags 0. A cheap
   experiment: the MO34WQC2's own 3440x1440@60 timing with flag 2 set (the one
   that gave a black screen).
8. **50 ms pause** between output off and set mode (Ubuntu). mct does them back
   to back.
9. **Mode table read** needs exactly 960 bytes; vendors read count x 32 (count
   from `0x84`).

## Capabilities

10. **Feature gating from descriptors.** Vendors check `b0` index 4 (feature
    level: below 3 means JPEG/I420 only; gamma needs 8 or more), the `b3` DISP
    descriptor (DispFunc bits 0-1 hotplug per output, bit 3 JPEG reset, bits 4-7
    rotation (Ubuntu uses `40 1f` only when non-zero), bit 7 lossless BGR idle
    refresh; LinkInterfaces per output, VGA gets no `0x87` and a generic EDID;
    table entry count) and `0x88` VRAM (Windows wants 20 MiB or more). Windows
    gamma table order: wIndex 0x100 blue, 0x200 green, 0x400 red. Proposed:
    read and log these at probe, gate formats and requests on them. Windows'
    buffer stride `align32((VRAM - 0x400) / 3)` would be ~20.3 MB on 58 MiB,
    enough for RGB32 at 3440x1440.
11. **Output picked once at probe**: on a 5601 with no monitor at probe, output
    0 stays forever. Proposed: switch on a change event for the other output.
12. **USB 2 links** (hubs, KVMs): vendors use smaller slices (0x6400), drop modes
    wider than 2048 or switch to JPEG. Proposed: on high speed, smaller
    fragments, NV12 only except for refreshes at rest, maybe cap at 1080p.
13. **Device IDs**: vendors match 0711:5600-561f and 5650-565f (Ubuntu also HP
    03f0:0182); mct matches 5600 and 5601. Only with the gating of item 10.

## Cursor

14. Negative positions are sent as `v - 1` by both Windows and Ubuntu (mct
    shifts the image instead and re-uploads 16 KiB per move at the top/left
    edge); Ubuntu uploads only the real cursor size in 4096-byte transfers;
    Windows supports 64, 96 and 128 px cursors (mct: 64, larger ones fall back
    to a software cursor). Proposed: native negative positions (test first),
    cursor size per backend up to 128, upload only the real size.

## Frame-done

15. Ubuntu's raw frames carry 1 KiB of zero padding counted in the payload
    length; macOS tracks a fence per frame before reusing a buffer. mct's exact
    length recipe gets no frame-done events. Experiment: padded flips, to see if
    the events (and with them a "buffer left scanout" signal) come back.

## T2 (Wacom)

16. The macOS T2 backend has `T2_startDDRclock`, `t2_dram_phase_fallback_logic`,
    `readAndParseT2EEPROM` and power-down sequences; Windows' T2Indisp.dll logs
    "failed to adjust dvo driving". Random-per-init lines that only a full
    re-init clears fit a DDR/DVO clock phase problem as well as analog sampling.
    Next: reverse `T2_startDDRclock` and the phase fallback in the macOS x86_64
    slice and compare with our init.

## No change needed

`0x31` after every mode set, skipping interlaced modes, the PLL limits, no
HDMI/DVI infoframes, suspend/resume: same as the vendors.
