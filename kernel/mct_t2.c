// SPDX-License-Identifier: GPL-2.0-only
/*
 * DRM driver for MCT Trigger II (T2) USB displays, e.g. the Wacom DTU-1031X
 * pen display (USB 0711:520d, 1024x600).
 *
 * Protocol (reverse engineered, see docs/protocol-t2.md):
 *  - bulk EP 0x03 / 0x04 carry register programming, replies on EP 0x81
 *  - bulk EP 0x02 carries framebuffer commands; the firmware parses it per
 *    USB transfer, so every command header is its own transfer
 *  - 0x13 writes a (sub-)rectangle into one of two framebuffers using a
 *    planar RLE, 0x15 flips scanout between the framebuffers
 *  - 0x12 uploads a 64x64 RGBA cursor image; EP 0x04 register lists
 *    position and show/hide it
 *  - the panel scans out rotated by 180 degrees
 *
 * The backlight is not controlled by the T2 but by the tablet's pen
 * function (a sibling USB device on the same internal hub): HID feature
 * report 0x06 holds the brightness level, 0 to 8 (the vendor tool uses 1..7).
 *
 * Structure modelled on drivers/gpu/drm/udl and drivers/gpu/drm/tiny/gm12u320.c.
 */

#include <linux/backlight.h>
#include <linux/gcd.h>
#include <linux/module.h>
#include <linux/pm.h>
#include <linux/usb.h>
#include <linux/unaligned.h>
#include <linux/vmalloc.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_encoder.h>
#include <drm/drm_fbdev_shmem.h>
#include <drm/drm_file.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_managed.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_plane.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>
#include <drm/drm_vblank_helper.h>

#include "mct_t2_init.h"

#define DRIVER_NAME	"mct_t2"
#define DRIVER_DESC	"MCT Trigger II USB display"
#define DRIVER_MAJOR	0
#define DRIVER_MINOR	3
#define DRIVER_PATCH	3

#define T2_WIDTH	1024
#define T2_HEIGHT	600
#define T2_STRIDE	(T2_WIDTH * 3)		/* device framebuffer: packed RGB24 */
#define T2_FB_A		0x00c000u
#define T2_FB_B		(T2_FB_A + T2_STRIDE * T2_HEIGHT)

#define T2_EP_FB	0x02
#define T2_EP_CMD	0x03
#define T2_EP_MCU	0x04
#define T2_EP_IN	0x81

#define T2_CHUNK	19968			/* vendor payload transfer size */
#define T2_BOUNCE	65536
#define T2_ENC_MAX	(T2_WIDTH * (T2_HEIGHT + 16) * 3 + 4096)
#define T2_TIMEOUT	2000

#define T2_CUR		64			/* hardware cursor is 64x64 */
#define T2_CUR_BYTES	(T2_CUR * T2_CUR * 4)

/* full frame + flip when the damage covers more than 1/FULL_DIV of the screen */
#define T2_FULL_DIV	3

struct mct_t2 {
	struct drm_device dev;
	struct drm_plane primary;
	struct drm_plane cursor;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector conn;

	struct mutex io_lock;		/* EP2 command sequences + bounce */
	u8 *bounce;			/* DMA-able transfer buffer */
	u8 *enc;			/* RLE output */
	u8 *shadow;			/* XRGB8888 copy of the visible image */
	const struct drm_edid *edid;	/* read from the device at init */
	u32 front;			/* framebuffer address currently scanned out */
	bool first;			/* next update must be a full frame */

	struct {
		struct work_struct work;
		struct mutex lock;	/* protects shadow writes + rect/dirty */
		struct drm_rect rect;	/* accumulated damage, visible coordinates */
		bool dirty;
	} upd;

	struct {
		u8 *buf;		/* DMA-able: position packets */
		u8 *img;		/* device-format image being built */
		u8 *shown;		/* image last uploaded */
		bool uploaded, sized, visible;
	} cur;
};

#define to_t2(__dev) container_of(__dev, struct mct_t2, dev)

static struct usb_device *t2_udev(struct mct_t2 *t2)
{
	return interface_to_usbdev(to_usb_interface(t2->dev.dev));
}

/* ------------------------------------------------------------------ */
/* USB								      */

static int t2_xfer(struct mct_t2 *t2, u8 *buf, u8 ep, const void *data, int len)
{
	struct usb_device *udev = t2_udev(t2);
	int ret, done;

	if (WARN_ON(len > T2_BOUNCE))
		return -EINVAL;
	if (data != buf) {
		if (data)
			memcpy(buf, data, len);
		else
			memset(buf, 0, len);
	}
	ret = usb_bulk_msg(udev, usb_sndbulkpipe(udev, ep), buf, len, &done, T2_TIMEOUT);
	if (!ret && done != len)
		ret = -EIO;
	return ret;
}

static int t2_out(struct mct_t2 *t2, u8 ep, const void *data, int len)
{
	return t2_xfer(t2, t2->bounce, ep, data, len);
}

