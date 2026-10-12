# Trigger II (T2) bulk protocol

> **Reference**, kept current with the code.

Source: cyrozap's [Protocol-T2.md](https://github.com/cyrozap/mct-usb-display-adapter-re/blob/master/doc/Protocol-T2.md)
(CC BY-SA 4.0) + live probing + analysis of the vendor drivers for
interoperability (`T2Indisp.dll`, and since 2026-10-12 the older kernel
driver `t2usb64.sys` from the same installer, whose assert strings name the
command fields) + Win11 VM usb-redir and usbmon captures. The 2026-10-12
analyses are in the lab (`captures/t2-research/binary-analysis.md`,
`capture-analysis.md`). All multi-byte fields little-endian unless noted.

Marks: **verified** = tested on the tablet; **captured** = seen in Windows
traffic; **code** = read from the vendor drivers, not tested on the tablet.

Commands go to EP `0x03` (registers and mode setup) or `0x02` (framebuffer
pipe: `0x11`-`0x16`, executed in order with the frames); replies on EP `0x81`.
EP `0x04` OUT: register lists, first byte the bank (`01` fb, `03` fc, `04` fd,
`05` fe; code), then a length and `(reg val)` pairs; the cursor and the MC8032
setup use it.
EP `0x85/0x06`: not used by the Windows driver; live EP85 polls returned no
data either idle or mid-frame. **OUT/IN are paired**: after a command with a
reply, read `0x81` before the next OUT or the firmware stalls. Pace ~50 ms.

## EP `0x03` commands

| Cmd | Sent | Reply (`0x81`) | Meaning |
|---|---|---|---|
| `0x02` | `02 0001 <addr16> <val>` (6 B) | none | write 1 B register |
| `0x05` | `05 <addr_lo> <addr_hi>` (3 B) | 1 B | read 1 B register |
| `0x04` | `04 <len16> <payload>` | none | fc-bank `(reg val)` list; observed: `(6a,idx)(6b,val)` and `(a3..a7/f3..f7/b1..b5, val)` register-pair programs |
| `0x14` | `14 <len16> (reg val)*` (27 B in the init) | none | fb-bank pairs: `66/67` y, `68/69` x, `6a/6b` h, `6c/6d` w (a window), `65` SEL, then `6e 01, 60 01, 6e 01`: **display enable** (captured, field names inferred from code) |
| `0x16` | `16 <len16=0x20> <32 B pairs>` (35 B) | none | **timing packet**: regs `0x20-0x2f` = width/height-derived + mode params (full template in init doc) |
| `0x22` | 62 B | none | second mode packet (regs incl. `0xfc`-area mirrors); template TBD from driver |
| `0x0a` | `0a` + 768 zeros (769 B) | none | a 256x3 lookup table (code); the legacy driver pairs it with the 8-bit palette format; Windows sends zeros. Effect untested (`tools/gamma_test.py`) |
| `0x1b` | `1b8000a000800000` (8 B) | 512 zeros (idle) | EDID query (zeros until inited?) |
| `0x1d` | `1d8000ae00000100` (8 B) | 512 B | **device info**: `55aa00ff...`, `07115200`, `MCT`, `DTU-1031X`, **EDID at +0x80** |
| `0x28` | `28 <a> <b> 00` (4 B) | none | write MC8032 port 1, the counterpart of `0x29` (code; never sent by Windows) |
| `0x29` | `29000000` (4 B) | 512 B, first byte used | `CT2Hal::ReadMC8032P1`; live reply starts `0f 00...` |
| `0x90` | `90 040400 00061a80` (8 B), `90 030100 00` / `90 080100 02` (5 B) | none | mode/pipe setup variants |
| `0x91` | `91 <arg1> <arg16>` (4 B) | 512 B | ID-ROM query; driver uses `91 00 0200` -> reply starts `03 02 00 00...` |

### Detecting an interrupted framebuffer payload (verified 2026-09-25)

