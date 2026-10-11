# Documentation

Everything learned about the MCT Trigger chips and the driver, including the
lab notes. Each file starts with its status:

- **Reference**: describes the code and the hardware as they are now, and
  is updated with them.
- **Status**: where things stand and what is left, updated as work goes on.
- **Research**: a dated snapshot of an investigation, not updated
  afterwards; what came of it is in the roadmap or the reference docs.
- **History**: chronological records. Entries are not rewritten; later
  findings are added after them, so earlier ones may have been superseded.
- **Legacy**: a route that still works but is no longer the recommended one.

## Start here

| File | Status | What's in it |
|---|---|---|
| [roadmap.md](roadmap.md) | Status | where the driver stands against the vendor drivers and the chips, what's left |
| [t6-comparison.md](t6-comparison.md) | Status | the T2 and T6 backends side by side, and mct against MCT's Windows, macOS and Ubuntu drivers (feature table) |

## reference/: the chips and devices

| File | Status | What's in it |
|---|---|---|
| [protocol-t6.md](reference/protocol-t6.md) | Reference | Trigger 6: control requests, bulk sessions, frame and rectangle headers, formats, cursor, events |
| [t6-limits.md](reference/t6-limits.md) | Reference | Trigger 6 limits the driver uses (pixel clock, scanout bandwidth, buffer room) and the measurements behind them |
| [protocol-t2.md](reference/protocol-t2.md) | Reference | Trigger II: bulk commands, RLE frames, flips, cursor, registers, I2C, the pen's brightness report |
| [init-sequence.md](reference/init-sequence.md) | Reference | the DTU-1031X init sequence captured from Wacom's Windows driver |
| [device-dtu1031x.md](reference/device-dtu1031x.md) | Reference | the Wacom DTU-1031X's hardware: USB functions, endpoints, panel, internals |

## driver/: the driver and its interfaces

| File | Status | What's in it |
|---|---|---|
| [shared-core.md](driver/shared-core.md) | Reference | how the driver is split: DRM core and chip backends |
| [t6-driver.md](driver/t6-driver.md) | Reference | the Trigger 6 backend: probe, modes, frames, errors and events, cursor, settings |
| [t2-driver.md](driver/t2-driver.md) | Reference, dated measurements | the Trigger II backend: design, measurements, lossy mode, recovery, known issues |
| [ABI-sysfs-mct.txt](driver/ABI-sysfs-mct.txt) | Reference | the per-device sysfs files, in the kernel's ABI format |
| [debugfs.md](driver/debugfs.md) | Reference | the statistics files and the T6 test hook |
| [companion-app.md](driver/companion-app.md) | Design and interface | MCT Display Manager and mctctl: the sysfs interface they use, identification, design |
| [display-driver.md](driver/display-driver.md) | Legacy | the EVDI + wacom-t2d userspace route for the DTU-1031X |

## research/: investigations

| File | Status | What's in it |
|---|---|---|
| [t6-vendor-review.md](research/t6-vendor-review.md) | Research, with item status | what MCT's T6 drivers do that mct didn't, and what became of each item |
| [other-linux-drivers.md](research/other-linux-drivers.md) | Research (2026-10-11) | MCT's triggerdm, ramifriedman's trigger6 and the trigger6 forks, compared with mct |
| [driver-analysis.md](research/driver-analysis.md) | Research (2026-09) | MCT's official Trigger driver package (T2) |
| [wacom-display-settings.md](research/wacom-display-settings.md) | Research (2026-10-11) | which display settings Wacom's own driver has for the DTU-1031X (brightness only) |
| [optimization-ideas.md](research/optimization-ideas.md) | Research (2026-10-10) | T2 speed ideas, tried and not tried |
| [capture.md](research/capture.md) | Lab how-to | the USB capture setup (Windows VM, usb-redir tee) |

## history/: lab notes

| File | Status | What's in it |
|---|---|---|
| [t6-lab-notebook.md](history/t6-lab-notebook.md) | History | the running log of the Trigger 6 work: every experiment and result |
| [project-history.md](history/project-history.md) | History | how the project went, from the first probes to the kernel driver (mostly Trigger II) |
| [first-light.md](history/first-light.md) | History | the first picture on the DTU-1031X |

Licence: [LICENSE.md](LICENSE.md) (CC BY-SA 4.0). Changes per version:
[../CHANGELOG.md](../CHANGELOG.md).
