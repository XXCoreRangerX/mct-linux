// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Filip Piękoś <mail@filippiekos.com>
 *
 * DRM core shared by the MCT chip backends (see mct_drv.h).
 *
 * Commits copy the damaged rows of the framebuffer into a shadow image and
 * kick a worker, so they never wait for USB; the worker (chip code) sends
 * what changed, and the newest image always wins. There is no vblank
 * interrupt: a hrtimer at the mode's refresh rate (DRM vblank timer
 * helpers) paces page-flip events.
 */

#include <linux/seq_file.h>
#include <linux/usb.h>
#include <linux/vmalloc.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_cache.h>
#include <drm/drm_color_mgmt.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_debugfs.h>
#include <drm/drm_drv.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>
#include <drm/drm_vblank_helper.h>

#include "mct_drv.h"

void mct_kick(struct mct_device *mdev)
{
	queue_work(system_long_wq, &mdev->work);
}

void mct_settle(struct mct_device *mdev, unsigned int ms)
{
	mod_delayed_work(system_long_wq, &mdev->settle_work, msecs_to_jiffies(ms));
}

static void mct_update_work(struct work_struct *work)
{
	struct mct_device *mdev = container_of(work, struct mct_device, work);

	mdev->funcs->update(mdev);
}

static void mct_settle_work(struct work_struct *work)
{
	struct mct_device *mdev = container_of(to_delayed_work(work), struct mct_device,
					       settle_work);

	mutex_lock(&mdev->lock);
	mdev->dirty = true;
	mutex_unlock(&mdev->lock);
	mct_kick(mdev);
}

static void mct_stop_updates(struct mct_device *mdev)
{
	/* the worker re-arms the settle timer and the timer queues the worker */
	do {
		cancel_work_sync(&mdev->work);
	} while (cancel_delayed_work_sync(&mdev->settle_work));
	cancel_work_sync(&mdev->work);
	if (mdev->funcs->stop)
		mdev->funcs->stop(mdev);
	mutex_lock(&mdev->lock);
	mdev->dirty = false;
	mdev->dirty_since = 0;
	mutex_unlock(&mdev->lock);
}

/* ------------------------------------------------------------------ */
/* primary plane						      */

static const u32 mct_formats[] = {
	DRM_FORMAT_XRGB8888,
};

static const u32 mct_cursor_formats[] = {
	DRM_FORMAT_ARGB8888,
};

static const u64 mct_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID
};

