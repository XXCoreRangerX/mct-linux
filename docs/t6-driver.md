# Kernel driver, Trigger 6 backend (`mct_t6.c`)

How the T6 backend works, as of mct_t6 0.21. The protocol is in
[protocol-t6.md](protocol-t6.md), the limits and their measurements in
[t6-limits.md](t6-limits.md), the shared DRM side in
[shared-core.md](shared-core.md), the history of every decision in
[t6-lab-notebook.md](t6-lab-notebook.md).

## Probe

Interface 0 of the device. Reads which output has a monitor (`0x87`), the
capabilities (`t6_read_caps`: platform and PLL base clock, feature level,
video memory, DISP flags; refuses JPEG-only devices and less than 48 MiB),
the device's mode table (`0x89`), allocates a pool of 16 bulk URBs and the
interrupt URB, registers with the core, starts the event endpoint. USB 2
links get smaller writes and modes up to 2048 wide.

## Modes

`t6_get_modes` reads the EDID (`0x80`) and adds:

- extra refresh rates for the preferred size (up to 1080p), within the
  monitor's range, pixel clock and line rate: CVT reduced blanking where it
  fits, a tighter reduced blanking otherwise;
- when the monitor's preferred mode is out of reach: the same size with
  reduced blanking at the highest rate that works, the highest rate where
  YUV 4:4:4 fits the scanout (a nearby standard rate becomes the default),
  or 2560x1440@60 for 16:9 monitors too tall for the adapter;
- without a usable EDID: the device's table up to 1080p.

`t6_mode_entry` turns a DRM mode into the device's 32-byte mode entry,
including the PLL settings (MCT's formula), and is the single check of what
the adapter can drive.

`enable` (from the core, updates stopped): allocates the converted image and
three frame buffer copies for the mode's size, sends set mode, "software
ready" and output on, forgets what the device's buffers hold. `disable`:
cursor off, output off.

## Frames

The core copies changed rows of each commit into a shadow image and runs
`t6_update`:

1. pick the format: the `format` parameter, or in auto mode exact RGB or
   YUV 4:4:4 where the scanout bandwidth allows, NV12 during large changes
   (more than a quarter of the screen) and for 150 ms after them;
2. convert the changed rows (gamma LUT applied here, YUV full range);
3. compare with what the next of the three device frame buffers holds and
   build up to 16 bands of changed rows, each limited to the changed
   columns;
4. send the bands as rectangle writes (session type 7) and switch to that
   buffer with an empty flip. The buffer on screen is never written.

USB transfers are queued without waiting (up to 16), each write under
0x19000 bytes (0x6400 on USB 2).

## Errors and events

- A failed or stuck transfer: kill what is queued, clear a stall, send the
  cancel request (`0x14`; on the 5600 it restarts the device, which comes
  back ~3 s later and is probed again), resend whole frames 100 ms later.
  `mct_t6_inject` in debugfs tests this ([debugfs.md](debugfs.md)).
- Monitor change events: re-read the status until it reads the same three
  times 300 ms apart, then set the mode and send everything again if a
  monitor is on, and let userspace re-probe. Without events the connector
  is polled.
- Redraw request events: resend picture and cursor.
- Event endpoint errors: reset the endpoint and resubmit with backoff;
  after 10 failures fall back to polling.

## Cursor

The core hands over the cursor as pixels and a screen position. The backend
uploads the smallest of 64, 96 or 128 px that holds everything visible, in
4096-byte chunks, into the cursor slot not on screen, then selects it.
Negative positions are sent as v - 1. A 128 px image is sent up to 0x10000
bytes (16-bit offset).
