# DTU-1031X hardware facts

> **Reference**: hardware facts about the DTU-1031X.

## USB topology

One physical Mini-USB cable -> internal SMSC USB2413 2-port hub:

| USB ID | Mfr/Product | Speed | Class | Driver (Linux) | Role |
|---|---|---|---|---|---|
| `0711:520d` | MCT / DTU-1031X | HS 480M | `ff` vendor, 1 iface, 6 bulk EPs | `(none)` (now `mct`) | **video (Trigger II T2-285A)**; also enumerates as `0711:5209` |
| `056a:032f` | Wacom / DTU-1031X | FS 12M | HID (boot mouse) | `usbhid` + linuxwacom | pen (`wacom-dtu-1031x.tablet` ships) |

Video EPs: OUT `0x02/0x03/0x04/0x06`, IN `0x81/0x85`. No kernel driver bound
before mct (`Driver=(none)`), `bcdDevice 0.01`, no serial. `0x85/0x06` unused.

## Internals (iFixit teardown)

From https://www.ifixit.com/Guide/Wacom+DTU-1031X+Disassembly/166191 (device
page https://www.ifixit.com/Device/Wacom_DTU-1031X, data sheet
https://documents.cdn.ifixit.com/cYPLspRSspiWE5YP.pdf). The guide's author
found the controller damaged in all three of their units.

- MCT Trigger II **T2-285A** (USB->VGA) + EtronTech 128 MB DDR1
- Realtek **RTD2280DLW** VGA->LVDS (panel path; shows Wacom logo with no signal)
- Wacom W8008 pen controller, SMSC USB2413 hub, MK60D X256ZV10
- T2-285A chips run hot / fail often; keep sessions short-ish if flaky.

## Panel / EDID (recovered live via `0x1d`, offset `0x80`)

`data/panel_edid.bin` (128 B, checksum `0x27` OK):

- WAC model 4165, week 21/2014, **analog (VGA)** input, 22x13 cm gamma 2.2
- Preferred + only DTD: **`1024x600 @ 59.99 Hz, 50.630 MHz`**
  - H: front 24, sync 136, back 160, pol N (total 1344)
  - V: front 1, sync 4, back 23, pol N (total 628)
- Name `DTU-1031X`, serial `1`. No established/standard timings.

Target framebuffer: **1024x600 RGB** (`1024*600*3 = 1,843,200 B`).

## Register map (idle, freshly plugged; `data/t2_regsweep.txt`, 512 reads)

Read via EP `0x03` cmd `05 <lo> <hi>`, 1 B reply on `0x81`. Notable:

- `fc00..fc13`: structured (mode/state area), e.g. `fc01=0x07`
- `fc30=0xff fc31=0x03 fc32=0x01` idle; change on control writes (`fc28=1` ->
  `fc30/31=0 fc32=2`)
- `feb0=0x00 feb1=0xf5` idle; `feb0=0x43` cascades `feb1->0xb4`
- `fed0..fedf`: data block; `fea1=fea2=0x55`; `fce8/ea=0x80`
- `fca3`: `0x80` idle -> driver writes `0x6e`, reads back `0x6e`
- `fc6f`, `fc59`, `fc32`, `fc34`, `fc2f`, `fbf6`, `fcb0`, `fcb5` written during init

Full map: `data/t2_regsweep.txt`. Doc examples from cyrozap JUA230 capture
(`feb0=0x43`, `fc28=0x01`) verified live, same T2 family (`5200`<->`520d`).

## Trigger II family (Windows INF `TrgIdd64.inf`, driver v25.01)

`USB\VID_0711&PID_520D` -> `Trigger2_Install` (UMDF). Family PIDs:
`5200-5210` (T1N=5210), `5300-530F`, `5400-540F`, `5500-550F` (183 PIDs total
incl. T1/T5/T6/T7). UMDF: `WUDFRd` + `UmdfService=TrgIdd` (+`IddCx0102`),
`TriggerDriverName=t2indisp.dll`.
