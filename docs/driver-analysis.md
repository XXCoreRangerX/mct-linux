# Driver analysis (Trigger Family v25.01.1223, MCT official)

Download: `https://www.mct.com.tw/include/download.php?dl=<token>` with
`window.open('include/download.php?dl='+path)` from `go_download()` on the
download page. Row 01 (USB Display Adapters/Win) = id `150`
(token base64 `…MDAwMDAxNTA=`). ZIP → `TriggerFamily-Dongle-25.01.1223.0179.exe`
(InstallShield; files carved via manifest sizes + `unshield x data1.hdr`).

## Win10 IDD (Indirect Display, UMDF) — the active stack

`Win10_IDD/{x64,arm64,x86}/`: `TrgIdd64.inf`, `TrgIdd.dll` (UMDF USB +
DXGI/DesktopDuplication frame grab), `T2Indisp.dll` / `T5Indisp.dll` /
`T6InDisp.dll` / `T1pIdd.dll` / `T7UsbDisp.dll` (per-chip framing+codec),
`t6sta.sys`. Only 3 files per T2 install: INF + 2 DLLs
(`UmdfService=TrgIdd`, `TriggerDriverName=t2indisp.dll`, UMDF 2.15, IddCx0102).

Legacy groups also present: `WinXP_Target_T2_Disp` (t2exgrp/t2mrgrp),
`T2-5_XDDM_*_UtilDLL` (`MTrigger2.exe` tray UI — display settings only,
no USB), `WinXP_Target_SiS` (sisusb-era, unrelated).

## T2Indisp.dll internals (from `t2umdf!` debug strings, `data/t2umdf_strings.txt`)

- `CT2Hal`: `ProbeUsb` (WDF USB), `ProbeFirmware` (HW version gate),
  `PowerUp` (DDR, DVO driving, display power), `Read/WritePipe` (bulk EPs),
  `Read/WriteMC8032P1` (on-chip 8051!).
- `CT2VideoControl`: `EncodeRgbDTR` (RGBX→RGB24 pack), `EncodeRleDTR`
  (RLE, 64-px units, "3K alignment"), `DrawColorBlock` (`0x11` fills),
  `TransferQJob` (`dir/indir/stretch` DTR records), enc/xfr threads.
- `CT2VideoTimingCluster::CalculatePixelClockPLL` → regs `fcf3–fcf7`.
- `CT2Display::SetFBAddress/SetOVAddress` (FB base `0xc000` class),
  `StartVideo` (timing lookup by EDID `%dx%dx%d`).
- `CT2Edid` full EDID parser; `CT2IdRom::ParseRom` (ID-ROM checksum);
  `CT2I2CBus` + `CT2HdmiSink` (TPI) + `CT2VgaSink`; `CT2RomSink`.
- DTR (transfer record): `[0x00..0x17]` job header, USB command at `+0x18`.
- `0x13` header builder @ `0x180011fe0` (1822 B); `0x11` builders @
  `0x1800113b0`/`0x180011b30`; `0x16` builder (35 B, regs `0x20–0x2f`);
  `0x22` builder @ `0x18000a920` (1081 B, object-state dependent);
  `0x91` builder @ `0x18000b290` (`91 AA BBCC`, ≤256); `0x90` @ `0x18000ce90`.
- Magic `30 05 28`: bytes `0x30,0x05` fixed + flags (`|8`).

RE tools: `tools/xref.py` (capstone .pdata+xref map), `tools/vft.py`
(vftable scan — inconclusive, classes built `/GR-`), `tools/locfunc.py`,
`tools/funcs.asm` (excerpts). 396 functions, x64, no WDF static imports
(UMDF binds at runtime).
