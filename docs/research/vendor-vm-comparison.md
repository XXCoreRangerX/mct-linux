# Vendor drivers vs mct, measured side by side

> **Research snapshot** (2026-10-15): MCT's Windows and Linux drivers and mct, each in a VM, all seeing the device through the same filtering usb-redir path.

MCT's own drivers compared with mct on both USB display devices:

- the j5create JUA254 T6 adapter (0711:5600, driving the LG 27GN7 over HDMI at 1920x1080@60)
- the Wacom DTU-1031X tablet display (T2, 0711:520d, 1024x600)

Every driver ran in a VM, and every USB packet went through the same chain:
`usbredirect` on the host, then a filtering usb-redir proxy, then QEMU `usb-redir` on an emulated xHCI controller. Absolute numbers include that chain's overhead (TCP loopback, QEMU, the guest's xHCI emulation), so only the differences between drivers are meaningful.

Raw captures, per-run logs and the scripts (the filtering proxy, injection and analysis) are kept in the lab, not in this repository.

## Drivers measured

| Device | Driver | Where | Version |
|---|---|---|---|
| T6 | mct (`mct_t6`) | NixOS VM, kernel 7.2.7, niri (llvmpipe) | 0.29.13 (repo HEAD) |
| T6 | MCT's Linux daemon `T6evdi` (evdi_t6_1 source, EVDI 1.15.1, libusb, libjpeg-turbo) | same VM, mct blacklisted | built from `third_party/t6-sources/linux/evdi_t6_1` |
| T6 | MCT Windows driver (`t6sta.sys` 1.0.2501.1021 + `TrgIdd64` IDD 1.9.2501.1223 / `T6InDisp.dll`) | Windows 11 Enterprise eval 24H2 VM | from TriggerFamily-Dongle-25.01.1223.0179 |
| T2 | mct (`mct_t2`) | NixOS VM as above | 0.3.40 (repo HEAD) |
| T2 | MCT Windows driver (`TrgIdd64` IDD + `T2Indisp.dll`) | Windows VM as above | same package |

MCT ships no Linux driver for the T2, so the T2 comparison is mct vs Windows only.

## Method

Phases, the same for every driver:

- **Idle:** 20 s with the pointer parked on the USB display and nothing changing.
- **Cursor:** 200 relative pointer moves of ±3 px at 57 per second, injected over QMP into an emulated USB mouse. The host logs CLOCK_MONOTONIC right before each QMP send; the proxy logs the same clock for every packet.
- **Shape:** a text field (a `foot` terminal under niri, a WinForms text box under Windows) on the left half, background on the right half. 20 jumps across the edge, 0.5 s apart.
- **Video:** a 1080p50 test video (testsrc2 + temporal noise, H.264, 30 s), played fullscreen with mpv (software decode) for 20 s.

How the numbers are computed:

- **Latency:** time from an injection to the first cursor-position request on USB after it, before the next injection.
- **Byte counts:** USB payload bytes (control: 8-byte setup + data; bulk: transfer length).
- **Frames:** counted from flip commands (T6 command 3, T2 `0x15`).

The rate is 57/s rather than 60/s on purpose. In the first mct run, injections at exactly 60/s locked in phase with the 60 Hz vblank, and the median swung between 1.5 ms and 16.1 ms from run to run depending on that phase. At 57/s the phase sweeps, so medians and p95 cover the whole frame period.

The Windows VM made the USB display its only display: the emulated adapter was disabled once the vendor IDD was up. The T6 then ran at 1920x1080@60 and the T2 at 1024x600@60. In the Linux VM, niri had QEMU's own output turned off, so the USB display was the only output.

## Results: T6 (JUA254 + LG 27GN7, 1920x1080@60)

| | mct | MCT Linux daemon (T6evdi) | MCT Windows |
|---|---|---|---|
| Idle traffic | **0 B/s** | 0 B/s | 0 B/s |
| Cursor | hardware cursor (`40 04`) | **software cursor**: drawn into full JPEG frames, no `40 04` / `40 10` at all | hardware cursor (`40 04`) |
| Injection → cursor position on USB, median / p95 | **0.75 / 8.4 ms** (at 120 Hz: 0.70 / 0.94 ms) | no position packet; injection → next frame flip 10.1 / 14.9 ms | **0.48 / 0.70 ms** |
| Bytes per move | 8 B position + 80 B empty flip (one per niri commit) = 87 B | **41.6 KB** (a full-screen JPEG frame per move) | 8 B |
| Bytes per shape change | 8 B (`40 05` slot select) | a new JPEG frame (~40 KB) | 8 B (`40 05` slot select) |
| Shapes cached on the device | **yes** (2 slots alternate; debugfs: 21 from the slot cache, 2 uploads) | n/a (no hardware cursor) | **yes** (slots 0 and 2 alternate, no upload during the phase) |
| Video: frames per second | 50.0 | 50.0 | 50.0 |
| Video: format | **raw NV12** (rectangle writes, signature 7) | JPEG | JPEG |
| Video: bytes per frame | 3.11 MB | 530 KB | 582 KB |
| Video: USB rate | 155.5 MB/s | 26.5 MB/s | 29.1 MB/s |
| Video: flip interval median / p95 / max | **20.0 / 21.2 / 23.8 ms** | 23.5 / 25.1 / 26.8 ms | 17.1 / 33.9 / 37.1 ms |

