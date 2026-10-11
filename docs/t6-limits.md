# Trigger 6 limits

What the limits in `kernel/mct_t6.c` are based on. Measured on a T6 USB
Station (0711:5600, "Super Lite", feature level 6). The details of each test
are in [t6-lab-notebook.md](t6-lab-notebook.md).

## Pixel clock (`T6_MAX_CLOCK_KHZ`, 340 MHz)

| Clock | Result |
|---|---|
| up to 333 MHz (1920x1080@144, reduced blanking) | fine in daily use |
| 348 MHz (1920x1080@150) | a few green pixels after a while |
| 353 MHz | visible artifacts |
| 360 MHz and up | no signal |

The device's own mode table stops at 2048x1152@60 (~160 MHz).

## Wide modes (`T6_WIDE_W`, `T6_WIDE_MAX_CLOCK_KHZ`, 316 MHz from 2560 wide)

On a 3440x1440 monitor over HDMI:

| Mode | Clock | Result |
|---|---|---|
| 2560x1440@60, monitor timing | 241.5 MHz | fine |
| 2560x1440@80, reduced blanking | 316.0 MHz | fine |
| 3440x1440@50 to @57, monitor timing | 266.6 to 303.9 MHz | fine |
| 3440x1440@60, reduced blanking | 312.8 MHz | fine |
| 3440x1440@62, reduced blanking | 323.7 MHz | fine |
| 3440x1440@60, monitor timing | 319.75 MHz | black screen (once the monitor hung) |
| 3440x1440@64, reduced blanking | 334.3 MHz | monitor dropped the signal |
| 3840x2160@30, CTA timing | 297 MHz | fine (NV12) |

The same clocks work at 1080p on another monitor, so this may be the
monitor's HDMI input rather than the T6; the driver stays within what
worked. When a monitor's preferred mode is out of reach, the driver offers
it with reduced blanking or at a lower rate (`t6_add_preferred_fallbacks`).

## Scanout bandwidth (`t6_scan_max`)

The device reads the whole frame buffer once per refresh; above these rates
the picture shows thin vertical lines or flickers.

| Format | Clean | Not clean | Limit |
|---|---|---|---|
| YUV 4:4:4 | 1080p120 (~750 MB/s), 3440x1440@55 (~817 MB/s) | 3440x1440@57 (~847), 1080p144 (~896) | 820 MB/s |
| RGB24 | 1080p75 (~470 MB/s) | 1080p100 (~620), 1080p120 | 500 MB/s |
| RGB32 | (not tested past RGB24's limit) | | 500 MB/s |
| NV12 | 1080p144, 3440x1440@60, 4K30 (~450 MB/s) | | none found |

YUY2 (format 0) doesn't scan out properly at all (row length limit, unsynced
flips), see the notebook.

## Frame buffers (`T6_BUF_SIZE`)

Three buffers 0xfffec0 (~16.8 MB) apart, as the vendor driver lays them out.
A frame has to fit one with its 0x30 byte header:

| Size | NV12 | YUV 4:4:4 / RGB24 | RGB32 |
|---|---|---|---|
| 2560x1440 | 5.5 MB | 11.1 MB | 14.7 MB |
| 3440x1440 | 7.4 MB | 14.9 MB | 19.8 MB (doesn't fit) |
| 3840x2160 | 12.4 MB | 24.9 MB (doesn't fit) | 33.2 MB (doesn't fit) |

The driver picks only formats that fit, at a given mode, both a buffer and
the scanout limit.

## Requests that stall

On feature level 6: gamma tables (`40 1c` / `40 1d`) and hardware rotation
(`40 1f`). The driver applies gamma in software and leaves rotation to the
compositor.
