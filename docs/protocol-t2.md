# Trigger II (T2) bulk protocol

Source: cyrozap `Protocol-T2.md` + live probing + `T2Indisp.dll` disassembly
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
| `0x16` | `16 <len16=0x20> <32 B pairs>` (35 B) | none | **timing packet**: regs `0x20–0x2f` = width/height-derived + mode params (full template in init doc) |
| `0x22` | 62 B | none | second mode packet (regs incl. `0xfc`-area mirrors); template TBD from driver |
| `0x0a` | `0a` + 768 zeros (769 B) | none | clear/reset |
| `0x1b` | `1b8000a000800000` (8 B) | 512 zeros (idle) | EDID query (zeros until inited?) |
| `0x1d` | `1d8000ae00000100` (8 B) | 512 B | **device info**: `55aa00ff…`, `07115200`, `MCT`, `DTU-1031X`, **EDID at +0x80** |
| `0x90` | `90 040400 00061a80` (8 B), `90 030100 00` / `90 080100 02` (5 B) | none | mode/pipe setup variants |
| `0x91` | `91 <arg1> <arg16>` (4 B) | 512 B | ID-ROM query; driver uses `91 00 0200` → reply starts `03 02 00 00…` |

## EP `0x02` framebuffer

### `0x11` color fill (21 B header + `w·h·3` RGB bytes)

```
11 <addr32> <w16> <h16> <w16> <h16> <X16> <Y16> <flag8> <len24> + RGB payload
```

- Fullscreen clear (captured): `11 00c00000 0400 0258 0400 0258 0400 0400 00 00201c`
  - addr `0x0000c000` (FB base from `SetFBAddress`), `1024 600 1024 600`,
    X=Y=`0x0400`, flag `0x00`, len `0x1C2000` = 1,843,200 = `1024·600·3`.
  - Tiny fill (captured): addr 0, `32 1 32 1`, X=0 Y=`0x40`, flag `0x40`,
    then 96 zero bytes (payload length implicit `w·h·3`).
- NOTE: cyrozap documents the two word fields as `0x1000`; the driver sends
  `0x4000` (bytes `00 40`) and the live clear has `0400`. Fills with `0x1000`
  were ignored — **use captured-exact headers**.
- Our red fill = captured clear header + `FF0000`×614400 (BGR order TBD:
  if the screen shows blue, swap to `0000FF`).

### Transfer framing (CRITICAL — cause of weeks of "lit but blank")

The firmware parses EP `0x02` **per USB transfer, not as a byte stream**:

- Every command header (`0x11` 21 B, `0x13` 36 B, `0x15` 23 B) is sent as its
  **own bulk transfer** (a short packet ends it).
- The payload follows in **separate** transfers. Windows chunks `0x13`
  payloads at 19968 B and `0x11` payloads at 16384 B (size probably free).
- Header+payload merged into one write is **silently ignored** (no stall,
  nothing drawn). Replays must keep the captured transfer boundaries.

### `0x15` register batch / buffer flip (EP `0x02`, 23 B)

```
15 <n=07> (bank reg val) × 7
15 07 fb 65 SEL  fc b4 11  fc b1 A0  fc b2 A1  fc b3 A2  fc b4 15  fc b4 11
```

- `(bank, reg, val)` triplets written in order through the framebuffer pipe
  (so they stay ordered w.r.t. frames).
- `A2:A1:A0` = scanout **byte address >> 2**; `SEL` = `0x10` for buffer A
  (`0x00C000` → `00 30 00`), `0x20` for buffer B (`0x1CE000` → `00 38 07`).
- `fcb4` 11→15→11 latches the new address.
- Windows double-buffers: `0x13` into the back buffer → `0x15` flip to it →
  next frame into the other buffer. **Without a flip nothing new is shown.**
- Same sequence also exists as EP3 register writes / `0x04` list during init.

### Framebuffers

| Buffer | Byte addr (`0x11`) | `0x13` addr field | flip `SEL` |
|---|---|---|---|
| A | `0x0000C000` | `0x00C00000` (`00 00 c0 00`) | `0x10` |
| B | `0x001CE000` = A + `1024·600·3` | `0x1CE00000` | `0x20` |

The `0x13` address field is `byte address << 8`. Partial updates point it at
the sub-rectangle start inside the buffer (`+3·x` for x offset; rows via
stride 3072), e.g. `0x00CB4000` = buffer A + x 960.