static int t2_in(struct mct_t2 *t2, int len)
{
	struct usb_device *udev = t2_udev(t2);
	int ret, done, tries;

	for (tries = 0; tries < 4; tries++) {
		ret = usb_bulk_msg(udev, usb_rcvbulkpipe(udev, T2_EP_IN), t2->bounce,
				   min(len, T2_BOUNCE), &done, T2_TIMEOUT);
		if (!ret)
			return done;
		msleep(200);
	}
	return ret;
}

/* header in its own transfer, payload in T2_CHUNK transfers; io_lock held */
static int t2_cmd(struct mct_t2 *t2, const u8 *hdr, int hlen, const u8 *pay, int plen)
{
	int off, ret;

	ret = t2_out(t2, T2_EP_FB, hdr, hlen);
	for (off = 0; !ret && off < plen; off += T2_CHUNK)
		ret = t2_out(t2, T2_EP_FB, pay + off, min(plen - off, T2_CHUNK));
	return ret;
}

/* ------------------------------------------------------------------ */
/* RLE encoder (see docs/protocol-t2.md)			      */

/* one channel line of n values; 0x30 is the escape byte, runs "30 NN VV" = NN+1 x VV */
static int t2_rle_line(const u8 *v, int n, u8 *out)
{
	u8 *o = out;
	int x = 0;

	while (x < n) {
		u8 c = v[x] == 0x30 ? 0x31 : v[x];
		int r = 1, i;

		while (x + r < n && r < 0xfb && (v[x + r] == 0x30 ? 0x31 : v[x + r]) == c)
			r++;
		if (r >= 3) {
			*o++ = 0x30;
			if (r - 1 == 0x30) {	/* vendor never emits count 0x30 */
				*o++ = 0x2f;
				*o++ = c;
			} else {
				*o++ = r - 1;
			}
			*o++ = c;
		} else {
			for (i = 0; i < r; i++)
				*o++ = c;
		}
		x += r;
	}
	return o - out;
}

/*
 * Encode device rect (dx, dy, w, h) from the visible XRGB8888 shadow image,
 * rotating by 180 degrees. Pixels stream row-major in 1024-pixel units, each
 * unit as R, G, B lines. Rows past the panel (alignment overshoot) are black.
 */
static int t2_encode(struct mct_t2 *t2, int dx, int dy, int w, int h)
{
	static u8 ch[3][1024];	/* only used with io_lock held */
	int x, y, k = 0, len = 0, c;

	for (y = dy; y < dy + h; y++) {
		for (x = dx; x < dx + w; x++) {
			if (y < T2_HEIGHT) {
				const u8 *px = t2->shadow +
					((T2_HEIGHT - 1 - y) * T2_WIDTH + (T2_WIDTH - 1 - x)) * 4;

				ch[0][k] = px[2];
				ch[1][k] = px[1];
				ch[2][k] = px[0];
			} else {
				ch[0][k] = 0;
				ch[1][k] = 0;
				ch[2][k] = 0;
			}
			if (++k == 1024) {
				for (c = 0; c < 3; c++)
					len += t2_rle_line(ch[c], 1024, t2->enc + len);
				k = 0;
			}
		}
	}
	return len;
}

static int t2_send_rect(struct mct_t2 *t2, u32 base, int dx, int dy, int w, int h)
{
	u32 a = (base + dy * T2_STRIDE + dx * 3) << 8;
	int len = t2_encode(t2, dx, dy, w, h);
	u8 hdr[36];

	hdr[0] = 0x13;
	put_unaligned_le32(a, hdr + 1);
	put_unaligned_le32(a, hdr + 5);
	hdr[9] = 0;
	put_unaligned_le16(w, hdr + 10);
	put_unaligned_le16(h, hdr + 12);
	put_unaligned_le16(w, hdr + 14);
	put_unaligned_le16(h, hdr + 16);
	memcpy(hdr + 18, "\x00\x40\x00\x40\x00\x00\x00\x39\x00", 9);
	put_unaligned_le24(w * h * 3, hdr + 27);
	put_unaligned_le24(len, hdr + 30);
	memcpy(hdr + 33, "\x30\x05\x28", 3);
	return t2_cmd(t2, hdr, sizeof(hdr), t2->enc, len);
}

/* 0x15 register batch: scanout address (>> 2) + latch */
static int t2_flip(struct mct_t2 *t2, u32 base)
{
	u32 a = base >> 2;
	const u8 cmd[23] = {
		0x15, 0x07, 0xfb, 0x65, base == T2_FB_A ? 0x10 : 0x20,
		0xfc, 0xb4, 0x11, 0xfc, 0xb1, a & 0xff, 0xfc, 0xb2, (a >> 8) & 0xff,
		0xfc, 0xb3, (a >> 16) & 0xff, 0xfc, 0xb4, 0x15, 0xfc, 0xb4, 0x11,
	};

	return t2_out(t2, T2_EP_FB, cmd, sizeof(cmd));
}

