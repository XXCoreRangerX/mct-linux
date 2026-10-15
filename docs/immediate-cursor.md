# The immediate cursor option

> **Reference** (2026-10-15): why niri can be built with an "immediate cursor" patch, what it changes, and why it is an option rather than the default. Measurements and background: [research/cursor-latency.md](research/cursor-latency.md).

## What it does

With `hardware.mct.niriFix` and `hardware.mct.niriImmediateCursor` on
NixOS, niri (and smithay, the library it is built on) are patched so that a
mouse move goes to the hardware cursor at once, through the kernel's
legacy cursor call, instead of waiting for niri's next frame. Everything
else stays as before. `NIRI_IMMEDIATE_CURSOR=0` in niri's environment turns
it off without rebuilding.

The effect, measured on the Trigger 6 with a 1000 Hz mouse at 60 Hz: a move
reaches the adapter in about 0.4 ms instead of 9 ms typical and 17 ms at
worst, the same as X11 and MCT's Windows driver.

## Why a cursor waits on Wayland

Wayland compositors drive displays with **atomic modesetting**: each commit
is one complete frame, with the picture, the cursor and every other plane
changing together at the next refresh. That is what keeps Wayland desktops
free of tearing and keeps the cursor in step with what is under it. The
kernel accepts only one pending frame per display, so a mouse move that
comes while a frame is pending waits for that frame, up to one refresh.

On a graphics card that costs at most one refresh (about 4 ms at 240 Hz).
On a USB display it costs more: the display has no refresh signal the
computer can see, so mct paces frames with a timer, and the adapter then
shows them on its own clock. A move could wait for both, and USB displays
often run at 60 Hz.

X11 and Windows don't have this wait. X11 moves the cursor with the
**legacy cursor call** (`drmModeMoveCursor()`), which the kernel treats as
unsynchronised on purpose; Windows moves its hardware cursor as soon as
the pointer moves.

## Why the legacy call

The atomic interface has no way to move a cursor between frames. Its
"async" mode only allows swapping a plane's image, not moving the plane.
Adding cursor moves to it has been proposed in the kernel more than once
(2018, 2019) and not merged; the open questions are what such a move means
while a frame is pending, how it interacts with variable refresh rate, and
how each GPU applies cursor registers. A kernel interface can't be changed
once programs rely on it, so these questions have to be settled first.

KWin takes another route: it commits shortly before each refresh deadline
with the newest cursor position. On a graphics card that comes close to
the legacy call. On a USB display there is no real deadline to aim for
(mct's is a timer, and the adapter refreshes on its own), so it would still
hold moves for part of a frame.

That leaves the legacy call as the one way to move the cursor at once
today. The kernel keeps it working because X11 depends on it, and drivers
can apply such moves immediately by supporting asynchronous cursor updates:
amdgpu and i915 do, and mct does since mct_t6 0.29.15 / mct_t2 0.3.42.

## Why it is an option, not the default

- **It mixes two interfaces.** The patched niri uses atomic commits for
  everything and the legacy call only for pure cursor moves. That works and
  is supported by the kernel, but it is not how Wayland compositors are
  normally written, and upstream niri and smithay don't do it.
- **The cursor can run ahead of the picture.** While you drag a window, the
  cursor moves at once and the window with the next frame, so the cursor
  can lead it by a few milliseconds. Atomic commits were designed to keep
  them together; this option chooses responsiveness instead, as X11 and
  Windows do.
- **It depends on the driver.** On a driver without asynchronous cursor
  updates the legacy call waits for the frame in flight. The patch notices
  that (moves taking over 2 ms) and turns itself off, but such a driver gets
  nothing from it.
- **It is a patch to someone else's software.** It is built against niri
  26.04 and the smithay revision that niri uses, and has to be carried
  along with new niri releases.

## What is on where

| Part | Default | How to change it |
|---|---|---|
| mct accepts cursor moves between frames (async cursor plane) | always on | nothing to change: it only affects programs that use the legacy call (X11, the patched niri) |
| mct sends no update for commits that change nothing | always on | nothing to change |
| the niri and smithay patches | off | `hardware.mct.niriFix = true; hardware.mct.niriImmediateCursor = true;` |
| the fast path in the patched niri | on | `NIRI_IMMEDIATE_CURSOR=0` in niri's environment |

The patches: [`nix/niri-immediate-cursor.patch`](../nix/niri-immediate-cursor.patch),
[`nix/smithay-immediate-cursor.patch`](../nix/smithay-immediate-cursor.patch),
built by [`nix/niri.nix`](../nix/niri.nix).