/* copy the rows set in rowmask from the (mapped) framebuffer, kick the worker */
static void mct_copy_rows(struct mct_device *mdev, struct drm_framebuffer *fb,
			  const struct iosys_map *map)
{
	struct drm_gem_object *obj = drm_gem_fb_get_obj(fb, 0);
	int w = mdev->width, y;
	ktime_t t0;
	u64 ns;

	mutex_lock(&mdev->lock);
	t0 = ktime_get();
	if (mdev->dirty)
		mdev->stats.skipped++;	/* the previous image never went out */
	/*
	 * Buffers allocated here (shmem) are cached system memory, cheap to read:
	 * compare each damaged row with the shadow and only take the ones that
	 * really changed. Compositors' damage is coarse (whole windows, or all
	 * rows on a rotated output with the clip workaround below).
	 */
	if (obj && !drm_gem_is_imported(obj) && !map->is_iomem) {
		for (y = 0; y < mdev->height; y++) {
			u8 *dst = mdev->shadow + (size_t)y * w * 4;
			const u8 *src = (const u8 *)map->vaddr + (size_t)y * fb->pitches[0];

			if (!mdev->rowmask[y] || (mdev->shadow_valid && !memcmp(dst, src, w * 4)))
				continue;
			memcpy(dst, src, w * 4);
			mdev->changed[y] = 1;
			mdev->stats.px_damage += w;
			mdev->stats.rows_copied++;
		}
		goto done;
	}
	/*
	 * Buffers shared by a GPU are usually mapped write-combined, where plain
	 * reads are very slow; the streaming-load copy is many times faster.
	 */
	for (y = 0; y < mdev->height; ) {
		int n = 0;

		while (y + n < mdev->height && mdev->rowmask[y + n])
			n++;
		if (!n) {
			y++;
			continue;
		}
		/*
		 * One call per run of rows: each call saves and restores FPU state.
		 * The mapping may be I/O memory (a plain-mapped VRAM import), so
		 * offset it as an iosys_map.
		 */
		if (fb->pitches[0] == w * 4) {
			struct iosys_map d = IOSYS_MAP_INIT_VADDR(mdev->shadow + (size_t)y * w * 4);
			struct iosys_map sm = *map;

			iosys_map_incr(&sm, (size_t)y * fb->pitches[0]);
			drm_memcpy_from_wc(&d, &sm, (size_t)n * w * 4);
		} else {
			int i;

			for (i = y; i < y + n; i++) {
				u8 *row = mdev->shadow + (size_t)i * w * 4;
				struct iosys_map d = IOSYS_MAP_INIT_VADDR(row);
				struct iosys_map sm = *map;

				iosys_map_incr(&sm, (size_t)i * fb->pitches[0]);
				drm_memcpy_from_wc(&d, &sm, w * 4);
			}
		}
		memset(mdev->changed + y, 1, n);
		mdev->stats.px_damage += (u64)n * w;
		mdev->stats.rows_copied += n;
		y += n;
	}
done:
	mdev->shadow_valid = true;
	ns = ktime_to_ns(ktime_sub(ktime_get(), t0));
	mdev->stats.copies++;
	mdev->stats.ns_copy += ns;
	mdev->stats.ns_copy_max = max(mdev->stats.ns_copy_max, ns);
	mdev->dirty = true;
	if (!mdev->dirty_since)
		mdev->dirty_since = t0;
	mutex_unlock(&mdev->lock);

	mct_kick(mdev);
}

/*
 * Rows to copy for this commit, in rowmask; false if nothing changed.
 *
 * Some compositors have sent FB_DAMAGE_CLIPS for a rotated output in the
 * output's rotated coordinates instead of the framebuffer's, and the kernel
 * then clips away what lies outside it. So take the raw clips and mark the
 * rows each one would cover under any rotation or flip: correct clips give
 * a few extra rows, unrotated ones still cover the change.
 */
static bool mct_damage_rows(struct mct_device *mdev, const struct drm_plane_state *state)
{
	unsigned int n = drm_plane_get_damage_clips_count(state), i;
	const struct drm_mode_rect *clips = drm_plane_get_damage_clips(state);
	int h = mdev->height, j, k;
	bool any = false;

	if (!n)
		mdev->stats.no_clips++;
	mdev->stats.clips += n;
	for (i = 0; i < n; i++)
		mdev->stats.clip_px += (u64)max(clips[i].x2 - clips[i].x1, 0) *
				       max(clips[i].y2 - clips[i].y1, 0);
	if (!n || !mdev->shadow_valid) {
		memset(mdev->rowmask, 1, h);
		return true;
	}
	memset(mdev->rowmask, 0, h);
	for (i = 0; i < n; i++) {
		const struct drm_mode_rect *c = &clips[i];
		int r[4][2] = {
			{ c->y1, c->y2 },		/* as given, or flipped horizontally */
			{ h - c->y2, h - c->y1 },	/* upside down */
			{ c->x1, c->x2 },		/* rotated by 90 or 270 */
			{ h - c->x2, h - c->x1 },
		};

		for (j = 0; j < 4; j++) {
			for (k = max(r[j][0], 0); k < min(r[j][1], h); k++) {
				mdev->rowmask[k] = 1;
				any = true;
			}
		}
	}
	return any;
}