static int t2_show_full(struct mct_t2 *t2)
{
	u32 back = t2->front == T2_FB_A ? T2_FB_B : T2_FB_A;
	int ret;

	ret = t2_send_rect(t2, back, 0, 0, T2_WIDTH, T2_HEIGHT);
	if (!ret)
		ret = t2_flip(t2, back);
	if (!ret)
		t2->front = back;
	return ret;
}

/* visible-space rect -> aligned device rect, written into the front buffer */
static int t2_show_partial(struct mct_t2 *t2, const struct drm_rect *r)
{
	int dx1 = T2_WIDTH - r->x2, dx2 = T2_WIDTH - r->x1;
	int dy1 = T2_HEIGHT - r->y2, dy2 = T2_HEIGHT - r->y1;
	int w, h, m;

	dx1 = round_down(dx1, 64);
	dx2 = round_up(dx2, 64);
	w = dx2 - dx1;
	m = 16 / gcd(w / 64, 16);	/* w * h must be a multiple of 1024 */
	h = roundup(dy2 - dy1, m);
	if (dy1 + h > T2_HEIGHT)
		dy1 = max(T2_HEIGHT - h, 0);
	return t2_send_rect(t2, t2->front, dx1, dy1, w, h);
}

/* single register write on EP3: 02 00 01 <hi> <lo> <val> */
static int t2_wreg(struct mct_t2 *t2, u16 reg, u8 val)
{
	const u8 cmd[6] = { 0x02, 0x00, 0x01, reg >> 8, reg & 0xff, val };
	int ret;

	mutex_lock(&t2->io_lock);
	ret = t2_out(t2, T2_EP_CMD, cmd, sizeof(cmd));
	mutex_unlock(&t2->io_lock);
	return ret;
}

/*
 * Panel power: the vendor driver has no display-power control for this
 * panel (its RomSink power method is a no-op). Stopping the pixel-clock PLL
 * (fcf6 = 0) makes the panel electronics switch the backlight off; setting
 * it back to 1 restores the picture without re-init.
 */
#define T2_REG_PLL_EN	0xfcf6

/* ------------------------------------------------------------------ */
/* device init							      */

/* 8051 pacing, validated from a cold chip (docs/init-sequence.md) */
static unsigned int t2_pace_us(u8 ep, const u8 *data)
{
	if (ep == T2_EP_FB)
		return 0;
	if (ep == T2_EP_CMD && data && data[0] == 0x02)		/* single register write */
		return data[3] == 0xfe && (data[4] == 0xb0 || data[4] == 0xb1) ? 50000 : 2000;
	return 20000;
}

static int t2_hw_init(struct mct_t2 *t2)
{
	const u8 *p = mct_t2_init_seq, *end = p + sizeof(mct_t2_init_seq);
	int n = 0, ret;

	mutex_lock(&t2->io_lock);
	while (p + 6 <= end) {
		u8 ep = p[0], flags = p[1];
		u32 len = get_unaligned_le32(p + 2);
		const u8 *data = NULL;
		u32 rdlen = 0, us;

		p += 6;
		if (!(flags & 1)) {
			data = p;
			p += len;
		}
		if (flags & 2) {
			rdlen = get_unaligned_le32(p);
			p += 4;
		}
		ret = -EINVAL;
		if (p > end)
			goto err;

		/*
		 * 0x14 display enable scans out buffer A: put a black frame there
		 * first (the trimmed init no longer clears video memory).
		 */
		if (ep == T2_EP_CMD && data && data[0] == 0x14) {
			/* shadow: zeroed at probe, last image on resume */
			ret = t2_send_rect(t2, T2_FB_A, 0, 0, T2_WIDTH, T2_HEIGHT);
			if (ret)
				goto err;
			t2->front = T2_FB_A;
		}

		ret = t2_out(t2, ep, data, len);
		if (ret)
			goto err;
		if (flags & 2) {
			ret = t2_in(t2, rdlen);
			if (ret < 0)
				goto err;
			/* 0x1d device info: panel EDID at +0x80 */
			if (data && data[0] == 0x1d && ret >= 0x80 + EDID_LENGTH && !t2->edid)
				t2->edid = drm_edid_alloc(t2->bounce + 0x80, EDID_LENGTH);
		}
		us = t2_pace_us(ep, data);
		if (us)
			usleep_range(us, us + us / 4);
		n++;
	}
	mutex_unlock(&t2->io_lock);
	t2->cur.uploaded = false;
	t2->cur.sized = false;
	t2->cur.visible = false;
	drm_dbg(&t2->dev, "init: %d transfers\n", n);
	return 0;
err:
	mutex_unlock(&t2->io_lock);
	drm_err(&t2->dev, "init failed at transfer %d: %d\n", n, ret);
	return ret;
}

/* ------------------------------------------------------------------ */
/* backlight (via the pen function's HID feature report 0x06)	      */

