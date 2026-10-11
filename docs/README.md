# Documentation

Everything learned about the MCT Trigger chips and the driver, including the
lab notes. Dated files and sections record what was true and tried at the
time; later findings are added, earlier ones are kept.

## Start here

| File | What's in it |
|---|---|
| [roadmap.md](roadmap.md) | where the driver stands against the vendor drivers and the chips, what's left |
| [t6-comparison.md](t6-comparison.md) | the T2 and T6 backends side by side, and mct against MCT's Windows, macOS and Ubuntu drivers (feature table) |
| [shared-core.md](shared-core.md) | how the driver is split: DRM core and chip backends |
| [companion-app.md](companion-app.md) | design of the planned desktop app (per-device settings, tray, About) |

## Protocol references

| File | What's in it |
|---|---|
| [protocol-t6.md](protocol-t6.md) | Trigger 6: control requests, bulk sessions, frame and rectangle headers, formats, cursor, events |
| [t6-limits.md](t6-limits.md) | Trigger 6 limits the driver uses (pixel clock, scanout bandwidth, buffer room), and the measurements behind them |
| [protocol-t2.md](protocol-t2.md) | Trigger II: bulk commands, RLE frames, flips, cursor, registers, I2C, the pen's brightness report |
| [init-sequence.md](init-sequence.md) | the DTU-1031X init sequence captured from Wacom's Windows driver |
| [device-dtu1031x.md](device-dtu1031x.md) | the Wacom DTU-1031X's hardware: USB functions, endpoints, panel |

## Driver notes

| File | What's in it |
|---|---|
| [t6-driver.md](t6-driver.md) | the Trigger 6 backend: probe, modes, frames, errors and events, cursor |
| [debugfs.md](debugfs.md) | the statistics files and the T6 test hook |
| [ABI-sysfs-mct.txt](ABI-sysfs-mct.txt) | the per-device sysfs files, in the kernel's ABI format |
| [kernel-driver.md](kernel-driver.md) | the Trigger II backend: design, measurements, lossy mode, recovery, known issues |
| [display-driver.md](display-driver.md) | the older EVDI + wacom-t2d userspace route for the T2 |
| [optimization-ideas.md](optimization-ideas.md) | T2 speed ideas, tried and not tried |
| [wacom-display-settings.md](wacom-display-settings.md) | which display settings Wacom's own driver has for the DTU-1031X (brightness only) |

## Research on the vendor drivers

| File | What's in it |
|---|---|
| [driver-analysis.md](driver-analysis.md) | MCT's official Trigger driver package (T2) |
| [t6-vendor-review.md](t6-vendor-review.md) | what MCT's T6 drivers do that mct didn't, and what became of each item |
| [capture.md](capture.md) | the USB capture setup (Windows VM, usb-redir tee) |

## Lab notes and history

| File | What's in it |
|---|---|
| [t6-lab-notebook.md](t6-lab-notebook.md) | the running log of the Trigger 6 work: every experiment and result |
| [first-light.md](first-light.md) | the first picture on the DTU-1031X |
| [project-history.md](project-history.md) | how the project went, from first probes to the kernel driver |

Licence: [LICENSE.md](LICENSE.md) (CC BY-SA 4.0). Changes per version:
[../CHANGELOG.md](../CHANGELOG.md).
