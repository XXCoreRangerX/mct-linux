# Project history

This file preserves the chronological development record and the major
successful and unsuccessful findings. It was created when the old, untracked
workspace `HANDOFF.md` was replaced with a current handoff on 2026-09-25. The
old handoff was read before replacement and its substantive historical details
are retained here. (Commit IDs quoted below are those of the development
repository; the published history has the same commits under other IDs.)
Git remains the exact source for every tracked code change;
the specialized documents linked below contain the byte-level evidence.

## Before the Git repository: protocol recovery and first light

The project began by reverse-engineering the Windows Trigger Family v25.01
driver, its `T2Indisp.dll`, and usbredir captures from a Windows VM. The target
was the Wacom DTU-1031X, whose display function is an MCT Trigger II T2-285A
(`0711:520d`) and whose separate pen function (`056a:032f`) already worked on
Linux. (On the development machine the pen is driven by the owner's own
OpenTabletDriver build through hidraw, not by the kernel's `wacom` driver; the
display driver must never depend on which pen driver is used.)

### Capture method

The first capture used usbmon on the host while a Win11 VM ran the vendor
driver with the tablet passed through. Its text interface truncates every
transfer after about 32 bytes, so it showed command headers but no payloads.
The replacement was a TCP tee between QEMU's `usb-redir` device and a locally
built `usbredirserver` (`tools/tee.py`, `tools/parse_redir.py`), which records
every byte in both directions. `captures/redir2.bin` (init plus black frames)
came from this setup.

### The "backlit but blank" period

Replaying the captured init lit the backlight but no fill or frame ever
appeared. Two independent mistakes caused it: headers were concatenated with
their payloads into single writes (the firmware parses per USB transfer), and
the `0x15` flip after each frame was missing. The replay generator also dropped
`0x13` headers but still sent their payloads, which desynchronised the frame
stream further. A verbatim, transfer-preserving replay followed by solid-colour
RLE frames and flips produced the first image.

The decisive first-light findings on 2026-09-24 were:

- EP2 command headers must be sent as their own USB transfers. Joining a
  `0x11` or `0x13` header to its payload is silently ineffective.
- Windows divides `0x13` payloads into 19,968-byte transfers and `0x11`
  payloads into 16,384-byte transfers.
- A framebuffer update needs the `0x15` register batch/buffer flip. The T2 has
  two relevant framebuffer bases, A at `0x0000c000` and B at `0x001ce000`.
- The `0x13` codec is row-wise planar R, G, B RLE. `30 NN VV` represents
  `NN+1` copies of `VV`; ordinary bytes are literal pixels and literal `0x30`
  is represented as `0x31`. The encoder was read out of `T2Indisp.dll`
  (function at `0x180013278`, called from `EncodeRleDTR`), and
  `tools/t2rle.py` reproduced captured Windows payloads byte-for-byte.
- The panel's device coordinates are rotated 180 degrees. This showed up when
  a decoded Windows desktop frame came out as the Windows wallpaper upside
  down; a corner test pattern on the panel confirmed it.
- Partial update addresses are based on `buffer + y*3072 + x*3` and sent in
  the command shifted by eight bits. A partial rect's pixels are streamed
  row-major and cut into 1024-pixel units, so `w*h` must be a multiple of 1024;
  all 172 frames in the desktop capture decode under that rule.
- The hardware cursor shape uses command `0x12`; position and visibility are
  controlled by an EP4 register list.

`tools/show_image.py` then displayed photos and test patterns without Windows.
`captures/redir2.bin` became the principal initialization capture, while
`captures/redir4.bin` recorded real partial updates and hardware cursor
traffic. The obsolete blank-screen hypotheses were retained as research
history in `docs/first-light.md`, not as current instructions.

## First usable display paths

The first continuous desktop used the userspace `t2d` daemon on EVDI. Two
outside pieces made that possible: EVDI 1.14.x did not build on Linux 7.2
(`struct drm_atomic_state` was renamed `drm_atomic_commit`), fixed by a small
rename patch (EVDI 1.15.1 later built as is), and niri needed its PR #2312
(released in 26.04) to render display-only DRM devices on the primary GPU.
Niri recognized the tablet as a 1024x600@60 output and damage-driven updates
were smooth. EVDI remained valuable for quick protocol work, but it could not expose
all hardware features cleanly.

The native `mct_t2` DRM/KMS driver followed the same day, modelled after small
USB DRM drivers. It provided a normal connector to Wayland, X11 and fbcon and
became the preferred implementation. The initial repository import also
preserved the capture tooling, Python codec and replay utilities, EVDI daemon,
Nix packages and protocol documentation.

## Native-driver feature milestones

