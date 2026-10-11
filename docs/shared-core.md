# Shared core (2026-10-11)

**Done** (main, `kernel/mct_drv.c` / `mct_drv.h`), tested live on both
devices at once: T6 at 1080p144, tablet with all updates under 16 ms. The
API ended up close to the plan below; differences: chips get the shadow's
changed rows through `mdev->changed` and run their own worker body
(`funcs->update`), the settle timer is shared (T6 back to full colour, T2
lossless refinement), and chip-specific setup (DMA device, events) stays in
the chip's probe. Size at the refactor: core 670 lines, `mct_t2.c`
1550 -> 1270, `mct_t6.c` 2300 -> 1680 (the T6 grew again with 0.18 to 0.21).
Not done yet: the Wacom profile struct in `mct_t2.c`.

## The API as it is (`kernel/mct_drv.h`)

A chip backend embeds `struct mct_device` (the DRM device, planes, CRTC,
connector, the shadow image and its locks, workers and commit-side
statistics) and fills in:

- `struct mct_config`: hooks, min/max image size, connector and encoder
  type, cursor size (the T2 64 px, the T6 128 px), whether to poll the
  connector, whether to offer a gamma LUT, the debugfs file name.
- `struct mct_funcs`: `get_modes`, `mode_valid`, `detect`,
  `late_register` / `early_unregister` (the T2's backlight), `enable`
  (may allocate per-mode buffers, updates are stopped), `disable`, `stop`
  (wait for transfers), `update` (the worker body: send what changed),
  `gamma`, `cursor_set` (pixels as on screen, position may be negative),
  `cursor_hide`, `stats`.

The core calls `update` from its worker after commits copied changed rows
into the shadow (`mdev->changed` marks them), and runs a shared settle
timer (`mct_settle()`) the chips use to come back after motion. USB probe,
disconnect, suspend and resume go through `kernel/mct.h`'s `mct_backend`;
the core has `mct_init`, `mct_register`, `mct_unplug`, `mct_suspend`,
`mct_resume`, and `mct_shadow_alloc` for chips with a fixed size.

## The design as written before

(Proposed names differ from the code; the T6 rotation in the table below
was dropped, the 5600 has none, and cursors are per chip, up to 128 px.)


Today `mct.ko` is one USB driver with two backends, `mct_t2.c` (1550 lines)
and `mct_t6.c` (2300), each a full DRM driver of its own. About a third of
each file is the same DRM plumbing written twice. The goal: one DRM core
that does the KMS side once, and per-chip files that only speak their
chip's protocol, so a third chip (Trigger 5, a plain T2 dongle) is mostly a
protocol file.

## What is the same today

| Part | T2 | T6 | Shared? |
|---|---|---|---|
| drm_device, planes, CRTC, encoder, connector setup | fixed 1024x600, USB connector | EDID modes, HDMI connector, gamma, rotation | yes, with chip limits as data |
| Primary plane: shadow plane, "whole fb is the image" check | merged damage rect | per-row mask, exact compare for own buffers | yes: the T6 path (rows + exact compare) for both |
| Shadow + update worker, newest image wins | `upd` {work, lock, dirty}, snap copy | `upd` + settle work | yes: core owns shadow, lock, dirty rows, worker kick |
| Cursor plane: check, 64x64 ARGB extraction, hide/show | register lists on EP 4 | control requests, 2 slots | extraction yes; upload/move/show per chip |
| Vblank timer, commit tail (enables before planes) | default tail | custom tail | yes: the T6 tail for both |
| debugfs stats | `mct_t2_stats` | `mct_t6_stats` | common counters in core, chip lines appended |
| Probe: drm alloc, dma dev, fbdev client, suspend/resume | | | yes |
| Picture path: diff, encode, send, flip | RLE, lossy, refine, two buffers | conversion, bands, URB pool, three buffers, adaptive format | no: per chip |
| Extras | backlight via pen HID, init blob, resync | hotplug events, PLL modes, rotation | no: per chip |

## Proposed split

```
mct_main.c   USB driver, ID table -> chip (as now)
mct_drv.c    DRM core: device, planes, CRTC, connector, shadow, worker, cursor
             extraction, stats, suspend/resume
mct_drv.h    struct mct_device, struct mct_chip_funcs
mct_t2.c     Trigger II protocol (+ Wacom DTU-1031X quirks as a profile)
mct_t6.c     Trigger 6 protocol
```

```c
struct mct_chip_funcs {
	/* modes */
	int (*get_modes)(struct mct_device *, struct drm_connector *);
	enum drm_mode_status (*mode_valid)(struct mct_device *, const struct drm_display_mode *);
	int (*detect)(struct mct_device *);			/* connector status */
	/* display on/off and mode, rotation 0..3 */
	int (*enable)(struct mct_device *, const struct drm_display_mode *, unsigned int rot);
	void (*disable)(struct mct_device *);
	/* send the shadow's changed rows; called from the core's worker */
	int (*present)(struct mct_device *, const u8 *dirty_rows);
	/* cursor: 64x64 ARGB in image coordinates, already mapped */
	int (*cursor_set)(struct mct_device *, const u8 *argb, int x, int y);
	void (*cursor_hide)(struct mct_device *);
	void (*gamma)(struct mct_device *, const struct drm_color_lut *);	/* optional */
	void (*stats)(struct mct_device *, struct seq_file *);		/* optional */
	int (*suspend)(struct mct_device *);
	int (*resume)(struct mct_device *);			/* re-init the chip */
};

struct mct_chip {				/* static data per chip */
	const char *name;			/* "mct_t2", "mct_t6": DRM driver name stays */
	int connector_type, cursor_size;
	unsigned int rotations;			/* DRM_MODE_ROTATE_* the chip can do */
	bool gamma;
	const struct mct_chip_funcs *funcs;
};
```

Backends embed `struct mct_device` and keep their own state next to it.

## Rules for doing it

- Behaviour stays the same on both devices: same picture path, same
  parameters (`lossy`, `format`), same debugfs numbers. Two steps, each
  tested on both devices before the next: (1) move the T6 onto the core
  (it already has the more general code), (2) move the T2 onto it.
- The DRM driver names (`mct_t2`, `mct_t6`) and the stats file names stay,
  so nothing in userspace or the docs changes.
- Wacom specifics in `mct_t2.c` (panel size, 180 degree scanout, init blob,
  pen HID backlight, IDs) go into a "DTU-1031X profile" struct, so a plain
  T2 dongle later is a second profile with EDID modes.
- Per-chip parts that look alike but aren't (two vs three buffers, RLE vs
  raw bands, lossy vs adaptive format) stay in the chip files; sharing them
  would add switches, not remove code.

## Expected size

Roughly 700-800 lines move into `mct_drv.c`; each chip file loses 400-600.
Total code shrinks by about 500 lines, and the T2 gets the T6's better
commit path (exact row compare, rotation-safe damage) for free.