### `0x13` frame update (36 B header + payload)

```
13 <addr32> <addr32> 00 <w16> <h16> <w16> <h16> 00 <tile16=64> <tile16=64>
   00 <unk32> <rawlen24> <complen24> 30 05 <flags> + payload
```

- Fullscreen RGB (captured): addr `0x1CE00000`×2, `1024 600 1024 600`,
  tiles `64 64`, unk `0x39000000`, rawlen `0x1C2000`, complen+magic cut off
  in capture (see below).
- Partial updates use other addrs (e.g. `0x00CB4000`) + smaller w/h.
- Payload is **RGB-raw** (`EncodeRgbDTR`: driver packs RGBX→RGB24) or
  **RLE** (`EncodeRleDTR`, 64-px units, "3K alignment"); selected by flags
  byte (`|8` in driver). Windows always sent RLE (`30 05 28`) in all captures.
- Exact header (verified live, full screen): `13` + `addr<<8` (4 B) ×2 + `00`
  + `0004 5802 0004 5802` (w h w h) + `0040 0040` + `00 00003900`
  + rawlen24 (`0x1C2000`) + complen24 (= payload length) + `30 05 28`.

### RLE payload (fully decoded 2026-09-24)

Source: `T2Indisp.dll` encoder @ `0x180013278` (called from `EncodeRleDTR`
@ `0x180011fe0`); verified by decoding a real Windows desktop frame and
re-encoding it **byte-identically** (`tools/t2rle.py`).

- **Planar per row**: for each row, three channel-lines **R, G, B**, each
  covering all `w` pixels (the encoder works in 1024-px units).
- Channel-line byte stream:
  - any byte `!= 0x30` → **one literal pixel** of that value
  - `30 NN VV` → **run** of `NN+1` pixels of value `VV`
- `0x30` is the escape, so pixel value `0x30` is never emitted as a literal;
  Windows maps it to `0x31` (invisible). Windows caps runs at `NN = 0xFA`
  (251 px) and never emits `NN = 0x30` (writes `30 2f VV VV` instead). Runs of
  1–2 px are written as literals.
- Windows' encoder is slightly lossy: it merges pixels within ±4 of the run
  value into a run. Our encoder is exact (apart from `0x30`→`0x31`).
- **Orientation: the panel scans out rotated 180°.** The first pixel in the
  stream is the bottom-right of the visible image. Windows pre-rotates, and we
  reverse the pixel order (`tools/show_image.py::rot180`).
- Sizes: solid colour 27 000 B; photo wallpaper ≈ 837 KB; worst case ≈ 1.84 MB
  (all literals). Full-frame send + flip ≈ 0.2 s from Python.

### Partial updates (verified live via t2d)

- Same 36 B `0x13` header with the rect's `w h w h`; address fields =
  `(buffer + y·3072 + x·3) << 8` (device coordinates, i.e. after 180° rotation).
- Payload: the rect's pixels in **row-major order, cut into 1024-px units**; each
  unit is encoded as R, G, B channel lines (a full-width row is simply one unit).
  Hence `w·h % 1024 == 0`. Windows keeps `x`, `w` multiples of 64 and rounds
  `h` up (it even overshoots row 600, e.g. `64×608`).
- Windows policy: big change → full frame into back buffer + `0x15` flip.
  Small change → partials straight into the **front** buffer, no flip.
  All 172 frames in `captures/redir4.bin` decode under this model.

## EP `0x04` hardware cursor (from `redir4.bin`)

`05 0e  38 XL 39 XH 3a YL 3b YH 44 00 45 00 36 07` — cursor position regs
`0x38/0x39` = x, `0x3a/0x3b` = y (sent at mouse-move rate). `05 02 36 24` /
`05 02 36 04` toggle cursor enable. Optional for a Linux driver.

## Gaps (resolved by usbredir capture — kept for history)

- `0x13` header tail (complen+magic, bytes 30–35) — candidate: complen=rawlen,
  magic `30 05 28`; **verify live on tablet**.
- `0x22` tail (30/62 B) + `0x04` 43 B tail (11 B) — skipped in first-light
  trial; reconstruct from driver if the panel needs them.
- RLE payload bytes — not needed if RGB-raw path works.
