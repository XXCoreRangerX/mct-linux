# Trigger II (T2) bulk protocol

Source: cyrozap's [Protocol-T2.md](https://github.com/cyrozap/mct-usb-display-adapter-re/blob/master/doc/Protocol-T2.md)
(CC BY-SA 4.0) + live probing + `T2Indisp.dll` disassembly
+ Win11 VM usbmon capture. All multi-byte fields little-endian unless noted.

Commands go to EP `0x03` (control-ish) or `0x02` (framebuffer); replies on
EP `0x81`. EP `0x04` OUT: `0x05` var-len list (MC8032 10 B, see init doc).
EP `0x85/0x06`: unused. **OUT/IN are paired**: after a command with a reply,
read `0x81` before the next OUT or the firmware stalls. Pace ~50 ms.

## EP `0x03` commands

| Cmd | Sent | Reply (`0x81`) | Meaning |
|---|---|---|---|
| `0x02` | `02 0001 <addr16> <val>` (6 B) | none | write 1 B register |
| `0x05` | `05 <addr_lo> <addr_hi>` (3 B) | 1 B | read 1 B register |
| `0x04` | `04 <len16> <payload>` | none | var-len list; observed: `(6a,idx)(6b,val)` and `(a3..a7/f3..f7/b1..b5, val)` register-pair programs |
| `0x16` | `16 <len16=0x20> <32 B pairs>` (35 B) | none | **timing packet**: regs `0x20-0x2f` = width/height-derived + mode params (full template in init doc) |
| `0x22` | 62 B | none | second mode packet (regs incl. `0xfc`-area mirrors); template TBD from driver |
| `0x0a` | `0a` + 768 zeros (769 B) | none | clear/reset |
| `0x1b` | `1b8000a000800000` (8 B) | 512 zeros (idle) | EDID query (zeros until inited?) |
| `0x1d` | `1d8000ae00000100` (8 B) | 512 B | **device info**: `55aa00ff...`, `07115200`, `MCT`, `DTU-1031X`, **EDID at +0x80** |
| `0x90` | `90 040400 00061a80` (8 B), `90 030100 00` / `90 080100 02` (5 B) | none | mode/pipe setup variants |
| `0x91` | `91 <arg1> <arg16>` (4 B) | 512 B | ID-ROM query; driver uses `91 00 0200` -> reply starts `03 02 00 00...` |

## EP `0x02` framebuffer

### `0x11` color fill (21 B header + `w*h*3` RGB bytes)

```
11 <addr32> <w16> <h16> <w16> <h16> <X16> <Y16> <flag8> <len24> + RGB payload
```

- Fullscreen clear (captured): `11 00c00000 0400 0258 0400 0258 0400 0400 00 00201c`
  - addr `0x0000c000` (FB base from `SetFBAddress`), `1024 600 1024 600`,
    X=Y=`0x0400`, flag `0x00`, len `0x1C2000` = 1,843,200 = `1024*600*3`.
  - Tiny fill (captured): addr 0, `32 1 32 1`, X=0 Y=`0x40`, flag `0x40`,
    then 96 zero bytes (payload length implicit `w*h*3`).
- NOTE: cyrozap documents the two word fields as `0x1000`; the driver sends
  `0x4000` (bytes `00 40`) and the live clear has `0400`. Fills with `0x1000`
  were ignored; **use captured-exact headers**.
- Our red fill = captured clear header + `FF0000`x614400 (BGR order TBD:
  if the screen shows blue, swap to `0000FF`).

### Transfer framing (important: this caused the "lit but blank" screen)

The firmware parses EP `0x02` **per USB transfer, not as a byte stream**:

- Every command header (`0x11` 21 B, `0x13` 36 B, `0x15` 23 B) is sent as its
  **own bulk transfer** (a short packet ends it).
- The payload follows in **separate** transfers. Windows chunks `0x13`
  payloads at 19968 B and `0x11` payloads at 16384 B (size probably free).
- Header+payload merged into one write is **silently ignored** (no stall,
  nothing drawn). Replays must keep the captured transfer boundaries.

Payload transfer size: keep payload transfers at the vendor's 19968 bytes.
A whole 837 KB frame sent as one transfer showed sparse corrupted pixels near
the end of the frame (2026-09-24); the same frame in 19968-byte chunks was
clean. Larger chunks gain only ~3% anyway (see throughput below).

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

### Framebuffers

