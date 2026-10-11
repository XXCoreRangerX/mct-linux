# Trigger 6 limits

> **Reference**, kept current with the code. The measurements behind each limit are in the lab notebook.

What the limits in `kernel/mct_t6.c` are based on. Measured on a T6 USB
Station (0711:5600, "Super Lite", feature level 6). The details of each test
are in [t6-lab-notebook.md](../history/t6-lab-notebook.md).

## Pixel clock (`T6_MAX_CLOCK_KHZ`, 340 MHz)

| Clock | Result |
|---|---|
| up to 333 MHz (1920x1080@144, reduced blanking) | fine in daily use |
| 348 MHz (1920x1080@150) | a few green pixels after a while |
| 353 MHz | visible artifacts |
| 360 MHz and up | no signal |

The device's own mode table stops at 2048x1152@60 (197 MHz).

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
| YUV 4:4:4 | 1080p120 (~746 MB/s, daily use) | 1080p131 (~815, thin lines during activity), 3440x1440@57 (~847), 1080p144 (~896) | 760 MB/s |
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

## Mode and device checks

What else decides whether a mode is offered (`t6_mode_entry`,
`t6_get_modes`, `t6_read_caps`):

- width a multiple of 16, height even, no interlaced modes
- largest image 3840x2160, or the device table's largest entry if bigger
  (entries above 4096x2304 are ignored)
- on a USB 2 link: widths up to 2048 and 0x6400 byte writes
- computed extra refresh rates (75, 100, 120, 144, 150, 165, 200, 240 Hz)
  only for sizes up to 1920x1080, within the monitor's range, pixel clock
  and line rate
- when the monitor's preferred mode is out of reach: the same size with
  reduced blanking, the highest rate where 4:4:4 fits, or 2560x1440@60 for
  16:9 monitors too tall for the adapter
- no usable EDID: the device table's modes up to 1920x1080, 1080p60
  preferred
- modes larger than the monitor's native resolution (its preferred mode)
  are dropped unless `all_modes=1`
- probe refuses feature level below 3 (JPEG only) and less than 48 MiB of
  video memory (the three frame buffers end there)
- cursor images 64, 96 or 128 px; a 128 px image is sent only up to
  0x10000 bytes (16-bit offset), its last two pixels never

## Requests that stall

On feature level 6: gamma tables (`40 1c` / `40 1d`) and hardware rotation
(`40 1f`). The driver applies gamma in software and leaves rotation to the
compositor.
