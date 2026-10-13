# JPEG on the Trigger 6 (work in progress)

> **Research, ongoing** (2026-10-13): what is known about the T6's JPEG decoder from userspace tests on a j5create JUA254 (`0711:5600`, firmware 17091401). JPEG is not in the driver; this page says why and what it would take.

## Why JPEG at all

The T6 can decode JPEG frames itself. The vendor drivers use it to send less
data over USB: they encode each frame on the CPU and the adapter decodes it.
mct sends uncompressed frames (RGB, YUV 4:4:4 or NV12), which a JPEG
encoder in the kernel would change. That wouldn't be accepted upstream, so
JPEG here would mean a userspace helper doing the encoding.

## What the tests found

`tools/t6_jpeg_test.py` (run with `tools/t6_run.sh`, which unbinds the
driver and binds it again) sends JPEG frames straight to the adapter, on a
1080p monitor at 60 Hz.

| Question | Answer |
|---|---|
| Full-screen JPEG frames | ✅ work |
| Partial updates (a JPEG rectangle into the hidden buffer, then a flip) | ✅ work |
| Decode time at 1080p | ~12 ms for simple pictures, ~30 ms for detailed ones, much more for very detailed content (noise: ~330 ms) |
| Decode time at larger sizes | 2560x1440: 71 ms, 3440x1440: 120 ms, 3840x2160: 329 ms |
| A reset request for the decoder (`0x86` in MCT's headers) | doesn't exist on this firmware (refused, harmless) |
| Leaves the adapter clean for the driver | ✅ when the rules below are kept |

Rules found the hard way:

- **JPEG data must not carry the "more pieces follow" mark.** A JPEG frame
  goes out in pieces with their offset; with the mark on (as raw frames
  take it) nothing was shown.
- **The bitstream must not go into a ring that wraps around with the
  decoder-reset flag** (how MCT's Ubuntu daemon does it). At the wrap the
  adapter's event channel failed, and once the decoder only came back after
  unplugging the adapter. Three fixed slots used in turn work cleanly.

## What it would bring

| Link | Raw frames today | With JPEG |
|---|---|---|
| USB 3 | ~95-100 fps at 1080p, ~45 fps at 3440x1440, whatever the content | slower everywhere: ~30-100 fps at 1080p depending on the content, ~8 fps at 3440x1440 |
| USB 2 | ~11 fps at 1080p for full-screen changes | ~30-100 fps at 1080p for desktop content, falling for photos or film grain |

So JPEG only pays off on USB 2 links, and there only with a limit on how
large each frame may get, falling back to raw frames above it.

## Status and next steps

**Put aside for now** (USB 2 isn't the setup being tested). If it is picked
up again:

1. Measure the same over a USB 2 port.
2. Decide the interface: a userspace encoder fed by the driver, with the
   driver keeping the decisions (when JPEG, when raw) and falling back to
   raw if the helper is missing or slow.
3. Build it for USB 2 links only, with the transport rules above.

The test runs and their logs are summarised in the lab notebook
([t6-lab-notebook.md](../history/t6-lab-notebook.md), "JPEG path").
