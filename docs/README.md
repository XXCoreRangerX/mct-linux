# Documentation

Everything learned about the MCT Trigger chips and the driver, including the
lab notes. File names start with the chip they are about (`t2-`, `t6-`)
unless they cover both; the protocol files are `protocol-t2.md` and
`protocol-t6.md`. Each file starts with its status:

- **Reference**: describes the code and the hardware as they are now, and
  is updated with them.
- **Status**: where things stand and what is left, updated as work goes on
  (the roadmap).
- **Design (date)**: a plan as written on that date; sections marked as
  kept current are updated with the code.
- **Research snapshot (date)**: an investigation as it stood on that date.
  It isn't rewritten; what came of it is in the roadmap or the reference
  docs.
- **Research, ongoing (date)**: an investigation still open, updated as it
  goes on.
- **History**: chronological records. Entries are not rewritten; later
  findings are added after them, so earlier ones may have been superseded.
- **Lab how-to**: how the research setup works; not needed to use the
  driver.
- **Legacy**: a route that still works but is no longer the recommended one.

The rule for dated docs (snapshots, design plans, history): the old text
stays. A later fact is added as an `EDIT (date): ...` note after the text it
corrects, which is struck through (`~~old~~`), or as an `Added (date): ...`
note. A snapshot that got such notes says "with dated additions" in its
status line. Material about the other chip goes to that chip's file instead.

## Start here

| File | Status | What's in it |
|---|---|---|
| [roadmap.md](roadmap.md) | Status | where the driver stands against the vendor drivers and the chips, what's open, parked or dropped |

## reference/: the chips and devices

| File | Status | What's in it |
|---|---|---|
| [protocol-t6.md](reference/protocol-t6.md) | Reference | Trigger 6: control requests, bulk sessions, frame and rectangle headers, formats, cursor, audio, events |
| [t6-limits.md](reference/t6-limits.md) | Reference | Trigger 6 limits the driver uses (pixel clock, scanout bandwidth, buffer room) and the measurements behind them |
| [protocol-t2.md](reference/protocol-t2.md) | Reference | Trigger II: bulk commands, RLE frames, formats and scaling, flips, video memory, cursor, registers, ID ROM, I2C, the pen's brightness report |
| [t2-init-sequence.md](reference/t2-init-sequence.md) | Reference | the DTU-1031X init sequence captured from Wacom's Windows driver |
| [t2-device-dtu1031x.md](reference/t2-device-dtu1031x.md) | Reference | the Wacom DTU-1031X's hardware: USB functions, endpoints, panel, internals |

## driver/: the driver and its interfaces

| File | Status | What's in it |
|---|---|---|
| [shared-core.md](driver/shared-core.md) | Reference | how the driver is split: DRM core and chip backends, the two backends side by side, and the plan the core started from |
| [t6-driver.md](driver/t6-driver.md) | Reference | the Trigger 6 backend: probe, modes, frames, errors and events, audio, cursor, settings |
| [t2-driver.md](driver/t2-driver.md) | Reference | the Trigger II backend: design, update strategy, lossy and half-resolution modes, gamma, cursor, ID ROM, backlight, recovery, known issues |
| [ABI-sysfs-mct.txt](driver/ABI-sysfs-mct.txt) | Reference | the per-device sysfs files, in the kernel's ABI format |
| [debugfs.md](driver/debugfs.md) | Reference | the statistics files and the T6 test hook |
| [companion-app.md](driver/companion-app.md) | Design (2026-10-11) | MCT Display Manager and mctctl: design, identification, saved settings |
| [wacom-t2d.md](driver/wacom-t2d.md) | Legacy | the EVDI + wacom-t2d userspace route for the DTU-1031X |

## research/: investigations

| File | Status | What's in it |
|---|---|---|
| [t6-jpeg.md](research/t6-jpeg.md) | Research, ongoing (2026-10-13) | the T6's JPEG decoder, tested from userspace; work in progress, put aside |
| [cursor-latency.md](research/cursor-latency.md) | Research, work in progress (2026-10-15) | why cursor moves wait for the next frame under Wayland compositors but not X11, the legacy cursor call, mct's async cursor moves and the smithay/niri fix being built |
| [vendor-vm-comparison.md](research/vendor-vm-comparison.md) | Research snapshot (2026-10-15) | mct against MCT's Windows and Linux drivers on both chips, measured through the same VM path: cursor latency, bytes, video, idle traffic |
| [t6-vendor-comparison.md](research/t6-vendor-comparison.md) | Research snapshot (2026-10-11), with dated additions | mct_t6 against MCT's Windows, macOS and Ubuntu drivers, JPEG on Linux, a feature table |
| [t6-vendor-review.md](research/t6-vendor-review.md) | Research snapshot (2026-10-11), with a status per item | what MCT's T6 drivers do that mct didn't, and what became of each item |
| [t6-other-drivers.md](research/t6-other-drivers.md) | Research snapshot (2026-10-11), with dated additions | MCT's triggerdm, ramifriedman's trigger6 and the trigger6 forks, compared with mct |
| [t2-other-drivers.md](research/t2-other-drivers.md) | Research snapshot (2026-10-12/13) | rhgndf/trigger2 compared with mct_t2, the vendor T2 drivers, products with a Trigger II |
| [t2-vendor-driver-analysis.md](research/t2-vendor-driver-analysis.md) | Research snapshot (2026-09) | MCT's official Trigger driver package (T2) |
| [t2-wacom-display-settings.md](research/t2-wacom-display-settings.md) | Research snapshot (2026-10-11) | which display settings Wacom's own driver has for the DTU-1031X (brightness only) |
| [t2-optimization-ideas.md](research/t2-optimization-ideas.md) | Research snapshot (2026-10-10), with dated additions | T2 speed ideas, tried and not tried |
| [usb-capture-setup.md](research/usb-capture-setup.md) | Lab how-to | the USB capture setup (Windows VM, usb-redir tee) |

## history/: lab notes

| File | Status | What's in it |
|---|---|---|
| [t6-lab-notebook.md](history/t6-lab-notebook.md) | History | the running log of the Trigger 6 work: every experiment and result |
| [t2-lab-notebook.md](history/t2-lab-notebook.md) | History | the dated Trigger II experiments: recovery soak, probe logs, half-resolution motion, the performance backlog |
| [t2-project-history.md](history/t2-project-history.md) | History | how the project went, from the first probes to the kernel driver (mostly Trigger II) |
| [t2-first-light.md](history/t2-first-light.md) | History | the first picture on the DTU-1031X |
| [roadmap-done.md](history/roadmap-done.md) | History | roadmap items moved out when they were finished |

Licence: [LICENSE.md](LICENSE.md) (CC BY-SA 4.0). Changes per version:
[../CHANGELOG.md](../CHANGELOG.md).