Leaving a `0x13` payload incomplete reproduces the garbage seen after some
driver swaps. While the EP2 parser waits for the declared compressed length,
EP3 commands are accepted by USB but not processed: both a `0x05` register
read and the vendor DLL's `0x29` MC8032-P1 read time out. Their replies appear
only after the missing EP2 payload bytes arrive. This timeout is the only
known observable indication that the frame stream is out of step; there is no
known independently readable parser-status register.

- Two complete idle sweeps of banks FB/FC/FE differed only at volatile
  `fc57` (`00`/`10`). A stuck-state sweep cannot proceed because its first
  `0x05` reply is held behind the unfinished payload.
- Five 100 ms EP85 reads returned no data both idle and mid-payload. The full
  Windows init capture likewise has no EP6/EP85 traffic; all replies use EP81.
- Static analysis found no video-memory readback command in `T2Indisp.dll`.
  It did reveal `0x29`, which is not present in the captured init, but live
  testing showed that it is serialized behind EP2 just like `0x05`.
- For a test with 924672 payload bytes missing, a queued `0x05` reply arrived
  after 938496 bytes of zero padding (47 ~~vendor-sized~~ 19968-byte transfers;
  EDIT (2026-10-12): 19968 is not the vendor's size but QEMU's split of
  Windows' 512 KiB transfers, see Transfer framing).
  Overshoot can be parsed as new commands and garble the following frame, so
  recovery must drain stale replies, run the normal init to realign the
  parser, and verify known post-init register values.

## EP `0x02` framebuffer pipe

| Op | Header | Vendor name (legacy driver) | Use |
|---|---|---|---|
| `0x11` | 21 B | `StretchCmd` | uncompressed rectangle write, with format conversion and scaling |
| `0x12` | 14 B | `DirRldCmd` | RLE decode straight to a VRAM address (the cursor upload uses it) |
| `0x13` | 36 B | `IndirRldCmd` | RLE decode, then a stretch into the target: the frame update |
| `0x14` | 48 B | firmware memory blit | VRAM-to-VRAM blit (code; legacy driver only, never sent by Windows 11) |
| `0x15` | 2 + 3n B | register batch | `(bank reg val)` triplets in order with the frames: the flip |
| `0x16` | 5 B | | a flip variant for 1366/1400-wide modes (code) |

The current Windows driver sends only `0x11`, `0x12`, `0x13` and `0x15`, and
only in the forms below. The other forms are known from code only.

### `0x13` frame update (36 B header + payload)

```
13 00 <src32> <dst32> <srcW16> <srcH16> <dstW16> <dstH16> <scaleX16> <scaleY16>
   <fmt8> <rld32> <rawlen24> <complen24> <esc8> <mode8> <flags8>  + payload
```

| Byte | Field | Value from Windows (all 178 captured headers) |
|---|---|---|
| 0, 1 | `13 00` | |
| 2 | source address (VRAM byte address, plain u32) | the target address |
| 6 | target address | buffer + y\*3072 + x\*3 |
| 10 | source width, height | the rectangle |
| 14 | target width, height | the same |
| 18 | scale x, scale y: 2.14 fixed point, = source/target | `0x4000` (1.0) |
| 22 | format | `00` (24-bit) |
| 23 | RLD buffer: VRAM address the compressed stream is staged at | `0x390000`, the first byte after framebuffer B |
| 27 | raw length = target w\*h\*3 | |
| 30 | compressed length = payload bytes | |
| 33 | escape byte | `0x30` (the RLE escape) |
| 34 | mode: plane layout | `05` = R, G, B planes per 1024-pixel unit |
| 35 | flags | `0x28` = `0x20` (`RldClock`, a legacy registry value; the same value goes to register `fe57`) \| `0x08` (`0x13`) |

- The stream is decoded into the source address, then stretched into the
  target. Windows decodes in place (source = target, scale 1.0).
- The framebuffer is always 3 bytes per pixel; other formats are converted
  by the chip (below).
- The `0x13` bytes on the wire equal the older reading of this doc
  (`addr << 8` twice, then `00`): for addresses below 16 MiB the two readings
  give the same bytes. The kernel driver builds the header field by field.
- Flag `0x04` marks an uncompressed payload (raw length = compressed length,
  escape `00`, mode `01`; code, inferred).

### `0x11` uncompressed rectangle (21 B header + payload)

```
11 <dst32> <srcW16> <srcH16> <dstW16> <dstH16> <scaleX16> <scaleY16> <fmt8> <len24> + payload
```

- Windows uses it only during init, with zero payloads (captured): clear
  buffer A and B (`1024 600 1024 600 4000 4000 00 1c2000`), and a 32x1 write
  at VRAM 0 (`0020 0001 0020 0001 4000 4000 00 000060`), between
  `90 040400 00001000` and `90 040400 00061a80`.
- 24-bit payload is **B, G, R** per pixel (code: `EncodeRgbDTR`), unlike the
  RLE planes, which are R, G, B.
- The payload is source w\*h\*bytes per pixel of the format (code).
- Earlier tests "ignored" fills with `0x1000` in the scale fields: that
  claimed a 4x stretch of a quarter-size source, so the payload size no
  longer matched (explained by the code, 2026-10-12).

### `0x12` direct RLE decode (14 B header + payload)

```
12 <addr32> <rawlen24> <complen24> <esc8> <mode8> <flags8> + payload
```

Decoded linearly to `addr`. The cursor upload is the only use (captured):
`12 00000000 004000 004000 00 01 25`, i.e. 16384 bytes stored uncompressed
(escape `00`, mode `01`, flags `0x20 | 0x04 | 0x01`).

### Source formats (code; RGB565 and YUV 4:2:2 verified 2026-10-12)

The legacy driver maps the desktop depth to a format byte per command; the
chip converts while writing, VRAM stays 24-bit:

| Source | `0x11` format byte | `0x13` format byte | Bytes per pixel |
|---|---|---|---|
| 24-bit (B, G, R) | `00` | `00` (mode 5, R, G, B planes) | 3 |
| 32-bit (BGRX) | `0e` | `00` (the encoder drops X) | 4 |
| RGB565 | `04` | not RLE-encoded by the driver | 2 |
| 8-bit palette (`0x0a` table) | `00` | `00` | 1 (inferred) |
| YUV 4:2:2 (YUY2) | `8a` | `8a` (mode 6: Y, U, V planes of 1536/768/768 per 3 KiB unit) | 2 |
| YUV 4:2:0 | `96` | `96` | 1.5 |

Windows 11 only ever sends 24-bit. cyrozap's T2 notes show a `0x11` with
format `04` and scale `0x1000` on the wire, which fits this table.

### Scaling (code; 2x upscale verified 2026-10-12)

`scale = source / target` in 2.14 fixed point: `0x4000` = 1.0, `0x2000` = a
2x upscale, `0x1000` = 4x. With source smaller than target the chip
stretches; the payload is the source size.

### Transfer framing (important: this caused the "lit but blank" screen)

The firmware parses EP `0x02` **per USB transfer, not as a byte stream**:

- Every command header (`0x11` 21 B, `0x12` 14 B, `0x13` 36 B, `0x15` 23 B)
  is sent as its **own bulk transfer** (a short packet ends it).
- The payload follows in **separate** transfers.
- Header+payload merged into one write is **silently ignored** (no stall,
  nothing drawn). Replays must keep the captured transfer boundaries.

Payload transfer size: Windows submits payloads as 512 KiB transfers. The
19968-byte pieces in the usb-redir captures are QEMU's emulated controller
splitting them (the pattern 19968 + 30 x 16384 + 12800 = 512 KiB repeats),
not a vendor rule; none of the pieces ends in a short packet, so the chip
cannot see them. The drivers here send 19968-byte transfers, which work. A
whole 837 KB frame sent as one transfer from Linux showed sparse corrupted
pixels near the end of the frame (2026-09-24); with the above, the cause is
more likely on the host side than a chip limit (inferred). Larger transfers
gain only ~3% (see throughput below).

### `0x15` register batch / buffer flip (EP `0x02`, 23 B)

```
15 <n=07> (bank reg val) x 7
15 07 fb 65 SEL  fc b4 11  fc b1 A0  fc b2 A1  fc b3 A2  fc b4 15  fc b4 11
```

- `(bank, reg, val)` triplets written in order through the framebuffer pipe
  (so they stay ordered w.r.t. frames).
- `A2:A1:A0` = scanout **byte address >> 2**; `SEL` = `0x10` for buffer A
  (`0x00C000` -> `00 30 00`), `0x20` for buffer B (`0x1CE000` -> `00 38 07`).
- `fcb4` 11->15->11 latches the new address.
- Windows double-buffers: `0x13` into the back buffer -> `0x15` flip to it ->
  next frame into the other buffer. **Without a flip nothing new is shown.**
- Same sequence also exists as EP3 register writes / `0x04` list during init.

### Video memory

| Address | Contents | flip `SEL` |
|---|---|---|
| `0x000000` | cursor image (64x64, 4 bytes per pixel) | |
| `0x00C000` | framebuffer A, 1024x600 RGB24, stride 3072 | `0x10` |
| `0x1CE000` | framebuffer B = A + 1024\*600\*3 | `0x20` |
| `0x390000` | RLD buffer: where `0x13` stages the compressed stream | |

The total video memory size is not known.

The flip latches the scanout address; the full layout of the `0x13` header,
including the exact values the tablet accepts, is in the `0x13` section
above (verified live for the 24-bit RLE form: full frames and partials).

### RLE payload (fully decoded 2026-09-24)

Source: `T2Indisp.dll` encoder @ `0x180013278` (called from `EncodeRleDTR`
@ `0x180011fe0`); verified by decoding a real Windows desktop frame and
re-encoding it **byte-identically** (`tools/t2rle.py`).

- **Planar per row**: for each row, three channel-lines **R, G, B**, each
  covering all `w` pixels (the encoder works in 1024-px units).
- Channel-line byte stream:
  - any byte `!= 0x30` -> **one literal pixel** of that value
  - `30 NN VV` -> **run** of `NN+1` pixels of value `VV`
- `0x30` is the escape, so pixel value `0x30` is never emitted as a literal;
  Windows maps it to `0x31` (invisible). Windows caps runs at `NN = 0xFA`
  (251 px) and never emits `NN = 0x30` (writes `30 2f VV VV` instead). Runs of
  1-2 px are written as literals.
- Windows' encoder is slightly lossy: it merges pixels within +/-4 of the run
  value into a run. Our encoder is exact (apart from `0x30`->`0x31`) unless the
  driver's optional lossy mode is on (see t2-driver.md).
- **Orientation: the panel scans out rotated 180 degrees.** The first pixel in the
  stream is the bottom-right of the visible image. Windows pre-rotates, and we
  reverse the pixel order (`tools/show_image.py::rot180`).
- Sizes: solid colour 27 000 B; photo wallpaper ~ 837 KB; worst case ~ 1.84 MB
  (all literals). Full-frame send + flip ~ 0.2 s from Python.

### Partial updates (verified live via wacom-t2d)

- Same 36 B `0x13` header with the rect's `w h w h`; both addresses =
  `buffer + y*3072 + x*3` (device coordinates, i.e. after 180 degrees rotation).
- Payload: the rect's pixels in **row-major order, cut into 1024-px units**; each
  unit is encoded as R, G, B channel lines (a full-width row is simply one unit).
  Hence `w*h % 1024 == 0`: the vendor's "3K alignment" (`w*h*3` a multiple of
  3072 bytes). Windows keeps `x`, `w` multiples of 64 and rounds `h` up; that
  overshoots row 600 (e.g. `64x608`, 8 rows into buffer B; done to buffer B
  it would run past the end of the framebuffers). mct_t2 widens or moves the
  rectangle instead, so it never writes past the buffer.
