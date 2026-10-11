# debugfs files

> **Reference**, kept current with the code.

Under `/sys/kernel/debug/dri/<device>/` (root only), one directory per
display:

    sudo cat /sys/kernel/debug/dri/*/mct_t6_stats
    sudo cat /sys/kernel/debug/dri/*/mct_t2_stats

Counters run since the device was probed. The first line is common, then
come the chip's lines, then the common commit-side lines.

## Common lines (kernel/mct_drv.c)

| Line | Meaning |
|---|---|
| `seconds` | since probe |
| `skipped images` | committed images replaced by a newer one before they were sent (normal under load) |
| `pixels damaged` | pixels in rows that really changed and were copied |
| `damage clips` | commits without damage clips (whole screen assumed), number of clips and their area as the compositor reported them |
| `rows changed` | rows copied per commit after comparing with the previous image |
| `ms per fb copy` | framebuffer to shadow copy per commit (and the maximum); high values mean the framebuffer is in GPU memory |
| `ms per commit` | the same including CPU access setup |

## Trigger 6 (`mct_t6_stats`)

| Line | Meaning |
|---|---|
| `mode` | current mode, output index, format in use (NV12, YUV 4:4:4, RGB24, RGB32); `0x0` while off |
| `device` | feature level, video memory, DISP display flags, USB link |
| `updates` | frames sent and the rate |
| `rows sent` | rows sent, in how many bands, rows per update |
| `usb bytes` | total and average rate |
| `ms per update` | conversion (shadow to the device format, diffing) and USB queueing time per update |
| `motion -> NV12` | how often auto mode switched to NV12 for large changes |
| `events` | interrupt events: frame done, monitor change, redraw request, other; endpoint errors and the last error code; `polling` if the events stopped and the connector is polled instead |
| `recoveries` | transfer errors that needed the cancel request (each restarts the 5600, ~3 s without picture) |
| `usb MB/s while busy` | throughput while transfers were queued |

### Test hook: `mct_t6_inject`

Write-only, for testing the error recovery:

    echo 1 | sudo tee /sys/kernel/debug/dri/<device>/mct_t6_inject

| Value | Effect on the next update |
|---|---|
| 1 | announce a 64 KiB rectangle write, send 256 bytes of it, fail the update: normal recovery |
| 2 | send only the cancel request (restarts the 5600) |
| 3 | like 1, but the recovery skips the cancel request once |

`tools/t6_cancel_test.sh` and `tools/t6_robust_test.sh` use it. The screen
goes dark for a few seconds each time.

## Trigger II (`mct_t2_stats`)

| Line | Meaning |
|---|---|
| `updates` | frames sent |
| `lossy updates` | updates sent with a lossy tolerance, by tolerance |
| `rects` | rectangles sent |
| `pixels sent` | pixels encoded and sent (and in full frames) |
| `sent/damaged` | how much of the damaged area had to be sent |
| `usb bytes` | total and average rate |
| `ms diff/encode/usb` | total time in diffing, RLE encoding and USB |
| `usb MB/s while busy` | throughput while sending |
| `over budget` | updates larger than one 60 Hz frame's worth of data, largest payload |
| `latency ms` | time from damage to the flip, as a histogram |