#define T2_PEN_VENDOR	0x056a
#define T2_BL_REPORT	0x06
#define T2_BL_MAX	8	/* report values 0..8; the firmware clamps above 8 */

struct t2_pen_lookup {
	struct usb_device *hub;
	struct usb_device *pen;
};

static int t2_match_pen(struct usb_device *udev, void *data)
{
	struct t2_pen_lookup *l = data;

	if (udev->parent != l->hub ||
	    le16_to_cpu(udev->descriptor.idVendor) != T2_PEN_VENDOR)
		return 0;
	l->pen = usb_get_dev(udev);
	return 1;
}

/* HID GET/SET_REPORT of the brightness feature report on the pen's interface 0 */
static int t2_bl_xfer(struct mct_t2 *t2, bool set, u8 *level)
{
	struct t2_pen_lookup l = { .hub = t2_udev(t2)->parent };
	u8 *buf;
	int ret;

	if (!l.hub)
		return -ENODEV;
	usb_for_each_dev(&l, t2_match_pen);
	if (!l.pen)
		return -ENODEV;

	buf = kmalloc(2, GFP_KERNEL);
	if (!buf) {
		usb_put_dev(l.pen);
		return -ENOMEM;
	}
	buf[0] = T2_BL_REPORT;
	buf[1] = *level;
	if (set)
		ret = usb_control_msg(l.pen, usb_sndctrlpipe(l.pen, 0), 0x09 /* SET_REPORT */,
				      USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
				      0x0300 | T2_BL_REPORT, 0, buf, 2, T2_TIMEOUT);
	else
		ret = usb_control_msg(l.pen, usb_rcvctrlpipe(l.pen, 0), 0x01 /* GET_REPORT */,
				      USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
				      0x0300 | T2_BL_REPORT, 0, buf, 2, T2_TIMEOUT);
	if (ret == 2 && !set)
		*level = buf[1];
	kfree(buf);
	usb_put_dev(l.pen);
	return ret < 0 ? ret : 0;
}

static int t2_bl_update_status(struct backlight_device *bd)
{
	struct mct_t2 *t2 = bl_get_data(bd);
	u8 level = backlight_get_brightness(bd);

	return t2_bl_xfer(t2, true, &level);
}

static int t2_bl_get_brightness(struct backlight_device *bd)
{
	struct mct_t2 *t2 = bl_get_data(bd);
	u8 level = 0;
	int ret;

	ret = t2_bl_xfer(t2, false, &level);
	if (ret)
		return ret;
	return min_t(int, level, T2_BL_MAX);
}

static const struct backlight_ops t2_bl_ops = {
	.update_status = t2_bl_update_status,
	.get_brightness = t2_bl_get_brightness,
};

static void t2_backlight_init(struct mct_t2 *t2, struct usb_interface *interface)
{
	struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.max_brightness = T2_BL_MAX,
		.brightness = 7,	/* power-on default */
		.scale = BACKLIGHT_SCALE_NON_LINEAR,
	};
	struct backlight_device *bd;
	char name[32];
	u8 level;

	/*
	 * After a replug the pen function may enumerate after us, so the pen
	 * is looked up on every access; keep the default if it isn't there yet.
	 */
	if (!t2_bl_xfer(t2, false, &level))
		props.brightness = min_t(int, level, T2_BL_MAX);
	snprintf(name, sizeof(name), "mct_t2-%d", t2->dev.primary->index);
	bd = devm_backlight_device_register(&interface->dev, name, &interface->dev, t2,
					    &t2_bl_ops, &props);
	if (IS_ERR(bd))
		drm_warn(&t2->dev, "backlight registration failed: %ld\n", PTR_ERR(bd));
}

/* ------------------------------------------------------------------ */
/* frame updates						      */

static void t2_update_work(struct work_struct *work)
{
	struct mct_t2 *t2 = container_of(work, struct mct_t2, upd.work);
	struct drm_rect rect;
	bool full;
	int idx, ret;

	mutex_lock(&t2->upd.lock);
	if (!t2->upd.dirty) {
		mutex_unlock(&t2->upd.lock);
		return;
	}
	rect = t2->upd.rect;
	t2->upd.dirty = false;
	mutex_unlock(&t2->upd.lock);

	if (!drm_dev_enter(&t2->dev, &idx))
		return;

	/*
	 * Encoding reads the shadow without upd.lock: a concurrent update may
	 * tear this transfer, but it re-queues its own damage right after.
	 */
	full = t2->first || (long)drm_rect_width(&rect) * drm_rect_height(&rect) * T2_FULL_DIV >
			    (long)T2_WIDTH * T2_HEIGHT;
	mutex_lock(&t2->io_lock);
	ret = full ? t2_show_full(t2) : t2_show_partial(t2, &rect);
	mutex_unlock(&t2->io_lock);
	if (!ret)
		t2->first = false;
	else if (ret != -ENODEV && ret != -ECONNRESET && ret != -ESHUTDOWN)
		drm_err_ratelimited(&t2->dev, "frame update error: %d\n", ret);

	drm_dev_exit(idx);
}