- Windows policy (code): the dirty rectangles are joined into one; if it
  covers 10% of the screen or more, a full frame goes into the back buffer +
  `0x15` flip, otherwise the rectangle goes straight into the **front**
  buffer, no flip. In the captures 66 full frames and 106 partials (5% of
  the bytes); 17 full frames went into the visible buffer followed by a flip
  to it, so repeated flips are harmless. After a full frame Windows often
  sends the same picture to the other buffer to keep both in step.
- Windows tries RLE first, raw `0x11` as fallback, and only on this tablet
  (`fc01` >= 7 and the device name "DTU-1031X"); other T2 devices get raw
  `0x11` only (code).
- All 178 frames in the captures decode under this model.

## Throughput (measured 2026-09-24, `tools/bench.py`)

USB 2.0 bulk tops out around 42-44 MB/s here, and full-frame updates reach it:

| Content | Payload per frame | Full frames/s |
|---|---|---|
| desktop photo (lossless) | 837 KB | ~52-55 |
| random noise (worst case) | 1843 KB | ~24 |
| solid colour | 27 KB | limited by latency, not bandwidth |

Transfer size 19968 / 64 KiB / 256 KiB: 40.1 / 41.2 / 41.6 MB/s. Typical
desktop use is mostly partial updates and much cheaper.

