/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * DRM side shared by the chip backends: one primary plane fed from a shadow
 * copy of the committed image, a cursor plane (size per chip), one CRTC
 * paced by a vblank timer, one connector. The chip code gets the shadow's changed rows
 * in its update worker and the cursor as plain pixels, and talks to its
 * device.
 */
#ifndef MCT_DRV_H
#define MCT_DRV_H

#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_device.h>
#include <drm/drm_encoder.h>
#include <drm/drm_plane.h>

struct seq_file;
struct usb_interface;
struct drm_color_lut;
struct mct_device;

struct mct_funcs {
	/* monitor and modes */
	int (*get_modes)(struct mct_device *mdev, struct drm_connector *conn);
	enum drm_mode_status (*mode_valid)(struct mct_device *mdev,
					   const struct drm_display_mode *mode);
	int (*detect)(struct mct_device *mdev);			/* optional */
	int (*late_register)(struct mct_device *mdev);		/* optional */
	void (*early_unregister)(struct mct_device *mdev);	/* optional */

	/*
	 * Display on in this mode / off. Updates are stopped before disable and
	 * while enable runs, so enable can (re)allocate per-mode buffers;
	 * returns -ENOMEM if it couldn't.
	 */
	int (*enable)(struct mct_device *mdev, const struct drm_display_mode *mode);
	void (*disable)(struct mct_device *mdev);
	/* wait for transfers in flight when updates stop (optional) */
	void (*stop)(struct mct_device *mdev);
	/* the update worker: send what changed in the shadow */
	void (*update)(struct mct_device *mdev);
	/* gamma LUT (256 entries) or NULL for linear; optional */
	void (*gamma)(struct mct_device *mdev, const struct drm_color_lut *lut);

	/*
	 * Cursor: cursor_size x cursor_size pixels B, G, R, A as on the
	 * screen, rows cursor_size * 4 bytes apart (transparent outside the
	 * framebuffer), (x, y) = the box's top left on the screen, may be
	 * negative.
	 */
	void (*cursor_set)(struct mct_device *mdev, const u8 *bgra, int x, int y);
	void (*cursor_hide)(struct mct_device *mdev);

	void (*stats)(struct mct_device *mdev, struct seq_file *m);	/* optional */
};

struct mct_config {
	const struct mct_funcs *funcs;
	int min_w, min_h, max_w, max_h;	/* image sizes */
	int connector_type, encoder_type;
	int cursor_size;		/* square hardware cursor, pixels */
	bool poll;			/* monitor can change: poll the connector */
	bool gamma;			/* offer a 256-entry gamma LUT */
	const char *stats_name;		/* debugfs file */
};

struct mct_device {
	struct drm_device dev;
	struct drm_plane primary;
	struct drm_plane cursor;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector conn;
	const struct mct_funcs *funcs;

	struct mutex lock;		/* the fields below, up to work */
	u8 *shadow;			/* XRGB8888 image as committed by userspace */
	size_t shadow_size;		/* allocated for the current mode */
	bool shadow_valid;		/* holds a whole committed image */
	u8 *rowmask;			/* rows to copy from the framebuffer */
	u8 *changed;			/* shadow rows changed, cleared by the chip code */
	bool dirty;			/* shadow changed since the worker last ran */
	ktime_t dirty_since;		/* first change not yet sent, 0 if none */
	int width, height;		/* current image, 0 while off */
	int refresh;

	struct work_struct work;	/* runs funcs->update */
	struct delayed_work settle_work;	/* marks dirty and runs the worker */
	u8 *cur;			/* cursor pixels for funcs->cursor_set */
	int cur_size;

	struct {			/* debugfs statistics of the commit side */
		u64 skipped;			/* images replaced before they went out */
		u64 px_damage, rows_copied;
		u64 no_clips, clips, clip_px;	/* damage clips per commit */
		u64 copies, ns_copy, ns_copy_max;	/* framebuffer -> shadow */
		u64 ns_commit, ns_commit_max;		/* the same incl. CPU access setup */
		ktime_t since;
	} stats;
};

#define to_mct(d) container_of(d, struct mct_device, dev)

int mct_init(struct mct_device *mdev, const struct mct_config *cfg);
/* shadow for a w x h image; done at enable, or earlier for a fixed size */
int mct_shadow_alloc(struct mct_device *mdev, int w, int h);
int mct_register(struct mct_device *mdev, struct usb_interface *intf);
void mct_unplug(struct mct_device *mdev);
int mct_suspend(struct mct_device *mdev);
int mct_resume(struct mct_device *mdev);

/* run the update worker; mark the shadow dirty first (under mdev->lock) */
void mct_kick(struct mct_device *mdev);
/* mark dirty and run the worker after ms, unless called again before */
void mct_settle(struct mct_device *mdev, unsigned int ms);

#endif
