# MCT Trigger 6 (T6) protocol

> **Reference**, kept current with the code. **V** / **C** mark what was verified on hardware and what was only seen in captures or vendor code.

Worked out from 2026-10-10 on a **T6 USB Station** (USB `0711:5600`, board
"JUA254H") driving an LG 27GN750 over HDMI. Sources, in the lab directory
(not published):

- the [trigger6](https://github.com/JingYenLoh/trigger6) Linux driver and its
  Windows USB capture of a `0711:5601` (`captures/t6-analysis/`,
  `captures/t6-research/capture-analysis.md`);
- static analysis of MCT's Windows driver 25.01, T6InDisp.dll and t6sta.sys
  (`captures/t6-research/dll-analysis.md`);
- MCT's own Linux daemon (EVDI + `T6evdi`; the Ubuntu package carries no
  licence, but MCT publishes the same code base under GPL-2.0-or-later as
  [triggerdm](https://github.com/mcttrigger/triggerdm); read for facts
  only, no code taken), cyrozap's notes, y-chan's and ramifriedman's drivers
  (`captures/t6-research/web-research.md`, `third_party/t6-sources/`);
- tests on the 5600 with `tools/t6dev.py` and the `tools/t6_*_test.py`
  scripts, logged in [t6-lab-notebook.md](../history/t6-lab-notebook.md).

**V** = verified on the 5600, **C** = seen in the capture or vendor code only.
Nothing here shares code or framing with the T2.

## Device

| | |
|---|---|
| USB | 3.1 Gen 1 (5 Gbit/s), `bcdDevice` 0x1010, "MCT Corp." / "T6 USB Station" |
| Interface 0 | class 0xff, one altsetting, the only interface on the 5600 |
| EP 0x02 | bulk OUT, 1024 B packets, bMaxBurst 15: frames and other data (V) |
| EP 0x81 | bulk IN: unused by all known drivers |
| EP 0x83 | interrupt IN, 64 B: frame-done and hotplug events (V) |
| VRAM | request `0x88` x 1 MiB (5600: 58 MiB, V) |
| CPU | Andes NDS32 running uC/OS-II (cyrozap) |

## Control requests (vendor, recipient device)

| Request | wValue | wIndex | Data | Meaning |
|---|---|---|---|---|
| `c0 87` | output | 0 | 1 B in | monitor present: 1 = yes (V). A sleeping monitor reads 0 |
| `c0 80` | block x 128 | output | 128 B in | EDID block (V) |
| `c0 89` | output | byte offset (0, 512) | 512 + 448 B in | mode table, 30 x 32 B (V) |
| `40 12` | output | 0 | 32 B out | set mode, one timing entry (V, also custom entries) |
| `40 31` | 0 | 0 | none | "software ready", after every mode set (V) |
| `40 03` | output | 1 on / 0 off | none | output on/off (V) |
| `40 10` / `40 11` | cursor slot | byte offset | up to 4096 B out (V) | cursor image, output 0 / 1 (V for 0x10) |
| `40 05` / `40 07` | cursor slot | 1 show / 0 hide | none | select and show / hide the cursor (V for 0x05) |
| `40 04` / `40 06` | x | y | none | cursor position, top left of the image (V for 0x04) |
| `c0 88` | 0 | 0 | 1 B in | VRAM size in MiB (5600: `3a` = 58) |
| `c0 b0` | 0 | 0..5 | 4-8 B in | 0 platform (0 "Lite", 48 MHz PLL base; 1 "Super Lite", 40 MHz), 1 boot code, 2 image code, 3 project code (5600: "JUA254H", 5601: "A8613HV"), 4 feature level, 5 serial |
| `c0 b1`, `c0 b3` | 0 | | 132 / 112 B in | function descriptors, "DISP" section (flags, quality, bandwidth cap) (C) |
| `40 1c`, `40 1d` | output << 8 (\| 2 enable) | 0x100 / 0x200 / 0x400 | 256 B out | gamma enable and one table per channel (C) |
| `40 14` | 0 | 0 | none | cancel after a failed bulk write (C); on the 5600 it restarts the device: it leaves the bus and re-enumerates ~3 s later (V) |
| `40 30` | | level | none | hardware reset (C) |
| `40 1f` | | 0..3 | none | hardware rotation (C); stalls on the 5600, also with 0 (V) |
| `40 23`, `40 24`, `c0 a1..a5` | | | | audio: `a1` engine caps, `a2`/`a3` node caps/values, `a4` engine state, `a5` formats, `24` set engine state, `23` node value (volume). **V** 2026-10-12: playback works on the 5600, see "Audio" below |

Outputs: the 5600 drives its HDMI port as output 0, the 5601 in the capture
as output 1. Probe `87` for both.

### Timing entry (32 bytes, little endian)

```
u32 pixel clock (kHz)   u8 refresh (Hz)  u8 reserved
u16 htotal  hdisplay  hsync start  hsync width
u16 vtotal  vdisplay  vsync start  vsync width
u16 PLL P   u16 PLL Q   u8 PLL N   u8 PLL output select
u8 hsync polarity  u8 vsync polarity  u8 reduced  u8 flags (bit 0: custom)
```

PLL (MCT's formula, V): with f the pixel clock in MHz and base 48 or 40 MHz
(`b0` index 0), take the first x of 4, 8, 16, 32 with f * x >= 800; then
N = floor(f * x / base), P = fraction * 1000, Q = 1000, output select =
{3, 1, 2, 0}[index of x]. Computed entries reproduce the device's own table.

The device's table on the 5600 goes up to 2048x1152@60 (197 MHz), but any
timing up to **about 350 MHz pixel clock** works, independent of refresh
rate (V): 1080p at 120, 144 and 150 Hz, 720p at 240 Hz. Artifacts from
~350 MHz (green pixels at 348 in long use, bars at 353), no signal from 360.

## Bulk data (EP 0x02)

Every unit starts with a **32-byte session header in its own transfer**, then
the payload:

```
u32 signature     0 display, 3 audio, 5 ROM, 7 NV12 rectangle write
u32 payload length
u32 address       VRAM byte address the payload is written to
u32 fragment size
u32 fragment offset
u32 flags         1 = more fragments follow (on all but the last)
u32 0, 0
```

Display payloads larger than 0x19000 bytes are sent in fragments, each with
its own header (offset and fragment size per fragment, the same address and
total length). The Windows kernel driver does this slicing (0x6400 bytes on
USB 2).

### Display commands (signature 0)

The payload starts with a **48-byte command**, the rest of the payload is its
data, written to VRAM from `address + 48` on:

```
u32 command       3 flip output 0, 4 flip output 1, 1/2 band update, 7 rectangle (FB_DIRECT)
u32 data length   bytes after this header (image only)
u32 fence ID      echoed in the frame-done event
u32 target format format of the scanout surface
u16 pitch 0, u16 pitch 1
u32 plane 0, plane 1, plane 2 addresses
u32 source format of the data sent (0xd = JPEG, else the same as the target)
...               x/y offset and size for centring, flags (byte 47: 0x80 reset JPEG)
```

Formats: 0 YUYV, 4 YV12/I420, 6 **NV12**, 8 RGB32, 9 BGR24, 10 YUV24,
11 YUV444, 13 JPEG.

The **flip is implicit**: scanout switches to the planes of the newest flip
command once its data length has arrived (V: a frame sent slowly over 3 s
appears at once). The data length must be exact; 48 bytes too many make the
chip wait for bytes of the next frame.

**Raw NV12 full frame** (V; used by early mct_t6 versions, the driver now
sends rectangle writes and empty flips only):
address = fb, plane 0 (Y) = fb + 0x30, plane 1 (interleaved CbCr) = plane 0 +
pitch * height, plane 2 = plane 0, pitches = width (align 16), target =
source = 6, data = Y then CbCr rows. BT.601 limited range works; the vendor
driver converts with full range.

Frame buffers: one output uses three, at 0x0, 0xfffec0 and 0x1fffd80 (V). Never
write the one on screen.

**Empty flip** (V): the same command with data length 0 (session payload
length 48) shows a frame buffer that is already filled.

**Raw BGR24** (trigger6's layout, V): address 0x30, plane 0 = plane 1 = 0x60,
pitch 0 = width * 3, target = source = 9. Writing into the visible buffer
tears; even into separate buffers it switches without vsync, so tears show.
NV12 flips are tear-free. (Superseded: with the exact data length and pitch
align32(w) * 3, BGR24 and RGB32 flips are tear-free too, see the notebook;
the driver's layouts are in `t6_set_layout`.)

JPEG (vendor drivers, C): JPEG data goes to a ring from VRAM - 0xA00000
(0x3000000 on 58 MiB) and is decoded into an NV12 frame buffer; payload padded
with >= 1 KiB, total rounded up to 1 KiB.

### Rectangle writes (signature 7)

```
u32 7
u32 data length
u32 Y address     plane 0 + y * pitch + x
u32 UV address    plane 1 + (y / 2) * pitch + x
u16 Y pitch, u16 UV pitch
u16 width, u16 height
u32 0, 0
```

Data: height rows of Y (width bytes each), then height / 2 rows of CbCr. One
header per write, no fragments (mct_t6 keeps each write under 0x19000 bytes,
0x6400 on a USB 2 link).
Writes into a hidden frame buffer don't show until a flip to it (V). Partial
update: write what changed into the next frame buffer, then an empty flip
(V, 325 fps of a moving block from Python).

### Interrupt events (EP 0x83, 64 bytes)

Byte 0x13: 0x04 frame done (fence ID at 0x0c, V), 0x01 / 0x02 monitor change
on output 0 / 1 (V for output 0), 0x08 JPEG error, 0x10 redraw request (C;
mct resends picture and cursor). Frame done is not locked to vblank (0.1 to
10 ms after the data, V); whether it comes at all varied between runs.

## Cursor

Image: 8-byte header `{u16 kind (1 = ARGB with alpha, 2 = mask), u16 width,
u16 height, u16 pitch}` then B, G, R, A pixels, 64, 96 or 128 square, sent in
pieces of up to 4096 bytes with `40 10` (wIndex = byte offset; the vendor
uses 512). wIndex is 16 bits, so a 128 px image (65,544 bytes) can only be
sent up to 0x10000: its last two pixels are never sent, which works (V). 5 slots per output
(output 1 from slot 5). `40 05` with a slot selects and shows it; uploading
into the slot on screen can leave the cursor scrambled, so upload into
another slot and select it (V). Position `40 04`: top left of the image,
wValue x, wIndex y; negative values as v - 1 (V).

## Throughput (5600, measured)

- USB: ~330-360 MB/s while busy, synchronous 100 KB transfers.
- Full raw NV12 1080p: ~98 fps; raw BGR24: ~52 fps; NV12 720p: ~195 fps.
- Partial updates only cost what changed.

## Audio (verified 2026-10-12 on the 5600, `tools/t6_probe.py` step D)

- Descriptors: one playback engine (`a1` index 0; index 1, capture, returns
  nothing), one format (`a5`: 48 kHz, 16-bit, 2 channels), one volume node
  (`a2`/`a3`: 0-31, 15 at start), cyclic buffer 128 KiB (`a4`).
- Start: `40 24` with 16 bytes `{Activity 1, 0, FormatIndex u16, 0 u32,
  ReturnSize u32 = 9600, CyclicBufferSize u32}` (MCT; for playback the
  Activity byte is ignored, the bulk stream drives the engine).
- Data: bulk sessions with signature 3: a 32-byte header `{3, n, 0, n, 0,
  flags u8 at byte 20 = 0, function-specific at 24: Audio.Flags (0x01 =
  restart engine), EngineIndex, 6 reserved}` in its own transfer, then n
  bytes of interleaved 16-bit little-endian stereo PCM; a 9600-byte prefill,
  then 1920-byte (10 ms) packets every 10 ms. Heard on the LG's headphone
  jack, with and without the restart flag: it is not required (the
  stream from MCT's and Windows' drivers, flag 0, works).
- Stop: `40 24` with Activity 0 (Windows does it; MCT's Linux daemon
  never stops the engine).
- `a4` reported "stopped, position 0" during playback too: not usable as a
  progress counter on this firmware.
- Audio plays only while the output is on with a mode set (HDMI carries
  audio inside the video signal), and only cleanly up to 297 MHz pixel
  clock: 1080p 60/75/120 clean, 1080p144 (333 MHz) buzzes on the LG
  27GN7, with the same stream (2026-10-12, mct_t6 0.29).
- Interrupt events (MCT's `T6INTERRUPTDATA`, `t6bulkdef.h`): byte 0 is a
  function mask (0x04 video, 0x20 audio); at 0x20 a u32 with the audio
  render DMA pointer (bits 0-23) and jack bits (24 jack 0, 25 DVO HDMI, 26
  LVDS HDMI). Whether the 5600 sends audio events during playback is not
  known yet (C); mct_t6 0.29.0 counts them in debugfs (`audio` line).
- The driver (mct_t6 0.29.0) does what MCT's daemon does: `40 24` with
  Activity 1 when the stream starts, 10 ms packets paced by the host clock
  about 90 ms ahead of playback, Activity 0 when it stops.

