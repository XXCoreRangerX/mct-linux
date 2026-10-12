# Kernel driver, Trigger 6 backend (`mct_t6.c`)

> **Reference**, kept current with the code. Describes mct_t6 0.26.0.

How the T6 backend works. The protocol is in
[protocol-t6.md](../reference/protocol-t6.md), the limits and their measurements in
[t6-limits.md](../reference/t6-limits.md), the shared DRM side in
[shared-core.md](shared-core.md), the history of every decision in
[t6-lab-notebook.md](../history/t6-lab-notebook.md).

## Probe

Interface 0 of the device. Reads which output has a monitor (`0x87`), the
capabilities (`t6_read_caps`: platform and PLL base clock, board name,
firmware versions, chip serial, feature level, video memory, DISP flags;
refuses JPEG-only devices and less than 48 MiB),
the device's mode table (`0x89`), allocates a pool of 16 bulk URBs and the
interrupt URB, registers with the core, starts the event endpoint. USB 2
links get smaller writes and modes up to 2048 wide.

## Modes

`t6_get_modes` reads the EDID (`0x80`; up to 5 tries, and if all fail while
a monitor is connected, the last EDID read is used again) and adds:

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

## Audio

Since 0.29.0, when the kernel has ALSA PCM support (`CONFIG_SND_PCM`) and
the adapter reports 48 kHz, 16-bit stereo (`a5`), the driver registers a
sound card named after the board, with one playback device "HDMI". If
that fails, the display still works.

- Start (ALSA trigger): an hrtimer ticks every 5 ms and queues a work item
  on the high-priority workqueue.
- The work takes `io_lock`, so packets go between frames, never inside one.
  On the first run it enables the engine (`40 24`, Activity 1, the format
  and cyclic buffer size read from `a4` at probe, ReturnSize 9600).
- It then sends 1920-byte (10 ms) packets, each a 32-byte session header
  (signature 3) and the PCM, until about 120 ms more than the host clock's
  elapsed playback has been sent. Data due now is sent whether or not the
  application has written it (ALSA reports the underrun); the lead only
  from what it has written.
- The ALSA position is what has been sent. The delay reported is sent
  minus played by the host clock, at most one second.
- Stop: the timer stops and the work sends Activity 0. `sync_stop` waits
  for the work before ALSA frees or resets the buffer.
- Unplug: the card is disconnected and the timer and work stopped before
  the DRM device goes. Suspend: streams are suspended and the timer and
  work stopped; the next start enables the engine again.
- Events with the audio function bit (0x20) are counted with their DMA
  pointer (debugfs `audio` line), to find out whether the adapter can
  pace playback itself.

Audio is heard only while the display is on (HDMI carries it in the video
signal). With the display off, playback runs but nothing is heard. The volume node
is not exposed yet; use the system's software volume.

## Cursor

The core hands over the cursor as pixels and a screen position. The backend
uploads the smallest of 64, 96 or 128 px that holds everything visible, in
4096-byte chunks, into the cursor slot not on screen, then selects it.
Negative positions are sent as v - 1. A 128 px image is sent up to 0x10000
bytes (16-bit offset).

## Settings and information

Per device in sysfs (`/sys/bus/usb/devices/<interface>/mct/`, see
[ABI-sysfs-mct.txt](ABI-sysfs-mct.txt)): `format` and `all_modes` to set,
the module parameters of the same names as defaults for new devices;
counters, mode, pixel format, USB link, and what the device reported at
probe (platform, board, firmware, serial, feature level, video memory,
display flags) to read.