### Display correctness and latency

- Damage clips and partial updates replaced unconditional whole-screen work.
- Initialization was reduced to 78 transfers and about one second from a cold
  device, while retaining the required mode, timing, memory-controller and
  display-enable operations. It was found by bisection with a distinct colour
  per variant: dropping reads, the ID-ROM query, a probe fill, duplicated
  blocks and the two 1.8 MB black clears was harmless, but the bank-FB buffer
  address writes (`fb60/6e/65/62..64`) are required (without them a garbage
  band appears at the bottom). Only a physical replug resets the chip's 8051,
  so every variant was validated cold: with no pacing a cold chip came up as a
  monochrome grid; 20 ms after every write worked (1.8 s); the final selective
  pacing (2 ms after register writes, 50 ms after `feb0`/`feb1`, 20 ms after
  other commands) takes about one second. Because `0x14` switches scanout to
  buffer A, a black frame is written into A just before it.
- A DRM hardware cursor plane used the T2's 64x64 ARGB overlay. Shape uploads
  occur only when needed; moves are small EP4 register writes.
- DPMS was implemented through pixel-clock PLL register `fcf6`. The vendor
  driver offers nothing to copy here: its `CT2RomSink` display-power method is
  a stub (`xor eax,eax; ret`). `fb60` (also via `0x14`), `fe36`, `fe57`, `fe70`
  and `feb0` were tried live without effect; stopping the pixel clock makes the
  panel electronics switch the backlight off, and re-enabling it restores the
  picture without re-init.
- Hardware cursor details were settled live: the position registers give the
  device-space top-left of the 64x64 box, negative values hide the cursor (so
  boxes hanging off the visible right/bottom edge are uploaded shifted), and
  `0x44`/`0x45` trim the box from the far end rather than offsetting the image.
- A 60 Hz DRM vblank timer stopped compositors from rendering unthrottled.
- The update strategy evolved to compare against the buffer from two updates
  earlier, group changed rows into bands, write only those areas into the back
  buffer, and flip. Always using the back buffer avoided visible tearing and
  felt better than the Windows driver's direct-front-buffer policy.

### Integration and lifecycle

- Nix packages, a NixOS module, console mapping and a reload helper were added.
- A niri patch works around niri 26.04 retaining a removed DRM device after
  unbind/hot-unplug. Root cause: `device_removed()` calls
  `DrmNode::from_dev_id()`, which reads sysfs entries that are already gone,
  logs "error creating DrmNode" and returns early; the fix looks the node up
  among niri's tracked devices. An upstream report draft exists outside the
  repository.
- The NixOS `uaccess` rule first had no effect: `services.udev.extraRules` lands
  in `99-local.rules`, after `73-seat-late.rules` applies uaccess. It is now
  shipped as `70-mct-t2.rules` (later renamed `70-mct.rules`).
- `soft_unbind` prevents a normal software unbind from cutting a frame in half.
- Deep suspend/reset-resume was verified with picture, cursor and backlight
  restored.
- The tablet was observed enumerating as both `0711:520d` and `0711:5209`; the
  Windows INF maps both to the same driver and both are supported. The journal
  showed 20 enumerations as `520d` over three days, then `5209`, `5209`, `520d`
  on three consecutive replugs. The ID ROM, which holds `0711:5200`, was
  unchanged byte for byte; the low bits are decided at power-up for reasons
  still unknown. The same replug exposed that the pen function can enumerate
  after the display, so the backlight looks the pen up on every access.
- Brightness took two attempts. First the display side: `CT2I2CBus` in the DLL
  turned out to drive two OpenCores-style I2C masters (`fce0..fce4`,
  `fce8..fcec`); `0x1b`/`0x1d` are block reads through them, bus 1 holds only
  the ID-ROM EEPROM at `0x57` (panel EDID at +0x80), and bus 0 is not wired
  (a START never completes). So there is no DDC/CI path. The Wacom manual then
  pointed at Wacom's own control panel, and a capture of Wacom's Windows
  driver 6.4.9-2 in the VM (pen function only, redirected on an xHCI bus)
  showed HID feature report `0x06`, levels 1-7, on the pen. Levels 0 and 8 were
  found afterwards to be real, darker and brighter steps. The DRM connector
  owns a Linux backlight device with off plus all nine firmware levels; being a
  child of the connector is what lets shells such as Noctalia match it to the
  right output. Level zero also stops the T2 pixel clock.

### Measurement and performance work

- Debugfs counters measure updates, damaged and transmitted pixels, USB bytes,
  and time spent diffing, encoding and transferring.
- A desktop sample sent only 16% of compositor-reported damage at 0.57 MB/s.
  A video sample sent 93% at 16.3 MB/s.
