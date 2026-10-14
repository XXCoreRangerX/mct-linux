# Captured Windows init sequence (DTU-1031X, Trigger2_Install, Win11 25H2)

> **Reference**: a capture from Windows (2026-09); it does not change.

Source: usbmon text capture (bus 6, dev 011, not published), first bulk at
line ~2629162. Order below = wire order. EP `0x03` unless noted.
The exact bytes (no truncation) are in `captures/redir2.bin` (development repository only); `tools/replay.py` replays them and `wacom-t2d/init-full.bin` is extracted from them (`wacom-t2d/init.bin` is the trimmed version).

## Phase 1: probe

1. `05 01 fc` -> `07` (reg fc01)
2. `91 00 0200` -> 512 B ID-ROM (`03 02 00 00...`)
3. `05 b0 fe` -> `43`; `05 b1 fe` -> `b4`
4. `02 0001 feb0 43`; `02 0001 feb1 03`; verify feb0=`43`
5. `02 0001 feb0 40`; `05 b1 fe` -> `b7`
6. `02 0001 fc6f 00`
7. `1d8000ae00000100` -> 512 B info+EDID
8. `02 0001 fc6f 00`
9. `90 040400 00061a80`; `90 030100 00`; `90 080100 02`
10. `02 0001 fee0 07`
11. `04 2800` + 40 B `(6a,idx)(6b,0x51...)` table program (tail truncated)
12. `04 1000` + 16 B `6a12 6b22 6a13 6b22 6a11 6b22 6a10 6b22`
13. `02 0001 fbff 81`
14. `0a` + 768 zeros
15. `02 0001 fc6f 00`; `05 a2 fc` -> `30`

## Phase 2: sink/mode registers

16. `05 a3 fc` -> `80`
17. `fcb0=20 fc4b=08 fbf2=02 fca2=30 fbf4=03 fcf0=01 fcf1=19 fcf2=00 fc4b=02`
18. `90 040400 00001000`
19. EP `0x02`: `11` tiny fill (32x1 at addr 0) + 96 zeros
20. `90 040400 00061a80`
21. `04 0a00` + `a439 a500 a600 a700 a36f` (regs a3-a7)
22. `fca3=6e`; `fb60=00 fb6e=01 fb70/71/72/74/75/76=00`;
    `fea8=a900, feaa=00, fe70=80, fe57=a0, fe36=20, fe70=00, fe57=20, fe36=00`
23. verify `feb0=40 feb1=b7`; `feb0=40 feb1=f7`
24. `fc28=01 fc59=01 fc32=00 fc34=00`
25. **`16` timing packet (35 B)**:
    `16 2000 2000210322c223012400250426582702280029032ac22b012c002d042e 582f02`
    (last 3 B reconstructed from driver; regs `0x20-0x2f`)
26. `fc2f=00 fbf6=00`; verify `fca3=6e`
27. `04 0c00` + `f320 f42d f601 f703 f500 4b03` (**PLL** fcf3-fcf7 + 0x4b)
28. `04 0c00` + `2801 2800 3200 5901 5900 3400`
29. **`22` 62 B packet** (tail truncated, skipped for now)
30. `04 1400` + `4000 4140 4200 4340 3600 3704 3858 3902 3400 3200`
31. `fb96=00 fcb0=09`
32. EP `0x04`: `05 0830 0031 0032 0033 00` (MC8032 XRAM)
33. `04 0c00` + `b411 b100 b230 b300 b415 b411` (+ `b422...`/`b238...` variants)
34. fb60-fb76 dance (`fb65`: ...01->15->11 / ...02->38->07->2a->22 ...), `fcb5=03`, ...
35. fea8/a9/aa, fe70/57/36 toggles (second pass)

## Phase 3: video (full bytes in `captures/redir2.bin` / `redir4.bin`)

36. EP `0x02`: `11` clear buffer A (`0xC000`) + `11` clear buffer B (`0x1CE000`),
    each header its own transfer + 1 843 200 zero bytes in 16 KiB chunks.
37. EP `0x03`: **`14` display enable** `141b0066016700680069006a586b026c006d0465106e0160016e01`
38. Loop: `13` frame -> back buffer, then `15` flip to it (see protocol doc).
    First frames are black; Windows keeps alternating B, A, B, ...