static int mct_primary_atomic_check(struct drm_plane *plane, struct drm_atomic_commit *state)
{
	struct drm_plane_state *new_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_framebuffer *fb = new_state->fb;
	struct drm_crtc_state *crtc_state = NULL;
	int ret;

	if (new_state->crtc)
		crtc_state = drm_atomic_get_new_crtc_state(state, new_state->crtc);
	ret = drm_atomic_helper_check_plane_state(new_state, crtc_state,
						  DRM_PLANE_NO_SCALING, DRM_PLANE_NO_SCALING,
						  false, false);
	if (ret || !fb || !new_state->visible)
		return ret;
	/* the image starts at the framebuffer's top left (larger ones: fbdev) */
	if (fb->width < crtc_state->mode.hdisplay || fb->height < crtc_state->mode.vdisplay ||
	    new_state->src_x || new_state->src_y || fb->offsets[0])
		return -EINVAL;
	return 0;
}

static void mct_primary_atomic_update(struct drm_plane *plane, struct drm_atomic_commit *state)
{
	struct mct_device *mdev = to_mct(plane->dev);
	struct drm_plane_state *new_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_shadow_plane_state *shadow = to_drm_shadow_plane_state(new_state);
	struct drm_framebuffer *fb = new_state->fb;
	ktime_t t0;
	u64 ns;

	if (!fb || !new_state->visible || !mdev->width)
		return;
	if (fb->width < mdev->width || fb->height < mdev->height)
		goto invalid;
	if (!mct_damage_rows(mdev, new_state))
		return;
	t0 = ktime_get();
	if (drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE))
		goto invalid;
	mct_copy_rows(mdev, fb, &shadow->data[0]);
	drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);
	ns = ktime_to_ns(ktime_sub(ktime_get(), t0));
	mdev->stats.ns_commit += ns;
	mdev->stats.ns_commit_max = max(mdev->stats.ns_commit_max, ns);
	return;

invalid:
	/* this commit's changes never reached the shadow: copy all next time */
	mutex_lock(&mdev->lock);
	mdev->shadow_valid = false;
	mutex_unlock(&mdev->lock);
}

static const struct drm_plane_helper_funcs mct_primary_helper_funcs = {
	DRM_GEM_SHADOW_PLANE_HELPER_FUNCS,
	.atomic_check = mct_primary_atomic_check,
	.atomic_update = mct_primary_atomic_update,
};

static const struct drm_plane_funcs mct_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	DRM_GEM_SHADOW_PLANE_FUNCS,
};

/* ------------------------------------------------------------------ */
/* cursor plane							      */

static int mct_cursor_atomic_check(struct drm_plane *plane, struct drm_atomic_commit *state)
{
	struct mct_device *mdev = to_mct(plane->dev);
	struct drm_plane_state *new_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_framebuffer *fb = new_state->fb;
	struct drm_crtc_state *crtc_state = NULL;

	if (new_state->crtc)
		crtc_state = drm_atomic_get_new_crtc_state(state, new_state->crtc);
	/* the image is taken from the framebuffer's top left */
	if (fb && (fb->width > mdev->cur_size || fb->height > mdev->cur_size ||
		   new_state->src_x || new_state->src_y))
		return -EINVAL;
	return drm_atomic_helper_check_plane_state(new_state, crtc_state,
						   DRM_PLANE_NO_SCALING, DRM_PLANE_NO_SCALING,
						   true, true);
}

static void mct_cursor_atomic_update(struct drm_plane *plane, struct drm_atomic_commit *state)
{
	struct mct_device *mdev = to_mct(plane->dev);
	struct drm_plane_state *new_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_shadow_plane_state *shadow = to_drm_shadow_plane_state(new_state);
	struct drm_framebuffer *fb = new_state->fb;
	int y, w, idx;

	if (!drm_dev_enter(plane->dev, &idx))
		return;
	if (!fb || !new_state->visible) {
		mdev->funcs->cursor_hide(mdev);
		goto out;
	}
	if (drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE))
		goto out;
	memset(mdev->cur, 0, mdev->cur_size * mdev->cur_size * 4);
	w = min_t(int, fb->width, mdev->cur_size) * 4;
	for (y = 0; y < min_t(int, fb->height, mdev->cur_size); y++)
		iosys_map_memcpy_from(mdev->cur + y * mdev->cur_size * 4, &shadow->data[0],
				      y * fb->pitches[0], w);
	drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);
	mdev->funcs->cursor_set(mdev, mdev->cur, new_state->crtc_x, new_state->crtc_y);