mct at 1080p120 (same attach, `niri msg output … mode 1920x1080@120`):

- The video still went out at 50.0 fps, steady (20.0 / 20.8 / 28.2 ms).
- mct used YUV 4:4:4 (source format 11) and averaged 1.56 MB per flip (77.8 MB/s): fewer bytes than 4:4:4 full frames would need, so partial updates. Why the format differs from the 60 Hz run was not investigated.
- `frame_latency.sh` inside the VM: commit → device 10.4 ms at 60 Hz and 8.2 ms at 120 Hz (50 updates/s, none replaced).

Notes:

- **mct's cursor p95 at 60 Hz (8.4 ms) is niri's frame clock, not USB.** About 1 in 6 moves waits for niri's next commit. Inside the VM, debugfs says commit → device is 0.28 ms per cursor move. Windows' IDD pushes the position as soon as the OS moves the pointer. At 120 Hz the wait halves and the p95 drops to 0.94 ms.
- The 80-byte empty flip per move comes from niri committing the primary plane on every pointer move; mct forwards each commit as a flip with no data. Windows sends only the 8-byte position.
- **MCT's Linux daemon has no hardware cursor in this build.** `evdi_enable_cursor_events` is commented out, so niri composites the cursor into the frame, and every pointer move costs a full ~42 KB JPEG frame. It also sends every frame as a full-screen JPEG (no damage rectangles). It chose JPEG over USB 3 because the LG's EDID lists 4K modes (its `bRun4K30` path). It ignored the real EDID for the EVDI output and gave niri its built-in "Microtec Generic 21.5"" EDID.
- **The daemon did not work out of the box:**
  - Before any mode set, its grab loop spins and logs "Buffer 0 not found. Not grabbing." about 2,000 times a second to stdout. Under systemd (journal + serial console), the pipe filled up, and the mode-change handler blocked on its own debug `printf` before it registered its buffer. The result was no frames at all, EVDI `flip_done` timeouts and soft-lockup warnings in the guest.
  - With stdout sent to a tmpfs file, it ran normally: about 253,000 such lines per run.
- **Windows streamed audio too** (signature 3 sessions, `40 23`/`40 24`): it enumerates the T6's audio function as a USB audio device and opens it.
- Pacing:
  - mct's video flips are the steadiest (p95 21 ms for 20 ms frames).
  - Windows' are bursty (p95 34 ms) but average 50 fps.
  - The daemon's interval median is 23.5 ms with a 20 ms mean; its intervals are bimodal around the 60 Hz grab clock.

## Results: T2 (DTU-1031X, 1024x600)

| | mct | MCT Windows |
|---|---|---|
| Idle traffic | **0 B/s** | **4.9 KB/s**: re-uploads the 16 KB cursor image every 3.0 s (`0x12` + 16384 B), like clockwork |
| Injection → cursor position on USB, median / p95 | 0.74 / 8.4 ms | **0.52 / 0.74 ms** |
| Bytes per move | 16 B EP4 position list + 23 B `0x15` flip = 38 B | 16 B |
| Bytes per shape change | 16.4 KB (image re-upload) | 16.4 KB (image re-upload) + `05 02 36 xx` hide/show |
| Shapes cached on the device | no (re-uploaded every change) | no (re-uploaded every change) |
| Video: frames per second | **29.5** | 28.8 |
| Video: bytes per frame | 940 KB (RLE, 53% of raw; rect 1024x576) | **498 KB** (RLE, 27% of raw; rect 1024x600) |
| Video: USB rate | 27.8 MB/s | 14.3 MB/s |
| Video: flip interval median / p95 / max | **33.5 / 37.0 / 40.5 ms** | 25.4 / 69.8 / 174 ms |

Notes:

- Both drivers use the same RLE settings (escape `0x30`, mode 5, flags `0x28`), full-width updates.
- Windows' frames compress to half of mct's size. The rendered pictures differ, though: different mpv scalers, and mct sent only the 576 rows of the letterboxed video. So this is not a clean encoder comparison; a byte-identical test image on both would settle it.
- mct holds a steadier ~30 fps; Windows has long stalls (max 174 ms).
- The cursor p95 difference is the same niri commit wait as on the T6. In-VM debugfs: commit → device 0.33 ms per cursor move.
- T2 shape caching is possible in principle (`fe30-fe33` image address, protocol-t2.md), but neither driver does it, and mct's 2026-10-13 attempt at a second slot showed garbage.

## What this says about mct

