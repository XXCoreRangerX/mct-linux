# Captured Windows init sequence (DTU-1031X, Trigger2_Install, Win11 25H2)

Source: usbmon text capture (bus 6, dev 011, not published), first bulk at
line ~2629162. Order below = wire order. EP `0x03` unless noted.
The exact bytes (no truncation) are in `captures/redir2.bin`; `tools/replay.py` replays them and `t2d/init.bin` is extracted from them.

## Phase 1 — probe

1. `05 01 fc` → `07` (reg fc01)
2. `91 00 0200` → 512 B ID-ROM (`03 02 00 00…`)
3. `05 b0 fe` → `43`; `05 b1 fe` → `b4`
4. `02 0001 feb0 43`; `02 0001 feb1 03`; verify feb0=`43`
5. `02 0001 feb0 40`; `05 b1 fe` → `b7`
6. `02 0001 fc6f 00`
7. `1d8000ae00000100` → 512 B info+EDID
8. `02 0001 fc6f 00`
9. `90 040400 00061a80`; `90 030100 00`; `90 080100 02`
10. `02 0001 fee0 07`
11. `04 2800` + 40 B `(6a,idx)(6b,0x51…)` table program (tail truncated)
12. `04 1000` + 16 B `6a12 6b22 6a13 6b22 6a11 6b22 6a10 6b22`
13. `02 0001 fbff 81`
14. `0a` + 768 zeros
15. `02 0001 fc6f 00`; `05 a2 fc` → `30`

## Phase 2 — sink/mode registers

16. `05 a3 fc` → `80`
17. `fcb0=20 fc4b=08 fbf2=02 fca2=30 fbf4=03 fcf0=01 fcf1=19 fcf2=00 fc4b=02`
18. `90 040400 00001000`
19. EP `0x02`: `11` tiny fill (32×1 at addr 0) + 96 zeros
20. `90 040400 00061a80`
21. `04 0a00` + `a439 a500 a600 a700 a36f` (regs a3–a7)
22. `fca3=6e`; `fb60=00 fb6e=01 fb70/71/72/74/75/76=00`;
    `fea8=a900, feaa=00, fe70=80, fe57=a0, fe36=20, fe70=00, fe57=20, fe36=00`
23. verify `feb0=40 feb1=b7`; `feb0=40 feb1=f7`
24. `fc28=01 fc59=01 fc32=00 fc34=00`
25. **`16` timing packet (35 B)**:
    `16 2000 2000210322c223012400250426582702280029032ac22b012c002d042e 582f02`
    (last 3 B reconstructed from driver; regs `0x20–0x2f`)
26. `fc2f=00 fbf6=00`; verify `fca3=6e`
27. `04 0c00` + `f320 f42d f601 f703 f500 4b03` (**PLL** fcf3–fcf7 + 0x4b)
28. `04 0c00` + `2801 2800 3200 5901 5900 3400`
29. **`22` 62 B packet** (tail truncated, skipped for now)
30. `04 1400` + `4000 4140 4200 4340 3600 3704 3858 3902 3400 3200`
31. `fb96=00 fcb0=09`
32. EP `0x04`: `05 0830 0031 0032 0033 00` (MC8032 XRAM)
33. `04 0c00` + `b411 b100 b230 b300 b415 b411` (+ `b422…`/`b238…` variants)
34. fb60–fb76 dance (`fb65`: …01→15→11 / …02→38→07→2a→22 …), `fcb5=03`, …
35. fea8/a9/aa, fe70/57/36 toggles (second pass)

## Phase 3 — video (full bytes in `captures/redir2.bin` / `redir4.bin`)

36. EP `0x02`: `11` clear buffer A (`0xC000`) + `11` clear buffer B (`0x1CE000`),
    each header its own transfer + 1 843 200 zero bytes in 16 KiB chunks.
37. EP `0x03`: **`14` display enable** `141b0066016700680069006a586b026c006d0465106e0160016e01`
38. Loop: `13` frame → back buffer, then `15` flip to it (see protocol doc).
    First frames are black; Windows keeps alternating B, A, B, …

**Verified 2026-09-24:** verbatim replay of phases 1–3 (`tools/replay.py`,
each op as its own transfer), followed by our own RLE frames + flips → solid
R, G, B, white shown on the panel.

## Repro notes

- No USB reset happens on passthrough attach: pre-existing reg values persist.
- After `05/91/1d` reads, the reply MUST be consumed before the next OUT.
- Firmware needs ~50 ms pacing between commands; ~10 s boot after power cycle.
- Fresh power-on reads `feb0=00 feb1=f5`; driver overwrites both.
