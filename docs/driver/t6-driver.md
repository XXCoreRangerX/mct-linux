# Kernel driver, Trigger 6 backend (`mct_t6.c`)

> **Reference**, kept current with the code (mct_t6 0.29.8).

How the T6 backend works. The protocol is in
[protocol-t6.md](../reference/protocol-t6.md), the limits and their measurements in
[t6-limits.md](../reference/t6-limits.md), the shared DRM side in
[shared-core.md](shared-core.md), the history of every decision in
[t6-lab-notebook.md](../history/t6-lab-notebook.md).

## Probe

Interface 0 of the device. Devices other than the tested `0711:5600` and
the untested `0711:5601` are refused with a log line unless the module parameter
`untested_ids` is set (since 0.29.4; the IDs are in `mct_main.c`). The Wacom
DTU-1031AX (`0711:5609`, or `056a:039f` on its vendor interface) gets only
its EDID's own mode (`t6_keep_preferred`), as j5create's macOS driver does. Reads which output has a monitor (`0x87`), the
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
  monitor's range, pixel clock and line rate: CVT reduced blanking (160 px
  horizontal blanking) where it fits, otherwise a tighter reduced blanking
  with as much horizontal blanking as fits, 152 down to 80 px (HDMI audio
  needs 112 px; 1080p144 gets 120 px);
- when the monitor's preferred mode is out of reach: the same size with
  reduced blanking at the highest rate that works, the highest rate where
  YUV 4:4:4 fits the scanout (a nearby standard rate becomes the default),
  or 2560x1440@60 for 16:9 monitors too tall for the adapter;
- without a usable EDID: the device's table up to 1080p.

`t6_mode_entry` turns a DRM mode into the device's 32-byte mode entry,
including the PLL settings (MCT's formula), and is the single check of what
the adapter can drive.

The converted image and the three frame buffer copies for the mode's size
are allocated by the core in the CRTC's `atomic_check` (since 0.6.0; a lack
of memory rejects the mode). `enable` (from the core, updates stopped):
takes those buffers over, sends set mode, "software ready" and output on,
forgets what the device's buffers hold. `disable`:
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
  (signature 3) and the PCM, until the device is 90 ms ahead of what it
  plays by the host clock (`audio_lead_ms`). It sends only what the application has written.
  When the application falls behind and the device has less than 30 ms
  left, it sends silence. The stream then plays that much later. A device
  that runs dry replays old contents of its ring buffer, which sounds like
  crackling rain.
- The ALSA position moves smoothly with the host clock: what the device
  plays now plus the lead. It never passes what was sent and never
  goes back. The reported delay is the lead. The PCM is marked "batch", so
  PipeWire keeps a period more queued. During a drain the last partial
  packet is padded with silence.
- Stop: the timer stops and the work sends Activity 0. `sync_stop` waits
  for the work before ALSA frees or resets the buffer.
- Unplug: the card is disconnected and the timer and work stopped before
  the DRM device goes. Suspend: streams are suspended and the timer and
  work stopped; the next start enables the engine again.
- debugfs `audio` line: starts, data packets, silence fills (underruns),
  audio events.
- Events with the audio function bit (0x20) are counted with their DMA
  pointer (debugfs `audio` line), to find out whether the adapter can
  pace playback itself.

Audio is heard only while the display is on (HDMI carries it in the video
signal). With the display off, playback runs but nothing is heard. It
needs at least 112 px of horizontal blanking, where HDMI carries audio
(tested at 1080p120: 104 px buzzes, 112 px is clean; the pixel clock
doesn't matter, 333 MHz with 280 px is clean). The driver's modes keep
that much where it fits under the adapter's 340 MHz; when a stream
starts in a mode with less, or with the display off, it logs it once per
mode. The lead
is the module parameter `audio_lead_ms` (default 90, 20-500, read at
stream start).

Since 0.29.3 the adapter's volume node is the mixer controls "PCM Playback
Volume" (stereo, 0-31, a linear scale from silence to 0 dB, as in
Windows' table) and "PCM Playback Switch". Both are read from the adapter
at probe (`a2`, `a3`); the adapter keeps them until it loses power. A
change sends `40 23`. Muting sends 0, since there is no mute node. The
volume is sent again on resume.

Since 0.29.5 the card also has the controls HDMI sound drivers offer:
"ELD" (PCM, read-only bytes: the monitor's audio capabilities from its
EDID, which DRM puts in `connector->eld`; PipeWire names the output after
the monitor) and "HDMI/DP,pcm=0 Jack" (on while a monitor with audio in its
EDID is connected). Both are updated on every EDID read and cleared when
the monitor goes, with ALSA change notifications.

## Cursor

The core hands over the cursor as pixels and a screen position. The backend
uploads the smallest of 64, 96 or 128 px that holds everything visible, in
4096-byte chunks, into a cursor slot not on screen, then selects it. Since
0.29.10 it keeps a copy of what each of its 5 slots holds: a shape seen
before (arrow, text beam, hand) is shown by selecting its slot, one small
request instead of an upload of up to 64 KiB (~11 ms, measured
2026-10-14). New shapes go into the least recently used slot; a mode set
or a redraw request forgets the slots.
Negative positions are sent as v - 1. A 128 px image is sent up to 0x10000
bytes (16-bit offset).

The cursor's control requests (move, show, image) use their own buffer
under `cur.lock`, not `io_lock` (since 0.29.8): they go over the control
endpoint while frames go over bulk, so a move doesn't wait for a frame
that is being sent. They come from the core's cursor worker, never from
inside a commit.

## Settings and information

Per device in sysfs (`/sys/bus/usb/devices/<interface>/mct/`, see
[ABI-sysfs-mct.txt](ABI-sysfs-mct.txt)): `format` and `all_modes` to set,
the module parameters of the same names as defaults for new devices;
counters, mode, pixel format, USB link, and what the device reported at
probe (platform, board, firmware, serial, feature level, video memory,
display flags) to read.