out:
	drm_dev_exit(idx);
}

static const struct drm_plane_helper_funcs mct_cursor_helper_funcs = {
	DRM_GEM_SHADOW_PLANE_HELPER_FUNCS,
	.atomic_check = mct_cursor_atomic_check,
	.atomic_update = mct_cursor_atomic_update,
};

/* ------------------------------------------------------------------ */
/* CRTC / encoder / connector					      */

static int mct_crtc_atomic_check(struct drm_crtc *crtc, struct drm_atomic_commit *state)
{
	struct mct_device *mdev = to_mct(crtc->dev);
	struct drm_crtc_state *crtc_state = drm_atomic_get_new_crtc_state(state, crtc);

	if (crtc_state->enable && mdev->funcs->mode_valid(mdev, &crtc_state->mode) != MODE_OK)
		return -EINVAL;
	if (crtc_state->gamma_lut && drm_color_lut_size(crtc_state->gamma_lut) != 256)
		return -EINVAL;
	return drm_crtc_helper_atomic_check(crtc, state);
}

static void mct_crtc_atomic_flush(struct drm_crtc *crtc, struct drm_atomic_commit *state)
{
	struct mct_device *mdev = to_mct(crtc->dev);
	struct drm_crtc_state *crtc_state = drm_atomic_get_new_crtc_state(state, crtc);

	if (crtc_state->color_mgmt_changed && mdev->funcs->gamma)
		mdev->funcs->gamma(mdev, crtc_state->gamma_lut ?
					 crtc_state->gamma_lut->data : NULL);
	drm_crtc_vblank_atomic_flush(crtc, state);
}

/*
 * The shadow is sized for the current mode: allocated when the CRTC goes on
 * (updates are stopped then), kept while the size stays the same.
 */
int mct_shadow_alloc(struct mct_device *mdev, int w, int h)
{
	size_t size = (size_t)w * h * 4;

	if (size == mdev->shadow_size)
		return 0;
	kvfree(mdev->shadow);
	mdev->shadow = kvzalloc(size, GFP_KERNEL);
	mdev->shadow_size = mdev->shadow ? size : 0;
	return mdev->shadow ? 0 : -ENOMEM;
}

static void mct_crtc_atomic_enable(struct drm_crtc *crtc, struct drm_atomic_commit *state)
{
	struct mct_device *mdev = to_mct(crtc->dev);
	struct drm_crtc_state *crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	const struct drm_display_mode *mode = &crtc_state->mode;

	/* if this fails (no memory) the display stays dark, the rest works */
	if (mct_shadow_alloc(mdev, mode->hdisplay, mode->vdisplay) ||
	    mdev->funcs->enable(mdev, mode)) {
		drm_err(&mdev->dev, "could not set up %dx%d\n", mode->hdisplay, mode->vdisplay);
		goto out;
	}
	mutex_lock(&mdev->lock);
	mdev->shadow_valid = false;
	mdev->refresh = drm_mode_vrefresh(mode);
	mdev->width = mode->hdisplay;
	mdev->height = mode->vdisplay;
	memset(mdev->shadow, 0, mdev->shadow_size);
	mutex_unlock(&mdev->lock);
out:
	drm_crtc_vblank_on(crtc);
}