The chip also has a decode limit (usbmon timing of the Windows driver,
2026-10-12, two data points): a black full frame (27 KB compressed) took
10.04 ms, about 184 MB/s of decoded RGB; a 64x608 partial about 0.55 ms. So
the chip decodes roughly 60-70 Mpixel/s, and a full-screen update costs at
least ~10 ms however well it compresses. Raw `0x11` writes ran at 39.4-40 MB/s.

A lossy variant like the vendor's (merge values within +-4 into runs) halves a
photo's payload (+-2: -36%, +-4: -46%) but leaves visible banding in smooth
gradients, so the drivers stay lossless by default. mct_t2 has an optional
lossy mode (`lossy=N`) only for updates too big for one frame, refined to
lossless afterwards (see t2-driver.md). `tools/t2rle.py encode(..., tol=N)`
keeps it for experiments.

## Panel power (verified live 2026-09-24)

The vendor driver has no power control for this panel: `CT2RomSink`'s
display-power method (vtable `0x18001e358` slot `+0x18`) is `xor eax,eax; ret`.
What works: **`fcf6` = pixel-clock PLL enable** (single register write on EP3,
`02 00 01 fc f6 VV`).

- `fcf6 = 00`: video signal stops and the panel electronics switch the **backlight off**.
- `fcf6 = 01`: picture back immediately, framebuffer contents intact, no re-init.