/* copy damage from the (mapped) framebuffer into the shadow and kick the worker */
static void t2_mark_dirty(struct mct_t2 *t2, struct drm_framebuffer *fb,
			  const struct iosys_map *map, const struct drm_rect *dirty)
{
	const u8 *src = map->vaddr; /* shmem GEM: always a system-memory mapping */
	int y;

	mutex_lock(&t2->upd.lock);
	for (y = dirty->y1; y < dirty->y2; y++)
		memcpy(t2->shadow + (y * T2_WIDTH + dirty->x1) * 4,
		       src + y * fb->pitches[0] + dirty->x1 * 4,
		       drm_rect_width(dirty) * 4);
	if (t2->upd.dirty) {
		struct drm_rect *r = &t2->upd.rect;

		r->x1 = min(r->x1, dirty->x1);
		r->y1 = min(r->y1, dirty->y1);
		r->x2 = max(r->x2, dirty->x2);
		r->y2 = max(r->y2, dirty->y2);
	} else {
		t2->upd.rect = *dirty;
		t2->upd.dirty = true;
	}
	mutex_unlock(&t2->upd.lock);

	queue_work(system_long_wq, &t2->upd.work);
}

static void t2_stop_updates(struct mct_t2 *t2)
{
	cancel_work_sync(&t2->upd.work);
	mutex_lock(&t2->upd.lock);
	t2->upd.dirty = false;
	mutex_unlock(&t2->upd.lock);
}

/* ------------------------------------------------------------------ */
/* primary plane						      */

static const u32 t2_primary_formats[] = {
	DRM_FORMAT_XRGB8888,
};

static const u64 t2_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID
};

static int t2_primary_atomic_check(struct drm_plane *plane, struct drm_atomic_commit *state)
{
	struct drm_plane_state *new_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc_state *crtc_state = NULL;

	if (new_state->crtc)
		crtc_state = drm_atomic_get_new_crtc_state(state, new_state->crtc);
	return drm_atomic_helper_check_plane_state(new_state, crtc_state,
						   DRM_PLANE_NO_SCALING, DRM_PLANE_NO_SCALING,
						   false, false);
}

static void t2_primary_atomic_update(struct drm_plane *plane, struct drm_atomic_commit *state)
{
	struct mct_t2 *t2 = to_t2(plane->dev);
	struct drm_plane_state *new_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_plane_state *old_state = drm_atomic_get_old_plane_state(state, plane);
	struct drm_shadow_plane_state *shadow = to_drm_shadow_plane_state(new_state);
	struct drm_framebuffer *fb = new_state->fb;
	struct drm_rect rect;

	if (!fb || !new_state->visible)
		return;
	if (!drm_atomic_helper_damage_merged(old_state, new_state, &rect))
		return;
	if (drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE))
		return;
	t2_mark_dirty(t2, fb, &shadow->data[0], &rect);
	drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);
}

static const struct drm_plane_helper_funcs t2_primary_helper_funcs = {
	DRM_GEM_SHADOW_PLANE_HELPER_FUNCS,
	.atomic_check = t2_primary_atomic_check,
	.atomic_update = t2_primary_atomic_update,
};

static const struct drm_plane_funcs t2_primary_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	DRM_GEM_SHADOW_PLANE_FUNCS,
};

/* ------------------------------------------------------------------ */
/* cursor plane (hardware cursor, see docs/protocol-t2.md)	      */

static const u32 t2_cursor_formats[] = {
	DRM_FORMAT_ARGB8888,
};

/* EP4 register list: position (device top-left of the 64x64 box) + show/hide */
static int t2_cursor_regs(struct mct_t2 *t2, int x, int y, bool show)
{
	u8 *b = t2->cur.buf;
	int n = 0;

	b[n++] = 0x05;
	b[n++] = 0;	/* payload length, filled in below */
	if (!t2->cur.sized) {
		b[n++] = 0x34; b[n++] = T2_CUR;
		b[n++] = 0x35; b[n++] = T2_CUR;
	}
	b[n++] = 0x38; b[n++] = x & 0xff;
	b[n++] = 0x39; b[n++] = x >> 8;
	b[n++] = 0x3a; b[n++] = y & 0xff;
	b[n++] = 0x3b; b[n++] = y >> 8;
	b[n++] = 0x44; b[n++] = 0;
	b[n++] = 0x45; b[n++] = 0;
	b[n++] = 0x36; b[n++] = show ? 0x07 : 0x04;
	b[1] = n - 2;
	if (t2_xfer(t2, b, T2_EP_MCU, b, n))
		return -EIO;
	t2->cur.sized = true;
	t2->cur.visible = show;
	return 0;
}

static int t2_cursor_hide(struct mct_t2 *t2)
{
	u8 *b = t2->cur.buf;

	if (!t2->cur.visible)
		return 0;
	b[0] = 0x05; b[1] = 0x02; b[2] = 0x36; b[3] = 0x04;
	t2->cur.visible = false;
	return t2_xfer(t2, b, T2_EP_MCU, b, 4);
}