static void mct_crtc_atomic_disable(struct drm_crtc *crtc, struct drm_atomic_commit *state)
{
	struct mct_device *mdev = to_mct(crtc->dev);
	int idx;

	drm_crtc_vblank_off(crtc);
	/* off first, so nothing kicked meanwhile does a whole update */
	mutex_lock(&mdev->lock);
	mdev->width = 0;
	mdev->height = 0;
	mutex_unlock(&mdev->lock);
	mct_stop_updates(mdev);
	if (drm_dev_enter(crtc->dev, &idx)) {
		mdev->funcs->disable(mdev);
		drm_dev_exit(idx);
	}
}

static const struct drm_crtc_helper_funcs mct_crtc_helper_funcs = {
	.atomic_check = mct_crtc_atomic_check,
	.atomic_flush = mct_crtc_atomic_flush,
	.atomic_enable = mct_crtc_atomic_enable,
	.atomic_disable = mct_crtc_atomic_disable,
};

static const struct drm_crtc_funcs mct_crtc_funcs = {
	.reset = drm_atomic_helper_crtc_reset,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
	DRM_CRTC_VBLANK_TIMER_FUNCS,
};

static const struct drm_encoder_funcs mct_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

static int mct_conn_get_modes(struct drm_connector *connector)
{
	return to_mct(connector->dev)->funcs->get_modes(to_mct(connector->dev), connector);
}

static enum drm_mode_status mct_conn_mode_valid(struct drm_connector *connector,
						const struct drm_display_mode *mode)
{
	return to_mct(connector->dev)->funcs->mode_valid(to_mct(connector->dev), mode);
}

static int mct_conn_detect(struct drm_connector *connector, struct drm_modeset_acquire_ctx *ctx,
			   bool force)
{
	struct mct_device *mdev = to_mct(connector->dev);
	int idx, ret;

	if (!mdev->funcs->detect)
		return connector_status_connected;
	if (!drm_dev_enter(connector->dev, &idx))
		return connector_status_disconnected;
	ret = mdev->funcs->detect(mdev);
	drm_dev_exit(idx);
	return ret;
}

static const struct drm_connector_helper_funcs mct_conn_helper_funcs = {
	.get_modes = mct_conn_get_modes,
	.mode_valid = mct_conn_mode_valid,
	.detect_ctx = mct_conn_detect,
};

static int mct_conn_late_register(struct drm_connector *connector)
{
	struct mct_device *mdev = to_mct(connector->dev);

	return mdev->funcs->late_register ? mdev->funcs->late_register(mdev) : 0;
}

static void mct_conn_early_unregister(struct drm_connector *connector)
{
	struct mct_device *mdev = to_mct(connector->dev);

	if (mdev->funcs->early_unregister)
		mdev->funcs->early_unregister(mdev);
}