Tried without effect: `fb60` (also via `0x14 60=00`), `fe36`, `fe57`, `fe70`, `feb0`.

## Backlight brightness (verified live 2026-09-24)

Brightness is not a T2 function. It belongs to the tablet's pen function
(`056a:032f`, a separate USB device on the same internal hub) and is HID
**feature report `0x06`**: `06 LL`, level `LL` = 0 (darkest) to 8 (brightest).
Wacom's driver only uses 1-7; 0 and 8 are real extra steps (checked visually),
values above 8 read back as 8.

- Found by capturing Wacom's Windows driver 6.4.9-2 (capture not included; parsed with
  `tools/redir_hid.py`) while moving "Tablet light brightness" in its Options
  dialog: SET_REPORT `0x0306` with `06 05`, `06 04`, ... `06 01`, ... `06 07`.
- It takes effect immediately and needs none of Wacom's other traffic: a plain
  SET_REPORT from Linux (hidraw or a control transfer) works while another
  program owns the pen.
- Power-on value is 7; Wacom's driver sets 6 when it starts.
- Level 0 is the darkest the firmware goes and is not "off"; use DPMS (`fcf6`) for off.

Other Wacom driver traffic seen at startup: feature `0x83` (`83 02 00`) x3,
`0x02` (`02 02`, mode), GET `0x0c` (serial), GET `0x10` (status), and a series of
`0x40` exchanges, some carrying random-looking 64-byte blocks: the pen data
encryption handshake (RSA-wrapped AES key, see
[wacom-display-settings.md](../research/wacom-display-settings.md)). None of it is needed
for brightness.

