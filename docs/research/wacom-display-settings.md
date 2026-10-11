# Wacom DTU-1031X: display settings in Wacom's driver (2026-10-11)

> **Research snapshot** (2026-10-11): static analysis, not updated afterwards.

What Wacom's Windows driver 6.4.9-2 offers for the DTU-1031X's display, and
how each setting reaches the device. Static research only: the installer
(`WacomTablet_6.4.9-2.exe`, unpacked with 7z), the earlier VM captures and
the user manual. No device traffic.

## Result

The control panel has **one** display setting for this model: "Tablet Light
Brightness", a slider in the Options dialog. Nothing else: no contrast,
colour temperature, gamma, sharpness, auto adjust, phase, clock, position
or OSD control. The manual agrees.

So `mct_t2` already covers what Wacom's control panel can do for the display.

## Brightness

| | |
|---|---|
| Transfer | SET_REPORT (`21 09`), wValue `0x0306`, 2 bytes `06 LL`, to the pen `056a:032f`, interface 0 |
| Levels | 0..9 accepted by the driver code; 0 only with the preference `DTU1031AllowDisplayBrightnessOff` (off by default, then 1 is the minimum). The firmware clamps above 8. |
| Default | Level 6 is applied when nothing is stored (power-on value is 7) |
| UI | Slider labels "Off", "Dim", "Brightest"; above level 6 a dialog "Additional Power Required" asks for both USB plugs |

Code: `Wacom_Tablet.exe`, class `CDTUEncryptedGraphicsTablet`, `ProcessEvent`
at VA `0x14066c9c0` (parameters `0x125` DisplayBrightness, `0x29d`
IsDisplayBrightnessAboveDefault), setter at `0x14066cfa0` (1 s timeout).
The captures hold 54 such SET_REPORTs: `06 06` after startup, then the
slider sweep.

`mct_t2` maps backlight 1..9 to report levels 0..8 and 0 to panel off.
Possible tweak: start at level 6 like Wacom (less power draw), and note in
the docs that levels above 6 want both USB plugs.

## Not device commands

- **Calibration** is software (4-point alignment). The "Phase Calibration"
  dialog in the UI database is EMR pen frequency calibration for Intuos
  tablets, not VGA sampling phase.
- **Display toggle, orientation, monitor mapping** use Windows display APIs.
- **Power saving** is left to the computer. The "no signal, go to power
  save" message in the manual is the RTD2280 scaler's own OSD text.

## The full display tool doesn't cover this model

`LCDSettings.exe` (contrast, colour presets 5000-10000 K, gamma,
sharpness, dynamic contrast, LED brightness, auto power down) talks MCCS
over DDC/CI tunnelled in HID feature reports (`CVcpHIDConnector`) or USB
I2C bridges. Its built-in capability strings are only for the DTH-W1320 /
W1620 (Cintiq Pro / MobileStudio Pro class); the DTU-1031X class doesn't
answer `SupportsDisplaySettings` and its UI has no such entry.

## Auto adjust and phase (the rainbow lines)

Nothing in the package sends them to a DTU-1031X. The RTD2280L understands
DDC/CI auto setup (VCP `0x1E`), clock (`0x0E`) and phase (`0x3E`), but the
T2's I2C bus 0, the would-be DDC line, is not wired (verified earlier). The
only path left is the pen MCU, if one of its unknown feature reports
reaches the scaler.

## Pen feature reports

| Report | Wacom's driver | Meaning |
|---|---|---|
| `0x02` | SET `02 02` | device mode |
| `0x03` | GET, `03 00` | unknown (maybe the virtual COM port mode flag of STU compatibility, low confidence) |
| `0x06` | SET `06 LL` | brightness |
| `0x0c` | GET | serial number |
| `0x10` | GET | status (encryption state) |
| `0x40` | SET/GET, many | **pen data encryption handshake**, not firmware update |
| `0x83` | SET `83 02 00`, stalls | generic Wacom mode switch probe, fails here |
| `0x04 05 07 08 09 0a 0b 0d b0 cc` | never used | unknown |

`0x40` (from `CDTU1031EncryptionManager.cpp`): drop old keys (commands 7,
8), set the encryption type, send an RSA-2048 public key (exponent 65537,
256-byte modulus), have the pen generate an AES key and read it back
wrapped, start encryption (command 6); the pen then sends encrypted reports
`0x12` / `0x13`. Writing it could switch pen input to encrypted reports
until a replug, so it still stays untouched.

## Next experiments (each needs an OK first)

1. No risk: look for a pen or scaler firmware image or OSD tool in older
   Wacom drivers (6.3.x) or the DTU-1031X CD (6.4.9-2 downloads updates
   online, no image inside).
2. Low risk, read only: GET_REPORT of `0x03 04 05 07 08 09 0a 0b 0d b0 cc`
   at two brightness levels, and in a rainbow-lines state against a clean
   one, to see if any byte mirrors the scaler.
3. Only if 2 finds a candidate: write back the value just read, one report
   at a time, with someone watching. Never `0x02`, `0x40`, `0x0c`; leave
   `0xb0` / `0xcc` alone until known.

Another Windows capture won't help: the driver never sends anything else.

## Sources

- DTU-1031X/1141 user's manual: https://cdn.wacom.com/u/marketplace/INK-SDK/faqs/tablet/1031X_1141_UsersManual.pdf
- DTU-1031X data sheet: https://documents.cdn.ifixit.com/cYPLspRSspiWE5YP.pdf
- RTD2280L summary: https://www.datasheetcafe.com/rtd2280l-datasheet-pdf-controller/
- MCCS codes 0x1E / 0x0E / 0x3E: https://www.hattelandtechnology.com/hubfs/pdf/misc/doc101681-1_8_and_13inch_dis_ddc_control.pdf