/* 0x12 upload of t2->cur.img (header verbatim from the vendor driver) */
static int t2_cursor_upload(struct mct_t2 *t2)
{
	static const u8 hdr[14] = {
		0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x40, 0x00, 0x00, 0x01, 0x25,
	};
	int ret;

	mutex_lock(&t2->io_lock);
	ret = t2_out(t2, T2_EP_FB, hdr, sizeof(hdr));
	if (!ret)
		ret = t2_out(t2, T2_EP_FB, t2->cur.img, T2_CUR_BYTES);
	mutex_unlock(&t2->io_lock);
	if (!ret) {
		memcpy(t2->cur.shown, t2->cur.img, T2_CUR_BYTES);
		t2->cur.uploaded = true;
	}
	return ret;
}

static int t2_cursor_atomic_check(struct drm_plane *plane, struct drm_atomic_commit *state)
{
	struct drm_plane_state *new_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_framebuffer *fb = new_state->fb;
	struct drm_crtc_state *crtc_state = NULL;

	if (new_state->crtc)
		crtc_state = drm_atomic_get_new_crtc_state(state, new_state->crtc);
	if (fb && (fb->width > T2_CUR || fb->height > T2_CUR))
		return -EINVAL;
	return drm_atomic_helper_check_plane_state(new_state, crtc_state,
						   DRM_PLANE_NO_SCALING, DRM_PLANE_NO_SCALING,
						   true, true);
}

static void t2_cursor_atomic_update(struct drm_plane *plane, struct drm_atomic_commit *state)
{
	struct mct_t2 *t2 = to_t2(plane->dev);
	struct drm_plane_state *new_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_shadow_plane_state *shadow = to_drm_shadow_plane_state(new_state);
	struct drm_framebuffer *fb = new_state->fb;
	int dx, dy, sx, sy, i, j, idx;
	const u8 *src;
	u8 *dst;

	if (!drm_dev_enter(plane->dev, &idx))
		return;

	if (!fb || !new_state->visible) {
		t2_cursor_hide(t2);
		goto out;
	}

	/*
	 * The visible 64x64 box at (crtc_x, crtc_y) lands, rotated by 180
	 * degrees, at device (dx, dy). Coordinates must be >= 0: if the box
	 * hangs off the visible right/bottom edge, shift the image instead.
	 */
	dx = T2_WIDTH - (new_state->crtc_x + T2_CUR);
	dy = T2_HEIGHT - (new_state->crtc_y + T2_CUR);
	sx = max(-dx, 0);
	sy = max(-dy, 0);
	dx = max(dx, 0);
	dy = max(dy, 0);
	if (sx >= T2_CUR || sy >= T2_CUR || dx >= T2_WIDTH || dy >= T2_HEIGHT) {
		t2_cursor_hide(t2);
		goto out;
	}

	/* ARGB8888 (visible orientation) -> A,B,G,R bytes, rotated 180, shifted */
	if (drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE))
		goto out;
	src = shadow->data[0].vaddr;
	dst = t2->cur.img;
	for (j = 0; j < T2_CUR; j++) {
		for (i = 0; i < T2_CUR; i++, dst += 4) {
			/* device (i + sx, j + sy) is visible (63 - (i + sx), 63 - (j + sy)) */
			int vx = T2_CUR - 1 - (i + sx), vy = T2_CUR - 1 - (j + sy);
			const u8 *px;

			if (vx < 0 || vy < 0 || vx >= fb->width || vy >= fb->height) {
				memset(dst, 0, 4);
				continue;
			}
			px = src + vy * fb->pitches[0] + vx * 4;	/* B, G, R, A */
			dst[0] = min_t(u8, px[3], 0xfe);	/* vendor uses 0xfe for opaque */
			dst[1] = px[0];
			dst[2] = px[1];
			dst[3] = px[2];
		}
	}
	drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);

	if (!t2->cur.uploaded || memcmp(t2->cur.img, t2->cur.shown, T2_CUR_BYTES))
		if (t2_cursor_upload(t2))
			goto out;
	t2_cursor_regs(t2, dx, dy, true);
out:
	drm_dev_exit(idx);
}

static const struct drm_plane_helper_funcs t2_cursor_helper_funcs = {
	DRM_GEM_SHADOW_PLANE_HELPER_FUNCS,
	.atomic_check = t2_cursor_atomic_check,
	.atomic_update = t2_cursor_atomic_update,
};

static const struct drm_plane_funcs t2_cursor_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	DRM_GEM_SHADOW_PLANE_FUNCS,
};

/* ------------------------------------------------------------------ */
/* CRTC / encoder / connector					      */