## I2C masters (verified live 2026-09-24, `tools/t2i2c.py`)

The T2 has two I2C masters with the OpenCores `i2c_master` register layout,
driven through normal register reads/writes (EP3 `0x05` / `0x02`):

| Bus | Prescale lo/hi | Control | Data (TXR/RXR) | Command/Status |
|---|---|---|---|---|
| 0 | `fce0`/`fce1` | `fce2` | `fce3` | `fce4` |
| 1 | `fce8`/`fce9` | `fcea` | `fceb` | `fcec` |

- Command bits: `0x80` START, `0x40` STOP, `0x20` READ, `0x10` WRITE, `0x08` NACK.
  Status bits: `0x80` no ACK, `0x40` bus busy, `0x02` transfer in progress.
  (Vendor `CT2I2CBus`: start = write `0x90` with the address byte, stop = `0x40`.)
- `0x1b` / `0x1d` on EP3 are block reads through bus 0 / bus 1:
  `1d 80 00 ae 00 00 01 00` reads 256 bytes from `0xae` (7-bit `0x57`) on bus 1,
  the ID ROM with the panel EDID at +0x80. `1b 80 00 a0 00 80 00 00` would read an
  EDID from bus 0 and returns zeros.
- Bus 1 (prescale `0x0080`, enabled) has a single device: the EEPROM at `0x57`.
- Bus 0 is left at reset values (prescale `0xffff`, disabled). Enabled with bus 1's
  settings, a START never completes (status `0x42`): nothing is wired there.