| Buffer | Byte addr (`0x11`) | `0x13` addr field | flip `SEL` |
|---|---|---|---|
| A | `0x0000C000` | `0x00C00000` (`00 00 c0 00`) | `0x10` |
| B | `0x001CE000` = A + `1024*600*3` | `0x1CE00000` | `0x20` |

The `0x13` address field is `byte address << 8`. Partial updates point it at
the sub-rectangle start inside the buffer (`+3*x` for x offset; rows via
stride 3072), e.g. `0x00CB4000` = buffer A + x 960.

### `0x13` frame update (36 B header + payload)

```
13 <addr32> <addr32> 00 <w16> <h16> <w16> <h16> 00 <tile16=64> <tile16=64>
   00 <unk32> <rawlen24> <complen24> 30 05 <flags> + payload
```

- Fullscreen RGB (captured): addr `0x1CE00000`x2, `1024 600 1024 600`,
  tiles `64 64`, unk `0x39000000`, rawlen `0x1C2000`, complen+magic cut off
  in capture (see below).
- Partial updates use other addrs (e.g. `0x00CB4000`) + smaller w/h.
- Payload is **RGB-raw** (`EncodeRgbDTR`: driver packs RGBX->RGB24) or
  **RLE** (`EncodeRleDTR`, 64-px units, "3K alignment"); selected by flags
  byte (`|8` in driver). Windows always sent RLE (`30 05 28`) in all captures.
- Exact header (verified live, full screen): `13` + `addr<<8` (4 B) x2 + `00`
  + `0004 5802 0004 5802` (w h w h) + `0040 0040` + `00 00003900`
  + rawlen24 (`0x1C2000`) + complen24 (= payload length) + `30 05 28`.

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
  value into a run. Our encoder is exact (apart from `0x30`->`0x31`).
- **Orientation: the panel scans out rotated 180 degrees.** The first pixel in the
  stream is the bottom-right of the visible image. Windows pre-rotates, and we
  reverse the pixel order (`tools/show_image.py::rot180`).
- Sizes: solid colour 27 000 B; photo wallpaper ~ 837 KB; worst case ~ 1.84 MB
  (all literals). Full-frame send + flip ~ 0.2 s from Python.

### Partial updates (verified live via t2d)

- Same 36 B `0x13` header with the rect's `w h w h`; address fields =
  `(buffer + y*3072 + x*3) << 8` (device coordinates, i.e. after 180 degrees rotation).
- Payload: the rect's pixels in **row-major order, cut into 1024-px units**; each
  unit is encoded as R, G, B channel lines (a full-width row is simply one unit).
  Hence `w*h % 1024 == 0`. Windows keeps `x`, `w` multiples of 64 and rounds
  `h` up (it even overshoots row 600, e.g. `64x608`).
- Windows policy: big change -> full frame into back buffer + `0x15` flip.
  Small change -> partials straight into the **front** buffer, no flip.
  All 172 frames in `captures/redir4.bin` decode under this model.

## Throughput (measured 2026-09-24, `tools/bench.py`)

USB 2.0 bulk tops out around 42-44 MB/s here, and full-frame updates reach it:

| Content | Payload per frame | Full frames/s |
|---|---|---|
| desktop photo (lossless) | 837 KB | ~52-55 |
| random noise (worst case) | 1843 KB | ~24 |
| solid colour | 27 KB | limited by latency, not bandwidth |

Transfer size 19968 / 64 KiB / 256 KiB: 40.1 / 41.2 / 41.6 MB/s. Typical
desktop use is mostly partial updates and much cheaper.

A lossy variant like the vendor's (merge values within +-4 into runs) halves a
photo's payload (+-2: -36%, +-4: -46%) but leaves visible banding in smooth
gradients, so the drivers stay lossless. `tools/t2rle.py encode(..., tol=N)`
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
`0x40` exchanges, some carrying random-looking 64-byte blocks (probably a
challenge/response the driver uses to identify genuine hardware). None of it is
needed for brightness.

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

## Gaps (resolved by the usbredir capture, kept for history)

- `0x13` header tail (complen+magic, bytes 30-35), candidate: complen=rawlen,
  magic `30 05 28`; **verify live on tablet**.
- `0x22` tail (30/62 B) + `0x04` 43 B tail (11 B), skipped in first-light
  trial; reconstruct from driver if the panel needs them.
- RLE payload bytes, not needed if RGB-raw path works.
