# Cursor latency: X11, Wayland and the legacy cursor call

> **Research and work in progress** (2026-10-15, final VM measurement added the same day): why a mouse move takes longer to reach a USB display under Wayland compositors than under X11 or MCT's Windows driver, what mct does about it, and the compositor fix being built.

## In short

- Under X11, and with MCT's Windows driver, a mouse move reaches the T6 in
  about 0.4-0.5 ms.
- Under Wayland compositors (niri, sway) it waits for the next frame: about
  9 ms typical and 17 ms worst case at 60 Hz with a fast mouse, and more
  while a video plays.
- The time isn't lost in the driver or on USB (0.3 ms). It's lost in how
  Wayland compositors commit the cursor: as part of a whole frame, and only
  one frame can be pending.
- The fix: the compositor moves the hardware cursor between frames with the
  kernel's legacy cursor call, the way X11 does, and the driver applies such
  moves at once. mct does its part since mct_t6 0.29.15 / mct_t2 0.3.42;
  the compositor part (smithay, used by niri) is being built.

## Measured

T6 driving a 1080p60 monitor. Every driver ran in a VM, all seeing the
device through the same USB redirection, with mouse moves injected the
same way. The table shows the time from a move to the cursor position on
USB, median / p95, in milliseconds. "1000/s" is a gaming mouse; "video"
means moving while a fullscreen 1080p50 video plays.

| | 1000/s, desktop | 1000/s, video | 250/s, desktop |
|---|---|---|---|
| MCT Windows driver | 0.39 / 0.48 | 0.51 / 5.7 | 0.49 / 0.70 |
| X11 + i3, mct | 0.42 / 0.5 | 1.45 / 13.9 | 0.50 / 0.8 |
| niri, mct | 9.1 / 16.5 | 13.5 / 21.1 | 9.1 / 16.6 |
| sway, mct | 9.2 / 16.7 | 27.6 / 46.6 | 9.1 / 16.5 |
| niri with a quick test patch that moves the cursor between frames, mct with async cursor moves | 0.42 / 14.2 | 9.0 / 16.0 | 0.60 / 9.1 |
| **niri with the immediate cursor patches (below), mct 0.29.15** | **0.44 / 0.50** | 8.9 / 16.1 | **0.62 / 0.93** |
| the same niri with the fast path off (`NIRI_IMMEDIATE_CURSOR=0`) | 9.3 / 16.9 | 14.0 / 21.5 | 9.1 / 16.7 |

With the patches, niri on an idle desktop matches X11 and MCT's Windows
driver, worst cases included: every one of 6000 moves at 1000/s went out
on its own, none waited for a frame.

While a video plays, it doesn't help yet in this test. The moves do go out
between frames, but they reach niri in bursts: in the VM, niri renders the
video in software on its main thread, and input waits while it does (6400
cursor moves in the 1000/s case, 4900 of them replaced by a newer one
before going out). X11 in the same VM shows the same pattern. On real
hardware, niri renders on the GPU and its main thread is far less busy, so
this case still has to be measured outside the VM. sway with wlroots' legacy (non-atomic) backend measured
the same as plain sway: it also moves the cursor only with each frame.

The method, the safety measures and all the runs are in
[vendor-vm-comparison.md](vendor-vm-comparison.md) and the lab notebook
([t6-lab-notebook.md](../history/t6-lab-notebook.md), 2026-10-15).

## Why Wayland compositors wait

Modern compositors drive the display with **atomic modesetting**: each
commit is one complete frame, with the picture, the cursor and every other
plane changing together at the next refresh. That is what makes Wayland
desktops tear-free. The kernel allows only one pending commit per display;
a compositor has to wait for the previous commit's flip event before it can
commit again (the kernel answers "busy with a previous commit" otherwise).
The cursor is part of the frame, so a mouse move that comes while a frame
is pending waits for that frame to complete.

On a normal graphics card this costs up to one frame, which is the familiar
"cursor feels heavier than on X11". On a USB display it's worse:

- The display has no refresh signal the computer can see. mct paces flip
  events with a timer at the refresh rate, and the adapter then shows the
  frame at its own refresh, so a move can wait for both.
- USB displays often run at 60 Hz, where one frame is 16.7 ms.

## Why X11 doesn't

X11 moves the hardware cursor with the **legacy cursor call**
(`DRM_IOCTL_MODE_CURSOR`, `drmModeMoveCursor()`), separately from the
picture. The kernel treats these moves as unsynchronised on purpose; its
source says "Legacy cursor ioctls are completely unsynced, and userspace
relies on that". A driver can apply them at once, without waiting for a
frame in flight, by supporting asynchronous plane updates on its cursor
plane, as amdgpu, i915 and others do.

The atomic interface has no such path. Its "async" mode
(`DRM_MODE_PAGE_FLIP_ASYNC`, for games that accept tearing) only allows
swapping a plane's framebuffer, not moving a plane. So a compositor that
wants to move the cursor between frames has to use the legacy call.

## The fix

**In the driver (done).** Since mct_t6 0.29.15 / mct_t2 0.3.42, mct's cursor
plane has `atomic_async_check` / `atomic_async_update`. A legacy cursor
move of the cursor already shown goes out over USB at once, even while a
frame is in flight. Everything else (a new cursor image, hiding it) still
goes the normal way. debugfs counts these moves ("moves between frames").
Since mct_t6 0.29.14 / mct_t2 0.3.41, cursor-only commits no longer send an
empty flip to the device.

**In the compositor (being built).** For smithay, and through it niri:

1. When only the pointer moved, and the cursor is on the hardware cursor
   plane with an unchanged image, move the plane at once with the legacy
   call, from the position of the last frame plus the pointer's movement.
2. Keep everything else on atomic commits; the next frame carries the same
   position anyway.
3. The kernel can't tell a compositor whether a driver applies legacy moves
   at once. On a driver that doesn't, the call can block for up to a frame,
   so the fast path watches how long each move takes and turns itself off
   for that display if moves block.
4. Turn it off where it could interfere: variable refresh rate, the cursor
   crossing to another output, rotated outputs until tested.

The patches are in this repository:
[`nix/smithay-immediate-cursor.patch`](../../nix/smithay-immediate-cursor.patch)
adds `DrmCompositor::move_cursor_immediately()` to smithay, and
[`nix/niri-immediate-cursor.patch`](../../nix/niri-immediate-cursor.patch)
calls it from niri on every pointer and tablet motion (niri 26.04, which
uses smithay at `ff5fa7df`). On NixOS, the module's `niriFix` and
`niriImmediateCursor` options build niri with them; `NIRI_IMMEDIATE_CURSOR=0`
turns the fast path off at run time. They will be offered upstream once
tested. Other Wayland compositors
(wlroots for sway and Hyprland, mutter, KWin) would need their own version
of step 1; mutter and KWin weren't measured here.

## Status

| Part | State |
|---|---|
| mct: no empty flips for cursor-only commits | ✅ mct_t6 0.29.14 / mct_t2 0.3.41 |
| mct: cursor moves between frames (async cursor plane) | ✅ mct_t6 0.29.15 / mct_t2 0.3.42; a prototype measured in the VM |
| smithay / niri fast path | ✅ measured in the VM: desktop at the Windows and X11 level; video case to be measured on real hardware |
| Measured with the final patches | T6 in the VM done; the tablet and real hardware (video) to do |