static const struct drm_connector_funcs mct_conn_funcs = {
	.late_register = mct_conn_late_register,
	.early_unregister = mct_conn_early_unregister,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

/* ------------------------------------------------------------------ */
/* debugfs							      */

/* "ms.hh" for a total of ns over n events */
static void mct_print_ms(struct seq_file *m, u64 ns, u64 n)
{
	u64 hundredths = div64_u64(ns, n * 10000);

	seq_printf(m, "%llu.%02llu", div_u64(hundredths, 100), hundredths % 100);
}

static int mct_stats_show(struct seq_file *m, void *data)
{
	struct drm_debugfs_entry *entry = m->private;
	struct mct_device *mdev = to_mct(entry->dev);
	u64 ms = max_t(u64, ktime_ms_delta(ktime_get(), mdev->stats.since), 1);
	u64 copies = max_t(u64, mdev->stats.copies, 1);

	seq_printf(m, "seconds          %llu\n", ms / 1000);
	if (mdev->funcs->stats)
		mdev->funcs->stats(mdev, m);
	seq_printf(m, "skipped images   %llu\n", mdev->stats.skipped);
	seq_printf(m, "pixels damaged   %llu\n", mdev->stats.px_damage);
	seq_printf(m, "damage clips     %llu commits without, %llu clips, %llu px in clips\n",
		   mdev->stats.no_clips, mdev->stats.clips, mdev->stats.clip_px);
	seq_printf(m, "rows changed     %llu per commit\n",
		   div64_u64(mdev->stats.rows_copied, copies));
	seq_puts(m, "ms per fb copy   ");
	mct_print_ms(m, mdev->stats.ns_copy, copies);
	seq_puts(m, " (max ");
	mct_print_ms(m, mdev->stats.ns_copy_max, 1);
	seq_printf(m, ") over %llu copies\n", mdev->stats.copies);
	seq_puts(m, "ms per commit    ");
	mct_print_ms(m, mdev->stats.ns_commit, copies);
	seq_puts(m, " (max ");
	mct_print_ms(m, mdev->stats.ns_commit_max, 1);
	seq_puts(m, ") incl. cpu access setup\n");
	return 0;
}

/* ------------------------------------------------------------------ */
/* sysfs: /sys/bus/usb/devices/<interface>/mct/ (docs/driver/companion-app.md) */

/*
 * NULL once the interface is unbound (the files go away a little later
 * than the driver data); the device itself lives until the files are gone.
 */
struct mct_device *mct_from_dev(struct device *dev)
{
	struct drm_device *drm = usb_get_intfdata(to_usb_interface(dev));

	return drm ? to_mct(drm) : NULL;
}

static ssize_t chip_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mct_device *mdev = mct_from_dev(dev);

	if (!mdev)
		return -ENODEV;
	return sysfs_emit(buf, "%s\n", mdev->chip);
}
static DEVICE_ATTR_RO(chip);

static ssize_t model_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	const char *p = interface_to_usbdev(to_usb_interface(dev))->product;

	return sysfs_emit(buf, "%s\n", p ?: "-");
}
static DEVICE_ATTR_RO(model);

static ssize_t driver_version_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mct_device *mdev = mct_from_dev(dev);
	const struct drm_driver *drv;

	if (!mdev)
		return -ENODEV;
	drv = mdev->dev.driver;
	return sysfs_emit(buf, "%s %d.%d.%d\n", drv->name, drv->major, drv->minor,
			  drv->patchlevel);
}
static DEVICE_ATTR_RO(driver_version);

static ssize_t connector_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mct_device *mdev = mct_from_dev(dev);

	if (!mdev)
		return -ENODEV;
	return sysfs_emit(buf, "%s\n", mdev->conn.kdev ? dev_name(mdev->conn.kdev) : "-");
}
static DEVICE_ATTR_RO(connector);

static ssize_t mode_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mct_device *mdev = mct_from_dev(dev);
	int w, h, hz;

	if (!mdev)
		return -ENODEV;
	mutex_lock(&mdev->lock);
	w = mdev->width;
	h = mdev->height;
	hz = mdev->refresh;
	mutex_unlock(&mdev->lock);
	if (!w)
		return sysfs_emit(buf, "off\n");
	return sysfs_emit(buf, "%dx%d@%d\n", w, h, hz);
}
static DEVICE_ATTR_RO(mode);

static struct attribute *mct_common_attrs[] = {
	&dev_attr_chip.attr,
	&dev_attr_model.attr,
	&dev_attr_driver_version.attr,
	&dev_attr_connector.attr,
	&dev_attr_mode.attr,
};

/* the common files and the chip's, as one group "mct" */
static int mct_attrs_init(struct mct_device *mdev, const struct mct_config *cfg)
{
	int n = 0, i;

	while (cfg->attrs && cfg->attrs[n])
		n++;
	mdev->attrs = drmm_kcalloc(&mdev->dev, ARRAY_SIZE(mct_common_attrs) + n + 1,
				   sizeof(*mdev->attrs), GFP_KERNEL);
	if (!mdev->attrs)
		return -ENOMEM;
	for (i = 0; i < ARRAY_SIZE(mct_common_attrs); i++)
		mdev->attrs[i] = mct_common_attrs[i];
	for (i = 0; i < n; i++)
		mdev->attrs[ARRAY_SIZE(mct_common_attrs) + i] = cfg->attrs[i];
	mdev->attr_group.name = "mct";
	mdev->attr_group.attrs = mdev->attrs;
	mdev->chip = cfg->chip;
	return 0;
}