- Video-stage measurements found about 2.3 ms/update in encoding and about
  24 ms/update in USB. Diffing was negligible; the T2 input path is the main
  limit.
- Transfer benchmarks measured about 40.1 MB/s with vendor-sized 19,968-byte
  chunks, 41.2 MB/s at 64 KiB and 41.6 MB/s at 256 KiB. Very large writes
  corrupted frame endings, so the small theoretical gain was not adopted.
- An eight-URB asynchronous/striped implementation was tested on branch
  `async-urbs`. It achieved 28.0 ms rather than 26.4 ms per comparable update
  and was not merged.
- Optional budget-triggered lossy RLE was added. With tolerance four, heavy
  video fell from roughly 26 ms to 17 ms/update and idle refinement restored an
  exact image. An attempted "lossy streak" shortcut skipped the first lossless
  encode during motion but produced settling shadows and horizontal/CRT-like
  artifacts; it was reverted and must not be restored unchanged.

- The lossy mode's first trigger (more than a third of the screen changed)
  made 83% of desktop updates lossy and caused "settling" shadows when windows
  opened; the budget rule (lossless first, lossy only past ~600 KB per frame)
  replaced it.

## Process notes

- Experimental work was done on branches and merged only after the tablet
  owner tested it; merged branches (`diff-updates`, `robust-detach`, `stats`,
  `perf`, `lossy-auto`, `lossy-streak`) were deleted, their commits remain in
  `main`. `async-urbs` is kept unmerged for reference.
- Reverting the lossy streak hit a version-line conflict, and the conflict
  markers were committed into `df093e9` by a chained command. `20f961c`
  resolved them; no code was lost. Since then conflicting Git operations are
  run and checked one at a time.

## The driver-swap corruption and 0.3.13

Version 0.3.12 still had a rare failure: swapping drivers while a large frame
was in flight could leave rainbow garbage, although another reload fixed it.
The first occurrences (before `soft_unbind`) logged a cancelled transfer
(`-2`) and once an init timeout (`-110`) that needed a replug. Later ones
happened with `soft_unbind` in place and with every frame error logged, yet no
error appeared; the stuck-parser mechanism below was demonstrated
deliberately, and 0.3.13 recovers from it, but whether it explains those
error-free cases was inferred rather than observed.
The publication gate was to learn whether the chip could reveal that its frame
stream was out of step and then recover automatically.

Live experiments deliberately sent a `0x13` header followed by only half of its
payload. The firmware then timed out on all EP3 command reads because it handles
one command at a time and was still waiting for EP2 data. Supplying the missing
payload as zeros released the queued read, but overshoot could itself be parsed
as commands. A normal initialization after padding reliably realigned the
parser and restored the display.

The resulting 0.3.13 recovery sequence drains stale replies, probes `feb0` with
a 200 ms timeout, sends zero padding in 19,968-byte chunks if the probe stalls,
runs initialization, and verifies `feb0=0x40` and `fcf6=0x01`, retrying init
once on a bad verification. A deliberate live half-frame recovered after
938,496 bytes and bound as a correct connected DRM display.

Further status-channel research found:

- Two idle sweeps of all 768 readable FB/FC/FE registers differed only at
  `fc57` (`0x00`/`0x10`), so it is not a dependable parser-state flag.
- A stuck sweep cannot advance beyond its first read.
- The DLL's previously missed `CT2Hal::ReadMC8032P1` command is
  `29 00 00 00` with a 512-byte reply whose first byte is used. It replies
  `0f 00...` when idle but also queues behind a partial framebuffer command.
- EP85 produced no data during five idle and five deliberately stuck probes;
  the Windows capture contains no EP85 or EP6 traffic.
- No framebuffer readback command was found. The command timeout is therefore
  the useful detector, not a special status register.

## Work intentionally still open

(As of 2026-09-25; since then the counters, adaptive lossy tolerance and the
recovery regression test were done. The current list is in roadmap.md.)

- Soak-test 0.3.13 across daily use, reload, deliberate interruption,
  unplug/replug, suspend/resume, DPMS, brightness, cursor, console and lossy
  video.
- Replace the untracked, unsafe `tools/desync_lab.py` scratch file with a
  cleanup-aware recovery regression test if its reproducer is retained.
- Add latency distributions and payload/over-budget counters before tuning.
- Evaluate adaptive lossy tolerance without reviving the failed streak logic.
- Test whether the 768-byte `0x0a` operation is a 256xRGB hardware gamma table.
  Its size is only a clue; gamma semantics are not yet established.
- Consider cost-based band merging. Expected gains are modest because desktop
  traffic is already small.