static void t2_crtc_atomic_enable(struct drm_crtc *crtc, struct drm_atomic_commit *state)
{
	struct mct_t2 *t2 = to_t2(crtc->dev);
	int idx;

	t2->first = true;	/* primary plane update follows with full damage */
	if (drm_dev_enter(crtc->dev, &idx)) {
		t2_wreg(t2, T2_REG_PLL_EN, 1);
		drm_dev_exit(idx);
	}
	drm_crtc_vblank_on(crtc);
}

static void t2_crtc_atomic_disable(struct drm_crtc *crtc, struct drm_atomic_commit *state)
{
	struct mct_t2 *t2 = to_t2(crtc->dev);
	int idx;

	drm_crtc_vblank_off(crtc);
	t2_stop_updates(t2);
	if (drm_dev_enter(crtc->dev, &idx)) {
		t2_cursor_hide(t2);
		t2_wreg(t2, T2_REG_PLL_EN, 0);	/* panel/backlight off */
		drm_dev_exit(idx);
	}
}

/*
 * The device has no vblank interrupt; a 60 Hz hrtimer (DRM vblank timer
 * helpers) paces page-flip events so compositors don't render faster than
 * the panel refreshes.
 */
static const struct drm_crtc_helper_funcs t2_crtc_helper_funcs = {
	.atomic_check = drm_crtc_helper_atomic_check,
	.atomic_flush = drm_crtc_vblank_atomic_flush,
	.atomic_enable = t2_crtc_atomic_enable,
	.atomic_disable = t2_crtc_atomic_disable,
};

static const struct drm_crtc_funcs t2_crtc_funcs = {
	.reset = drm_atomic_helper_crtc_reset,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
	DRM_CRTC_VBLANK_TIMER_FUNCS,
};

static const struct drm_encoder_funcs t2_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

static int t2_conn_get_modes(struct drm_connector *connector)
{
	struct mct_t2 *t2 = to_t2(connector->dev);
	int count = 0;

	if (t2->edid) {
		drm_edid_connector_update(connector, t2->edid);
		count = drm_edid_connector_add_modes(connector);
	}
	if (!count) {
		drm_edid_connector_update(connector, NULL);
		count = drm_add_modes_noedid(connector, T2_WIDTH, T2_HEIGHT);
		drm_set_preferred_mode(connector, T2_WIDTH, T2_HEIGHT);
	}
	return count;
}

static enum drm_mode_status t2_conn_mode_valid(struct drm_connector *connector,
					       const struct drm_display_mode *mode)
{
	if (mode->hdisplay != T2_WIDTH || mode->vdisplay != T2_HEIGHT)
		return MODE_BAD;
	return MODE_OK;
}

static const struct drm_connector_helper_funcs t2_conn_helper_funcs = {
	.get_modes = t2_conn_get_modes,
	.mode_valid = t2_conn_mode_valid,
};