- So there is no DDC/CI path to the panel. Brightness on this tablet is handled by
  Wacom's driver over the pen's USB HID function (Wacom manual: "Brightness setting
  in the Wacom tablet control panel"), not by the display chip.

## Hardware cursor (verified live 2026-09-24, `tools/cursor_test.py`)

The T2 has a 64x64 ARGB cursor overlay, drawn by the chip on top of scanout.

**Shape upload** (EP `0x02`, found in `redir4.bin`: 39 uploads, 6 distinct shapes):

```
12 00 00 00 00 00 40 00 00 40 00 00 01 25      (14 B header, own transfer; sent verbatim)
+ 16384 B = 64x64 pixels, 4 B each: A, B, G, R  (DRM_FORMAT_RGBA8888)
```

- Stored at video address 0 (below framebuffer A at `0xC000`). The header fields
  presumably encode address 0 and length `0x4000`.
- The image is in **device orientation**, like the framebuffer. For the 180 degrees panel,
  reverse the pixel order: the arrow's tip ends up at the bottom-right of the stored box.
- Windows uses alpha `0xFE` for opaque pixels and `0x00` for transparent ones.

**Position / enable** (EP `0x04`, `05 <n> (reg val)...` register list, like the MC8032 write):

```
05 12  34 40 35 40  38 XL 39 XH  3a YL 3b YH  44 00 45 00  36 07   (first time: size 0x40 x 0x40)
05 0e  38 XL 39 XH  3a YL 3b YH  44 00 45 00  36 07                (every move)
05 02  36 04  /  05 02  36 24                                      (hide / transient during shape change)
```

- `0x38/0x39` = x, `0x3a/0x3b` = y: **device-space top-left of the 64x64 box**.
  (0,0) puts the whole box in the visible bottom-right corner.
- Coordinates must be >= 0. Negative (two's complement) values make the cursor vanish.
  The chip clips boxes that run past the far edges (x > 960 / y > 536).
- `0x44` / `0x45` shrink the drawn box from the **far** end (e.g. `0x44 = 32` ->
  only image columns 0-31 are drawn). This is not an image offset.
- `0x36`: `07` = cursor shown, `04` = hidden (verified by blinking). Windows
  writes `24` then `04` around some shape changes.
- To show a cursor hanging off the visible right/bottom edge (negative device
  coordinates), upload a copy shifted by the overhang, with transparent fill, and position it at 0.

## Tested on the tablet 2026-10-12 (`tools/t2_probe.py`)

Each one drawn in the hidden buffer next to a reference box drawn the known
way, then shown with a flip; the user saw the expected picture every time,
and the chip answered a register read right after every command (log in
the lab: `captures/t2_probe-20261012-142605.log`). Verified:

- **512 KiB payload transfers** (a 1,082,400 B full frame as 524288 +
  524288 + 33824 B), like Windows.
- **`0x11` raw RGB24**, payload B, G, R.
- **2x hardware upscale** with `0x11` (64x32 source, 128x64 target, scale
  `0x2000`, payload = source size) and with `0x13` (same, source = target
  address, raw length = source pixels x 3).
- **RGB565 through `0x11`** (format `04`, 2 bytes per pixel, little-endian
  5-6-5): colours as expected.
- **`0x13` decoding to a separate address** (`0x3a0000`), stretched 1:1 into
  the framebuffer.
- **Partial 2x upscale without touching the neighbours** (steps j and k,
  2026-10-12): a 64x32 source scaled into a 128x64 box inside a plain grey
  band, decoded in place (source = target) and from scratch VRAM: the band
  stayed clean in both cases.
- **YUV 4:2:2 RLE through `0x13`** (format `8a`, mode 6, Y/U/V planes of
  1536/768/768 per 3 KiB unit): flat colour bands as expected.

Still untested:

- the blit engine (`0x14`, or `0x15` register batches on `fc50-5d`, `fc34`,
  `fc59`, `fe60-89`, `fe70`), which could make scrolling a short command
  instead of resending the rows; the riskiest, since a wrong command may hang
  the chip until the tablet is power-cycled.

### The blit engine (code, 2026-10-12; lab `captures/t2-research/blit-engine.md`)

- `0x14` (48 B, one transfer): `14 dst32 dst32 srcW srcH dstW dstH scaleX
  scaleY fmtflag 90 srcfmt plane src32 plane2_32 plane3_32 dst32 srcW srcH
  pitchPx16 01`. The `0x15` form writes the same values as 51 register
  triplets: a reset (`fc59=01 fe70=80 fc59=00 fe70=00`), the scaler
  (`fc50-5d`, `fc34`, `fc59=90`) and the copier (`fe71` source format,
  `fe72` planes, `fe74-77` source, `fe60-67` extra planes, `fe80-83` target,
  `fe78-7b` source size, `fe88/89` target pitch in pixels, `fe70=01` start).
- It copies a **packed** `srcW x srcH` source to a target rectangle with a
  pixel pitch (1024 here), scaling and converting on the way. No source
  pitch, no fill mode. A full-width band of a framebuffer is packed, so a
  scroll (front buffer rows into the back buffer, shifted) fits; a window
  move would need one command per row unless the source honours a pitch.
- Plain RGB24 copy: scale `0x4000`, `fc34 = 00`, `fe71 = 05`, `fe72 = 00`.
- The older driver checks nothing before `0x14` and never waits for it; the
  `0x15` form polls `fe70` bit 2 first (probably a "done" bit: `fe70` reads 0
  at rest). It used the engine only from staging slots into the
  framebuffer, never framebuffer to framebuffer, never overlapping.
- **On this tablet the older driver would not use the engine by default**:
  it needs the ID ROM's byte `0x16` bit 0 (here `00`) and a non-default
  transfer method. So whether firmware `0x32` handles `0x14` at all is
  unknown. Startup resets seen: Windows 11 `fe70=80 fe57=a0 fe36=20`, 1 ms,
  `fe70=00 fe57=20 fe36=00`.

## Gaps (resolved by the usbredir capture, kept for history)

- `0x13` header tail (complen+magic, bytes 30-35), candidate: complen=rawlen,
  magic `30 05 28`; **verify live on tablet**.
- `0x22` tail (30/62 B) + `0x04` 43 B tail (11 B), skipped in first-light
  trial; reconstruct from driver if the panel needs them.
- RLE payload bytes, not needed if RGB-raw path works.