/* ------------------------------------------------------------------ */
/* setup							      */

/*
 * Enable the CRTC before updating the planes: the mode set clears what the
 * chip code knows about the device's frame buffers and cursor, so the
 * planes of the same commit must come after it, or the first frame and the
 * cursor after a mode set or resume would be dropped.
 */
static void mct_commit_tail(struct drm_atomic_commit *state)
{
	struct drm_device *dev = state->dev;

	drm_atomic_helper_commit_modeset_disables(dev, state);
	drm_atomic_helper_commit_modeset_enables(dev, state);
	drm_atomic_helper_commit_planes(dev, state, 0);
	drm_atomic_helper_fake_vblank(state);
	drm_atomic_helper_commit_hw_done(state);
	drm_atomic_helper_wait_for_vblanks(dev, state);
	drm_atomic_helper_cleanup_planes(dev, state);
}

static const struct drm_mode_config_helper_funcs mct_mode_config_helpers = {
	.atomic_commit_tail = mct_commit_tail,
};

static const struct drm_mode_config_funcs mct_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static void mct_release(struct drm_device *dev, void *res)
{
	struct mct_device *mdev = to_mct(dev);

	/* a late sysfs write may have queued work after the unplug stopped it */
	cancel_delayed_work_sync(&mdev->settle_work);
	cancel_work_sync(&mdev->work);
	kvfree(mdev->shadow);
}