- **Cursor:** on par with Windows on the T6 (8 B, cached shapes) and far ahead of MCT's Linux daemon, which has no hardware cursor at all. The remaining gap to Windows is not the driver's USB path (0.28 ms from commit to device). It is when a move gets committed at all: a compositor can't commit while its previous commit's flip event is pending, and mct paces those events at the refresh rate, so a move can wait up to one refresh. On top of that, each move carries an empty flip. Both are open work (roadmap).
- **Idle:** mct sends nothing on either device; Windows' T2 driver refreshes the cursor image every 3 s.
- **Video on the T6:** all three drivers hold 50 fps. mct does it raw (NV12, 155 MB/s over USB 3) where both MCT drivers use JPEG at about a fifth of the bandwidth; mct's frame pacing is the steadiest. On a USB 2 link or at higher resolutions, JPEG (mct's parked work) would matter.
- **Video on the T2:** same frame rate as Windows with steadier pacing, but about twice the bytes per frame, worth a closer look with identical content.

## Safety record

- **Filtering proxy:** built from `tools/tee.py`. It parses usbredir packets in both directions and checks every guest → device packet before forwarding it.
  - **T6 hard blocks** (drop the packet and close both sockets):
    - vendor or class requests 0x01, 0x13, 0x15, 0x30, 0x33, 0x34, 0x40, in either direction
    - bulk session signature 5
    - unknown bulk signatures, tracked with a header/payload state machine
    - unknown standard requests
  - **T6 soft blocks:** an unknown vendor OUT request gets a STALL back to the guest.
  - **T2 hard blocks:**
    - any EP3 opcode outside the known-safe set, which excludes 0x1a, 0x1c and 0x28
    - register writes to the I2C masters (`fce0-fcef`) via EP3 `0x02`/`0x04`, EP2 `0x15`/`0x16` or EP4 lists
    - unknown EP2 or EP4 commands
  - **T2 soft blocks:** any vendor or class control OUT request.
  - **Both devices:** `usb_redir_reset` from the guest is dropped.
  - Self-test with synthetic packets: 30/30 pass. It was run before any device was attached and again after each rule change.
- **Block events:** **none**, hard or soft, in the 11 sessions whose logs were kept. The log of the 12th session (the first gdb diagnostic run, which failed on PATH) was deleted with its directory; that run attached the T6 but measured nothing. Every guest reset request was dropped: 2-6 per session, 34 in the kept logs.
- **No USB resets by the tooling:**
  - `usbredirect` was rebuilt with usbredir's `QUIRK_DO_NOT_RESET` forced on, so it never resets the device: not at attach, not at release, not on guest request.
  - Both devices kept their bus and device numbers (2-002 and 6-014) through every cycle, so neither ever re-enumerated.
- **Windows firmware update:**
  - Before the driver was ever installed, `HKLM\SYSTEM\CurrentControlSet\Services\t6sta\Parameters` got `RomAutoUpdateFlags = 0` and `ForcedUpdateFW = 0`.
  - Only `t6sta.inf` and `TrgIdd64.inf` with their binaries were installed, with `pnputil`. The InstallShield setup never ran, so no `.t6img` images and no `Parameters\Firmware\<project>` image paths exist.
  - A scan of DriverStore, Program Files, `drivers` and ProgramData found 0 images, at install and again before and after each device attach. Each Windows run boots without the device, runs that check, and hot-plugs the device over QMP only when the check passes.
  - After the T6 runs, t6sta's registry shows `RomState 2`, `RomError 0`, `RomResult` all zeros and `RomSize 0x400000`.
  - The request audit lists every request Windows sent. Its only ROM-related requests are reads: `c0 b0` version ×6, `c0 b4` ROM size, `c0 b1` function IDs.
- **Linux daemon:** only `T6evdi` was built. `T6contrl`, the tool that contains `t6_write_rom_date` (signature 5 burn), was not. The daemon sent no request outside the known set.
- **Final host state:**
  - T6: `mct_t6 0.29.12`, firmware `16062899 17091401` (unchanged), errors 0, recoveries 0.
  - T2: `mct_t2 0.3.39`, errors 0.
  - The pen (056a:032f) was never redirected.

### Attach/release cycles

| Device | Cycles | Runs |
|---|---|---|
| T6 | 10 | mct ×4 (bad niri config; foot/mpv not found; 60/s phase-locked; final incl. 120 Hz), T6evdi ×4 (log-flood hang; two gdb diagnostics, the first of which failed on PATH and was discarded; final), Windows ×2 (display layout wrong; final) |
| T2 | 2 | mct ×1, Windows ×1 |

None of the cycles reset the device (see above).

## Caveats

- Everything ran in VMs with software rendering (llvmpipe under niri, Windows' IDD path with no GPU). Frame rates above 50 fps were not tested.
- The Linux daemon and mct ran under niri; Windows ran its own compositor (DWM). Cursor timing includes each compositor's behaviour.
- The Windows eval ISO (build 26100.1742) is past its evaluation date. The VM clock was set to 2025-01-15 to avoid the expired-licence hourly shutdown.
- The vendor daemon's cursor latency is "injection → next JPEG flip". It has no position packet, and the flip that carries the moved cursor may be a later one.
- The T2 frame-size comparison uses different rendered content (see the T2 notes).