**Verified 2026-09-24:** verbatim replay of phases 1-3 (`tools/replay.py`,
each op as its own transfer), followed by our own RLE frames + flips -> solid
R, G, B, white shown on the panel.

## Repro notes

- No USB reset happens on passthrough attach: pre-existing reg values persist.
- After `05/91/1d` reads, the reply MUST be consumed before the next OUT.
- Firmware needs ~50 ms pacing between commands; ~10 s boot after power cycle.
- Fresh power-on reads `feb0=00 feb1=f5`; driver overwrites both.

## Trimmed init (78 transfers, ~1 s from cold), verified 2026-09-24

`wacom-t2d/init.bin` / `kernel/mct_t2_init.h` (made by `tools/gen_init_min.py` from
`wacom-t2d/init-full.bin`). Record numbers refer to the full 359-record sequence.

| Records | What | Notes |
|---|---|---|
| 4-5, 7 | `feb0=03 feb1=03`, `feb0=00` | block reset / release (**50 ms** after each) |
| 9-11 | `fc6f=00`, **`0x1d` device info** (+512 B read), `fc6f=00` | EDID at reply +0x80 |
| 12-14 | `0x90 04 04 00 00061a80`, `0x90 03 01 00 00`, `0x90 08 01 00 02` | pipe setup |
| 15-18 | `fee0=07`, two `0x04` lists (`6a/6b` index/value tables), `fbff=81` | `fbff=81` resets bank FB (see 75-90) |
| 19-20 | `0x0a` + 768 zeros, `fc6f=00` | 256x3 table (gamma/palette?) |
| 23-31 | `fcb0=20 fc4b=08 fbf2=02 fca2=30 fbf4=03 fcf0/1/2 fc4b=02` | |
| 35-37 | `0x90 04 04 ...61a80`, `0x04` list (`a4..a7,a3`), `fca3=6e` | |
| 38-54 | `fb60/6e`, `fb70-76=0`, `fea8-aa=0`, `fe70/fe57/fe36` pulses | |
| 57-62 | `feb0=40 feb1=f7`, `fc28 fc59 fc32 fc34` | |
| 63 | **`0x16` timing** (regs `0x20-0x2f`) | |
| 64-65 | `fc2f=00 fbf6=00` | |
| 67 | **PLL** `0x04`: `f3=20 f4=2d f6=01 f7=03 f5=00 4b=03` | `fcf6` = pixel-clock enable (panel power, below) |
| 68 | `0x04`: `28 01 28 00 32 00 59 01 59 00 34 00` | |
| 69 | **`0x22` mode** (bank FE/FC triplets) | |
| 70-72 | `0x04` list (`40..43, 36..39, 34, 32`), `fb96=00 fcb0=09` | |
| 73 | EP4 `05 08 30 00 31 00 32 00 33 00` | MC8032 XRAM |
| 75-90 | bank-FB buffer addresses: `fb60/6e/65`, `fb62..64` = A (`00 30 00`) and B (`00 38 07`) | **required**; without it a garbage band shows at the bottom |
| 96 | `fcb5=03` | |
| - | black `0x13` frame into buffer A | not from the capture: replaces the clears |
| 358 | **`0x14` display enable** (`66..6e`, `65=10` -> scanout **A**, `60=01`) | |

Dropped from the vendor sequence: register reads (unused values), `0x91`
ID-ROM query, a `0x90`+32x1 probe fill at address 0, manual bank-FC scanout
programming (our `0x15` flips do that), an exact repeat of 38-54, and the two
1.8 MB black clears.

**Pacing (8051):** 2 ms after single register writes, 50 ms after `feb0`/`feb1`
writes, 20 ms after other EP3/EP4 commands, none on EP2. With **no** pacing a
cold chip comes up misconfigured (monochrome tiled image). Warm chips tolerate
anything, so pacing changes must be tested cold (physical replug; toggling the
hub port's `disable` does *not* reset the 8051).

Bisection method (`tools/init_trim.py`): drop one group at a time from the
known-good set, show a distinct colour, reset with the full init in between.
The final set was validated cold three times (zero, 20 ms, selective pacing).