static const struct drm_connector_funcs t2_conn_funcs = {
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

/* ------------------------------------------------------------------ */
/* driver							      */

DEFINE_DRM_GEM_FOPS(t2_fops);

static const struct drm_driver t2_drm_driver = {
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC,
	.name = DRIVER_NAME,
	.desc = DRIVER_DESC,
	.major = DRIVER_MAJOR,
	.minor = DRIVER_MINOR,
	.patchlevel = DRIVER_PATCH,
	.fops = &t2_fops,
	DRM_GEM_SHMEM_DRIVER_OPS,
	DRM_FBDEV_SHMEM_DRIVER_OPS,
};

static const struct drm_mode_config_funcs t2_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static int t2_modeset_init(struct mct_t2 *t2)
{
	struct drm_device *dev = &t2->dev;
	int ret;

	ret = drmm_mode_config_init(dev);
	if (ret)
		return ret;
	dev->mode_config.min_width = T2_WIDTH;
	dev->mode_config.max_width = T2_WIDTH;
	dev->mode_config.min_height = T2_HEIGHT;
	dev->mode_config.max_height = T2_HEIGHT;
	dev->mode_config.cursor_width = T2_CUR;
	dev->mode_config.cursor_height = T2_CUR;
	dev->mode_config.funcs = &t2_mode_config_funcs;

	ret = drm_universal_plane_init(dev, &t2->primary, 0, &t2_primary_funcs,
				       t2_primary_formats, ARRAY_SIZE(t2_primary_formats),
				       t2_modifiers, DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ret;
	drm_plane_helper_add(&t2->primary, &t2_primary_helper_funcs);
	/* let userspace pass damage rects so small changes become partial updates */
	drm_plane_enable_fb_damage_clips(&t2->primary);

	ret = drm_universal_plane_init(dev, &t2->cursor, 0, &t2_cursor_funcs,
				       t2_cursor_formats, ARRAY_SIZE(t2_cursor_formats),
				       t2_modifiers, DRM_PLANE_TYPE_CURSOR, NULL);
	if (ret)
		return ret;
	drm_plane_helper_add(&t2->cursor, &t2_cursor_helper_funcs);

	ret = drm_crtc_init_with_planes(dev, &t2->crtc, &t2->primary, &t2->cursor,
					&t2_crtc_funcs, NULL);
	if (ret)
		return ret;
	drm_crtc_helper_add(&t2->crtc, &t2_crtc_helper_funcs);

	ret = drm_encoder_init(dev, &t2->encoder, &t2_encoder_funcs, DRM_MODE_ENCODER_NONE, NULL);
	if (ret)
		return ret;
	t2->encoder.possible_crtcs = drm_crtc_mask(&t2->crtc);

	drm_connector_helper_add(&t2->conn, &t2_conn_helper_funcs);
	ret = drm_connector_init(dev, &t2->conn, &t2_conn_funcs, DRM_MODE_CONNECTOR_USB);
	if (ret)
		return ret;
	ret = drm_connector_attach_encoder(&t2->conn, &t2->encoder);
	if (ret)
		return ret;

	ret = drm_vblank_init(dev, 1);
	if (ret)
		return ret;

	drm_mode_config_reset(dev);
	return 0;
}

static void t2_release(struct drm_device *dev, void *res)
{
	struct mct_t2 *t2 = to_t2(dev);

	kvfree(t2->enc);
	kvfree(t2->shadow);
	drm_edid_free(t2->edid);
}

static int t2_usb_probe(struct usb_interface *interface, const struct usb_device_id *id)
{
	struct mct_t2 *t2;
	struct drm_device *dev;
	struct device *dma_dev;
	int ret;

	if (interface->cur_altsetting->desc.bInterfaceNumber != 0)
		return -ENODEV;

	t2 = devm_drm_dev_alloc(&interface->dev, &t2_drm_driver, struct mct_t2, dev);
	if (IS_ERR(t2))
		return PTR_ERR(t2);
	dev = &t2->dev;

	dma_dev = usb_intf_get_dma_device(interface);
	if (dma_dev) {
		drm_dev_set_dma_dev(dev, dma_dev);
		put_device(dma_dev);
	} else {
		drm_warn(dev, "buffer sharing not supported");
	}

	INIT_WORK(&t2->upd.work, t2_update_work);
	mutex_init(&t2->upd.lock);
	mutex_init(&t2->io_lock);
	t2->front = T2_FB_A;

	t2->bounce = drmm_kmalloc(dev, T2_BOUNCE, GFP_KERNEL);
	t2->cur.buf = drmm_kmalloc(dev, 64, GFP_KERNEL);
	t2->cur.img = drmm_kmalloc(dev, T2_CUR_BYTES, GFP_KERNEL);
	t2->cur.shown = drmm_kmalloc(dev, T2_CUR_BYTES, GFP_KERNEL);
	t2->enc = kvmalloc(T2_ENC_MAX, GFP_KERNEL);
	t2->shadow = kvzalloc(T2_WIDTH * T2_HEIGHT * 4, GFP_KERNEL);
	ret = drmm_add_action_or_reset(dev, t2_release, NULL);
	if (ret)
		return ret;
	if (!t2->bounce || !t2->cur.buf || !t2->cur.img || !t2->cur.shown ||
	    !t2->enc || !t2->shadow)
		return -ENOMEM;

	ret = t2_hw_init(t2);
	if (ret)
		return ret;

	ret = t2_modeset_init(t2);
	if (ret)
		return ret;

	usb_set_intfdata(interface, dev);
	ret = drm_dev_register(dev, 0);
	if (ret)
		return ret;

	drm_client_setup(dev, NULL);
	t2_backlight_init(t2, interface);
	return 0;
}

static void t2_usb_disconnect(struct usb_interface *interface)
{
	struct drm_device *dev = usb_get_intfdata(interface);

	drm_dev_unplug(dev);
	drm_atomic_helper_shutdown(dev);
}

static int t2_suspend(struct usb_interface *interface, pm_message_t message)
{
	return drm_mode_config_helper_suspend(usb_get_intfdata(interface));
}

static int t2_resume(struct usb_interface *interface)
{
	struct drm_device *dev = usb_get_intfdata(interface);
	int ret;

	ret = t2_hw_init(to_t2(dev));
	if (ret)
		return ret;
	return drm_mode_config_helper_resume(dev);
}

static const struct usb_device_id id_table[] = {
	/* Wacom DTU-1031X; it has been seen enumerating with either ID */
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5209, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x520d, 0) },
	{},
};
MODULE_DEVICE_TABLE(usb, id_table);

static struct usb_driver t2_usb_driver = {
	.name = DRIVER_NAME,
	.probe = t2_usb_probe,
	.disconnect = t2_usb_disconnect,
	.id_table = id_table,
	.suspend = pm_ptr(t2_suspend),
	.resume = pm_ptr(t2_resume),
	.reset_resume = pm_ptr(t2_resume),
};

module_usb_driver(t2_usb_driver);
MODULE_DESCRIPTION("DRM driver for MCT Trigger II USB displays (Wacom DTU-1031X)");
MODULE_LICENSE("GPL");