- Introduce device profiles gradually when a second physical MCT product gives
  evidence about what actually varies.

Do not prioritize RGB565, arbitrary panel modes, direct front-buffer writes,
diff-loop micro-optimization or another async-URB implementation without new
evidence.

## Complete tracked commit ledger through 0.3.13

This ledger includes experimental commits visible in the repository's branches;
Git records the exact diffs and parent relationships.

| Commit | Change |
|---|---|
| `53f6349` | Initial import: protocol research, captures, tools, EVDI daemon and packaging |
| `d5f161c` | Add native `mct_t2` DRM driver |
| `fcdbbd6` | Damage clips, documentation and NixOS driver selection |
| `b48222a` | Decode hardware cursor shape and position protocol |
| `eb7246b` | Trim init and add hardware cursor plane |
| `50ef4b4` | Kernel DPMS, vblank timer and documentation |
| `4d4084b` | EVDI DPMS and non-flake NixOS entry point |
| `bd9aa17` | Documentation wording/ASCII cleanup |
| `10b5917` | Credits, related work, mainline status and documentation license |
| `7b4f743` | README current-state cleanup |
| `04ba356` | Throughput measurements and 19,968-byte transfer decision |
| `543f8ba` | NixOS `consoleMap` option |
| `e3dcf05` | Document EVDI 1.15.1 Linux 7.2 build status |
| `303e286` | Kernel checkpatch cleanup |
| `804f73e` | Verify vblank; document hotplug, niri bug and console behavior |
| `4264f14` | Map both T2 I2C masters and rule out panel DDC/CI |
| `3ae2ae8` | Optional niri removed-device patch |
| `e18d03a` | Backlight via Wacom pen HID report |
| `8856337` | Package version 0.3.0 |
| `a1127b9` | Support USB ID `0711:5209` |
| `c32365a` | Late pen enumeration handling and varying-ID documentation |
| `50ee52c` | Expose all backlight levels |
| `f44772d` | Parent backlight device to DRM connector |
| `4270f66` | Module reload helper and Noctalia verification |
| `3cff0d6` | Make brightness zero turn the panel off |
| `677acdf` | Document wake delay after DPMS/backlight off |
| `d9bf2cc` | Send only changed pixels with double buffering |
| `2bb37e3` | Document diff-based update strategy |
| `9696bdd` | Add `soft_unbind` frame-completion behavior |
| `a492c59` | Add debugfs traffic statistics |
| `05438d2` | Record diff-strategy bandwidth measurements |
| `51d46fc` | Record verified suspend/resume |
| `1fc978f` | Add diff/encode/USB stage timing |
| `697282e` | Record timing and initial async assessment |
| `80ba581` | Experimental async transfers/striped encoding (`async-urbs`) |
| `bcd50a0` | Record that async URBs did not improve throughput |
| `4ba96d3` | Add optional lossy motion updates and lossless refinement |
| `12ae684` | Trigger lossy mode only after exceeding frame budget |
| `e406569` | Add NixOS lossy option and documentation |
| `0ccbbd7` | Correctly pass the NixOS lossy option to the module |
| `baae411` | Experimental lossy-streak shortcut |
| `5c75e59` | Serialize refinement shutdown and report all frame errors |
| `df093e9` | Version 0.3.12 docs, rejected streak and driver-swap glitch |
| `20f961c` | Resolve remnants of the lossy-streak revert |
| `446bc0e` | Install uaccess rule as `70-mct-t2.rules` |
| `7920b94` | Version 0.3.13 parser recovery implementation |
| `e2bbd39` | Document parser-status experiments |
| `024dd06` | Record performance research backlog |
| `099d6fb` | Add development roadmap |
| `0e8b44a` | Add this project history |

## Detailed source records

- `docs/first-light.md`: early failures, resolved hypotheses and first image
- `docs/protocol-t2.md`: command framing, RLE, buffers, status experiments,
  cursor, power, brightness and throughput
- `docs/init-sequence.md`: initialization phases and exact packet roles
- `docs/driver-analysis.md`: Windows stack and DLL reverse engineering
- `docs/device-dtu1031x.md`: hardware topology, EDID and register facts
- `docs/display-driver.md`: the earlier EVDI implementation
- `docs/kernel-driver.md`: the T2 backend's design, tests and roadmap (the
  shared core: `docs/shared-core.md`; the T6: `docs/t6-limits.md`,
  `docs/t6-lab-notebook.md`)
- `docs/capture.md`: Windows VM and usbredir capture procedure

Together with Git, these are the durable historical record. `HANDOFF.md` (in
the lab directory, not in this repository) is a current operational summary
and may be rewritten again without serving as the sole archive.
