# MCT Trigger 6 (T6) protocol notes

Started 2026-10-10 with a **T6 USB Station** (USB `0711:5600`) driving an LG
27GN7 over HDMI. Sources: the [trigger6](https://github.com/JingYenLoh/trigger6)
Linux driver, its Windows USB capture (`captures/mctt6.pcapng` in that repo,
a `0711:5601`), decoded into `captures/t6-analysis/` in the lab directory, and
tests on the 5600. Nothing here shares code or framing with the T2.

## Device

| | |
|---|---|
| USB | 3.1 Gen 1 (5 Gbit/s), `bcdDevice` 0x1010, strings "MCT Corp." / "T6 USB Station" |
| Interface 0 | class 0xff, one altsetting |
| EP 0x02 | bulk OUT, 1024 B packets: frames (and audio on audio-capable models) |
| EP 0x81 | bulk IN, 1024 B: never used by the Windows driver in the capture |
| EP 0x83 | interrupt IN, 64 B, bInterval 5: frame-done notifications |
| Chip ID | vendor request `0xb0` index 3 returns "A8613HV" (5601) |

The 5600 shows no audio or network function, only the display interface.

## Control requests (vendor, recipient device)

| bmRequestType / bRequest | wValue | wIndex | Data | Meaning |
|---|---|---|---|---|
| `c0` / `87` | output | 0 | 1 B in | monitor present: 1 = yes |
| `c0` / `80` | byte offset (block x 128) | output | 128 B in | EDID block |
| `c0` / `89` | output | byte offset (0, 512) | 512 + 448 B in | mode table, 30 x 32 B |
| `40` / `12` | output | 0 | 32 B out | set mode (one mode table entry) |
| `40` / `03` | output | 1 = on, 0 = off | none | enable/disable the output |
| `c0` / `b0` | 0 | 0..5 | 4-8 B in | device info words (Windows reads these first) |
| `c0` / `b1` | 0 | 0 / 3 | 132 B in | function descriptors: "Trigger 6 External Graphics", "USB Audio Device" |
| `c0` / `b3` | 0 | 0 | 112 B in | "DISP" descriptor |
| `c0` / `88` | 0 | 0 | 1 B in | unknown (`3a`) |
| `40` / `1c`, `40` / `31` | | | none | sent by Windows before probing outputs; unknown |
| `40` / `1d` | `0100` | `0100` / `0200` / `0400` | 256 B out | gamma ramp per channel (R/G/B), then `1c 0102` |
| `40` / `11` | 5 | offset, step 0x200 | 512 B out | cursor image upload, 64x64 ARGB |
| `40` / `07` | 5 | 1 / 0 | none | cursor show / hide |
| `40` / `06` | x | y | none | cursor position |
| `40` / `24`, `40` / `23`, `c0` / `a1..a5` | | | | audio setup |

**Output index differs between models.** On the 5601 in the capture, the HDMI
monitor is output 1 (`87` with wValue 0 returns 0, wValue 1 returns 1); on the
5600 here it is output 0. A sleeping monitor (or one on another input) reads
as not present, so the status can go 0 -> 1 later; poll it.

### Mode table entry (32 bytes, little endian)

```
u32 pixel clock (kHz)   u16 refresh (Hz)
u16 htotal  hdisplay  hsync start  hsync width
u16 vtotal  vdisplay  vsync start  vsync width
u16 unknown x3          u8 hsync polarity, u8 vsync polarity   u16 unknown
```

The same 32 bytes go to `40/12` to set a mode. The driver matches EDID modes
against this table by width, height and refresh.

## Frames (bulk EP 0x02)

Every frame is split into fragments of at most 0x19000 bytes. Each fragment is
preceded by a **32-byte session header in its own transfer**:

```
u32 type        0 = video, 3 = audio
u32 total_len   whole frame (video header + image)
u32 dest        see below
u32 frag_len
u32 offset      byte offset of this fragment in the frame
u32 more        Windows: 1 on all but the last fragment
u32 0, 0
```

The frame starts with a **48-byte video header**:

```
u32 type        Windows: 4 = full frame, 7 = partial rectangle
u32 data_len
u32 seq
u32 unk         Windows: 6
u16 width, u16 height   (meaning depends on type, see below)
u32 start, u32 end      framebuffer addresses
u32 unk
u32 format      0xd JPEG, 0x6 NV12, 0x9 BGR24
u32 unk x3
```

### What works on the 5600 (raw BGR24, used by mct_t6 0.1)

trigger6's layout, verified with modetest colour bars and under niri:

- session: type 0, `dest = 0x30`, offset per fragment, `more = 0` always
- video header: type 3, `data_len` = whole frame, seq 1, unk 9,
  `width = hdisplay * 3`, height 0, `start = end = 0x60`, format 9
- image: hdisplay x vdisplay pixels, 3 bytes each in B, G, R order, no padding

A 1920x1080 frame is 6.2 MB.

### What Windows sends (5601 capture)

- Always JPEG (format 0xd), 34-325 KB per full 1360x768 frame, 5-16 KB per
  partial rectangle.
- Session `dest` is a ring buffer: `0x03000000` plus a running offset that
  advances by each frame's total length.
- Type 4 (full): `width = height = 0x550` (looks like the stride, 1360), then
  `start`/`end`, with `end - start = 1360 x 768`; start moves ~8 MB per frame.
- Type 7 (partial): rectangle `w, h` first, then `0x550, 0x550`, then start/end,
  then format; every field after the rectangle is one dword later than in type 4.
- Each frame is followed by a 64-byte notification on EP 0x83 (`04 00 ...`, a
  counter at byte 12 matching the frame's seq). There was no hotplug event in
  the capture.

## Getting trigger6 to work on the 5600 (kept in lab `third_party/trigger6`, branch `t6-5600-fixes`)

1. Match 0x5600 as well as 0x5601.
2. Bulk frames went to endpoint 4; the device's OUT endpoint is 2.
3. Advertise `DRM_FORMAT_MOD_LINEAR` for the primary plane.
4. Drop its `gem_prime_import` override (`drm_gem_prime_import_dev` with the
   USB host controller as DMA device). On Linux 7.2 the shmem helpers import
   dma-bufs without mapping them (`drm_gem_shmem_prime_import_no_map`); with
   the override, niri 26.04 could not set up the output and reported
   "No supported plane buffer format found" (its error for the last colour
   format it tried, not the real failure). modetest worked either way since it
   uses dumb buffers.

## Open questions

- Partial updates with raw BGR24: does type 7 work with format 9, and what
  goes into start/end? Needed to stop resending the whole screen.
- What `dest`, `start` and `end` address, and whether the ring layout is
  needed for anything beyond pipelining.
- NV12 (format 6) halves the data of BGR24; JPEG would need an encoder.
- The `b0`/`1c`/`31` setup Windows sends before probing: required on any model?
- Hardware cursor and gamma (requests above), audio.