int mct_init(struct mct_device *mdev, const struct mct_config *cfg)
{
	struct drm_device *dev = &mdev->dev;
	int rows = max(cfg->max_w, cfg->max_h);
	int ret;

	mdev->funcs = cfg->funcs;
	ret = drmm_mutex_init(dev, &mdev->lock);
	if (ret)
		return ret;
	ret = mct_attrs_init(mdev, cfg);
	if (ret)
		return ret;
	INIT_WORK(&mdev->work, mct_update_work);
	INIT_DELAYED_WORK(&mdev->settle_work, mct_settle_work);

	ret = drmm_add_action_or_reset(dev, mct_release, NULL);
	if (ret)
		return ret;
	mdev->rowmask = drmm_kzalloc(dev, rows, GFP_KERNEL);
	mdev->changed = drmm_kzalloc(dev, rows, GFP_KERNEL);
	mdev->cur_size = cfg->cursor_size;
	mdev->cur = drmm_kzalloc(dev, mdev->cur_size * mdev->cur_size * 4, GFP_KERNEL);
	if (!mdev->rowmask || !mdev->changed || !mdev->cur)
		return -ENOMEM;

	ret = drmm_mode_config_init(dev);
	if (ret)
		return ret;
	dev->mode_config.min_width = cfg->min_w;
	dev->mode_config.max_width = cfg->max_w;
	dev->mode_config.min_height = cfg->min_h;
	dev->mode_config.max_height = cfg->max_h;
	dev->mode_config.cursor_width = cfg->cursor_size;
	dev->mode_config.cursor_height = cfg->cursor_size;
	dev->mode_config.funcs = &mct_mode_config_funcs;
	dev->mode_config.helper_private = &mct_mode_config_helpers;

	ret = drm_universal_plane_init(dev, &mdev->primary, 0, &mct_plane_funcs,
				       mct_formats, ARRAY_SIZE(mct_formats),
				       mct_modifiers, DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ret;
	drm_plane_helper_add(&mdev->primary, &mct_primary_helper_funcs);
	/* let userspace pass damage rects so small changes become partial updates */
	drm_plane_enable_fb_damage_clips(&mdev->primary);

	ret = drm_universal_plane_init(dev, &mdev->cursor, 0, &mct_plane_funcs,
				       mct_cursor_formats, ARRAY_SIZE(mct_cursor_formats),
				       mct_modifiers, DRM_PLANE_TYPE_CURSOR, NULL);
	if (ret)
		return ret;
	drm_plane_helper_add(&mdev->cursor, &mct_cursor_helper_funcs);

	ret = drm_crtc_init_with_planes(dev, &mdev->crtc, &mdev->primary, &mdev->cursor,
					&mct_crtc_funcs, NULL);
	if (ret)
		return ret;
	drm_crtc_helper_add(&mdev->crtc, &mct_crtc_helper_funcs);
	if (cfg->gamma) {
		drm_crtc_enable_color_mgmt(&mdev->crtc, 0, false, 256);
		ret = drm_mode_crtc_set_gamma_size(&mdev->crtc, 256);
		if (ret)
			return ret;
	}

	ret = drm_encoder_init(dev, &mdev->encoder, &mct_encoder_funcs, cfg->encoder_type, NULL);
	if (ret)
		return ret;
	mdev->encoder.possible_crtcs = drm_crtc_mask(&mdev->crtc);

	drm_connector_helper_add(&mdev->conn, &mct_conn_helper_funcs);
	ret = drm_connector_init(dev, &mdev->conn, &mct_conn_funcs, cfg->connector_type);
	if (ret)
		return ret;
	/* events for changes where the chip sends them, polling as well */
	if (cfg->poll)
		mdev->conn.polled = DRM_CONNECTOR_POLL_HPD | DRM_CONNECTOR_POLL_CONNECT |
				    DRM_CONNECTOR_POLL_DISCONNECT;
	ret = drm_connector_attach_encoder(&mdev->conn, &mdev->encoder);
	if (ret)
		return ret;

	ret = drm_vblank_init(dev, 1);
	if (ret)
		return ret;
	drm_mode_config_reset(dev);

	if (cfg->poll)
		drmm_kms_helper_poll_init(dev);
	mdev->stats.since = ktime_get();
	drm_debugfs_add_file(dev, cfg->stats_name, mct_stats_show, NULL);
	return 0;
}

int mct_register(struct mct_device *mdev, struct usb_interface *intf)
{
	int ret;

	usb_set_intfdata(intf, &mdev->dev);
	ret = drm_dev_register(&mdev->dev, 0);
	if (ret)
		return ret;
	/* removed with the interface's devres when the driver unbinds */
	ret = devm_device_add_group(&intf->dev, &mdev->attr_group);
	if (ret)
		drm_warn(&mdev->dev, "no sysfs files: %d\n", ret);
	drm_client_setup(&mdev->dev, NULL);
	return 0;
}

/*
 * The workers can be kicked with the display already off (format change,
 * redraw request), when the shutdown below doesn't stop them: do it here.
 */
void mct_unplug(struct mct_device *mdev)
{
	struct usb_device *udev = interface_to_usbdev(to_usb_interface(mdev->dev.dev));

	/*
	 * A software unbind (rmmod, sysfs) leaves the device attached: switch
	 * it off first, while it can still be reached.
	 */
	if (udev->state != USB_STATE_NOTATTACHED) {
		drm_atomic_helper_shutdown(&mdev->dev);
		drm_dev_unplug(&mdev->dev);
	} else {
		drm_dev_unplug(&mdev->dev);
		drm_atomic_helper_shutdown(&mdev->dev);
	}
	mct_stop_updates(mdev);
}

int mct_suspend(struct mct_device *mdev)
{
	int ret = drm_mode_config_helper_suspend(&mdev->dev);

	if (!ret)
		mct_stop_updates(mdev);
	return ret;
}

int mct_resume(struct mct_device *mdev)
{
	return drm_mode_config_helper_resume(&mdev->dev);
}
