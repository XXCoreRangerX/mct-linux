# Other drivers for MCT Trigger II (2026-10-12)

> **Research snapshot** (2026-10-12/13): the other Linux driver for the Trigger II, the vendor drivers read for interoperability facts, and the products known to carry the chip, compared with mct 0.6.0 (mct_t2 0.3.36). Later facts will be marked Added or EDIT with their date. The Trigger 6 drivers are in [t6-other-drivers.md](t6-other-drivers.md).

Written on 2026-10-14 from the lab notes of 2026-10-12 and 2026-10-13 (web
searches and static reading of the downloaded drivers; no device was
touched for this).

## Linux drivers for the Trigger II

| Project | What it is | Licence | State |
|---|---|---|---|
| [rhgndf/trigger2](https://github.com/rhgndf/trigger2) | a DRM driver for the Trigger II in j5create JUA230 and JUA230U (DVI) dongles, by the author of the drm/trigger5 patch | GPL-2.0-only | published 2026-09-25, last change 2026-10-08; tested through an HDMI capture card |

No other Linux driver for the Trigger II was found:

- MCT's own Linux driver (triggerdm) and its Ubuntu daemon are for the
  Trigger 6 only.
- The kernel's `sisusbvga` lists some `0711` IDs but rejects Trigger II
  devices (cyrozap's notes).
- MAX's `xserver-xorg-video-tusb` is for the older Trigger 1+
  (`0711:5100`).

### mct_t2 and trigger2

| | mct_t2 0.3.36 | trigger2 (2026-10-08) |
|---|---|---|
| Tested hardware | Wacom DTU-1031X (a fixed 1024x600 panel) | j5create JUA230 / JUA230U (a DVI port) |
| USB IDs | `0711:5209`, `520d`; `5201`, `520f` only after the panel's EDID shows the Wacom panel | `0711:5200-520f`, `5300-530f`, `5400-540f`, `5500-550f` |
| Modes | the panel's one mode; the init programs its timing | any mode within measured limits: width a multiple of 16 and at most 2032, height at most 2032, pixel clock under 200 MHz. Other sizes hang the device until it is replugged |
| Updates | two buffers: each update goes into the hidden one, then a flip (tear-free); only rows that changed are sent | one buffer, written while it is scanned out; no flip |
| Picture data | planar RLE, runs of 1-2 values sent as literals | planar RLE, runs of 1-2 values sent as literals ("the device mis-decodes runs shorter than three at some offsets"); Windows does the same |
| Hardware cursor | yes, 64x64 | no |
| Scaling, other source formats | 2x upscale for the experimental half-resolution motion mode | no |
| Licence | GPL-2.0-only | GPL-2.0-only |

mct_t2 also has a lossy mode, software gamma, the panel's backlight
through the pen's HID function, and recovery from a chip left halfway
through a frame; these are tied to the tablet or not described for
trigger2.

What each could take from the other:

- trigger2's timing registers and its measured PLL formula are what a
  plain-T2 profile in mct_t2 would need (any monitor, mode setting). Both
  are in [protocol-t2.md](../reference/protocol-t2.md#timing-and-pll-rhgndftrigger2-checked-against-this-tablet),
  with the PLL checked against the DTU-1031X's own values.
- mct_t2's flip after each update, hardware cursor and recovery could
  help trigger2.

Other protocol facts found in trigger2 are in protocol-t2.md, not repeated
here: the in-band `0x16` register lists and the
[second address set](../reference/protocol-t2.md#second-address-set-rhgndftrigger2-untested).

## Vendor drivers for the Trigger II

Read for interoperability facts only; no code was taken.

- **MCT TriggerFamily for Windows**: the 24.01 and 25.01 packages carry the
  same Trigger II files (the j5create Windows package 24.01 is MCT's, with
  only the signatures differing). StarTech's and SIIG's packages are MCT's
  too. The Windows display stack is described in
  [t2-vendor-driver-analysis.md](t2-vendor-driver-analysis.md).
- **The older Windows kernel driver** (`t2usb64.sys`), in the same
  packages: per-device limits and settings (see "Products" below).
- **j5create's macOS driver, written for MCT** (versions 3.3 of 2020 and
  4.2.2 of 2026): a complete Trigger II backend that drives the chip from
  userspace and names the DTU-1031X. It gave the ID ROM layout (including
  the panel's "rotated 180 degrees" flag), the power register sets, the
  cursor address and edge-clip registers, and the EP2 timeout register.
  These are in [protocol-t2.md](../reference/protocol-t2.md), with what was
  tested on the tablet.

No other OEM package with its own Trigger II code was found.

## Products with a Trigger II

| Product | Evidence |
|---|---|
| j5create JUA210 (VGA), JUA230 (DVI), JUA250 (HDMI) | j5create's tray settings map its display utility to `0711:5300-5308`; trigger2 drives a JUA230/JUA230U |
| StarTech USB2VGAE3, USB2DVIE3 (and USB2VGAE2) | T2-285A / T2-285B per StarTech; "Trigger II External Graphics" reported for the USB2VGAE2 |
| SIIG JU-VG0111-S1 | "MCT Trigger II, 16 MB SDRAM" in its listings |
| MCT UVGA-A8211 | "Trigger II T2-285, 16MB SDRAM", up to 1680x1050 / 1600x1200 |
| Wacom DTU-1031X, DTU-1031H3 | named in the macOS driver's Trigger II code; Wacom offers MCT's package for the DTU-1031X. The DTU-1031AX is a Trigger 6 (`0711:5609`) |

Not Trigger II: `0711:5100` is a Trigger 1+ (Tritton XD300, UVGA-A8201,
the first USB2VGAE2).

USB IDs: MCT's Windows INF binds `0711:5200-520f`, `5300-530f`,
`5400-540f` and `5500-550f` as "Trigger II External Graphics"; `5210` is
special-cased by both Windows drivers (at most 1680 x 1088). The low bits
are not a product code: the DTU-1031X's ID ROM holds `0711:5200`, and the
chip enumerates as `520d` or `5209`.

What the older Windows driver allows any Trigger II: at most 1920 x 1200,
16 MiB of video memory (8 MiB on the oldest chips). trigger2 measured its
own limits on a JUA230 (above). Driving these adapters with mct_t2 would
still need a plain Trigger II to test on: their init, EDID and modes
differ from the Wacom panel's.
