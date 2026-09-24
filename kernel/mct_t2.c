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
 *  - the panel scans out rotated by 180 degrees
 *
 * Modelled on drivers/gpu/drm/tiny/gm12u320.c.
 */

#include <linux/gcd.h>
#include <linux/module.h>
#include <linux/pm.h>
#include <linux/usb.h>
#include <linux/unaligned.h>
#include <linux/vmalloc.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
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
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>

#include "mct_t2_init.h"

#define DRIVER_NAME	"mct_t2"
#define DRIVER_DESC	"MCT Trigger II USB display"
#define DRIVER_MAJOR	0
#define DRIVER_MINOR	1

#define T2_WIDTH	1024
#define T2_HEIGHT	600
#define T2_STRIDE	(T2_WIDTH * 3)		/* device framebuffer: packed RGB24 */
#define T2_FB_A		0x00c000u
#define T2_FB_B		(T2_FB_A + T2_STRIDE * T2_HEIGHT)

#define T2_EP_FB	0x02
#define T2_EP_IN	0x81

#define T2_CHUNK	19968			/* payload transfer size used by the vendor driver */
#define T2_BOUNCE	65536
#define T2_ENC_MAX	(T2_WIDTH * (T2_HEIGHT + 16) * 3 + 4096)
#define T2_TIMEOUT	2000

/* full frame + flip when the damage covers more than 1/FULL_DIV of the screen */
#define T2_FULL_DIV	3

struct mct_t2 {
	struct drm_device dev;
	struct drm_simple_display_pipe pipe;
	struct drm_connector conn;

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
};

#define to_t2(__dev) container_of(__dev, struct mct_t2, dev)

static struct usb_device *t2_udev(struct mct_t2 *t2)
{
	return interface_to_usbdev(to_usb_interface(t2->dev.dev));
}

/* ------------------------------------------------------------------ */
/* USB								      */

static int t2_out(struct mct_t2 *t2, u8 ep, const void *data, int len)
{
	struct usb_device *udev = t2_udev(t2);
	int ret, done;

	if (WARN_ON(len > T2_BOUNCE))
		return -EINVAL;
	if (data)
		memcpy(t2->bounce, data, len);
	else
		memset(t2->bounce, 0, len);
	ret = usb_bulk_msg(udev, usb_sndbulkpipe(udev, ep), t2->bounce, len, &done, T2_TIMEOUT);
	if (!ret && done != len)
		ret = -EIO;
	return ret;
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

/* header in its own transfer, payload in T2_CHUNK transfers */
static int t2_cmd(struct mct_t2 *t2, const u8 *hdr, int hlen, const u8 *pay, int plen)
{
	int off, ret;

	ret = t2_out(t2, T2_EP_FB, hdr, hlen);
	for (off = 0; !ret && off < plen; off += T2_CHUNK)
		ret = t2_out(t2, T2_EP_FB, pay + off, min(plen - off, T2_CHUNK));
	return ret;
}

static int t2_hw_init(struct mct_t2 *t2)
{
	const u8 *p = mct_t2_init_seq, *end = p + sizeof(mct_t2_init_seq);
	int n = 0, ret;

	while (p + 6 <= end) {
		u8 ep = p[0], flags = p[1];
		u32 len = get_unaligned_le32(p + 2);
		const u8 *data = NULL;
		u32 rdlen = 0;

		p += 6;
		if (!(flags & 1)) {
			data = p;
			p += len;
		}
		if (flags & 2) {
			rdlen = get_unaligned_le32(p);
			p += 4;
		}
		if (p > end)
			return -EINVAL;

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
		/* the 8051 firmware needs pacing on the register pipes */
		if (ep != T2_EP_FB || len < 64)
			usleep_range(ep == T2_EP_FB ? 5000 : 20000, 25000);
		n++;
	}
	drm_dbg(&t2->dev, "init: %d transfers\n", n);
	return 0;
err:
	drm_err(&t2->dev, "init failed at transfer %d: %d\n", n, ret);
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
	static u8 ch[3][1024];	/* only used from the (single) update worker */
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
				ch[0][k] = ch[1][k] = ch[2][k] = 0;
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
	 * Encoding reads the shadow without the lock: a concurrent update may
	 * tear this transfer, but it re-queues its own damage right after.
	 */
	full = t2->first || (long)drm_rect_width(&rect) * drm_rect_height(&rect) * T2_FULL_DIV >
			    (long)T2_WIDTH * T2_HEIGHT;
	ret = full ? t2_show_full(t2) : t2_show_partial(t2, &rect);
	if (!ret)
		t2->first = false;
	else if (ret != -ENODEV && ret != -ECONNRESET && ret != -ESHUTDOWN)
		drm_err(&t2->dev, "frame update error: %d\n", ret);

	drm_dev_exit(idx);
}

/* copy damage from the (mapped) framebuffer into the shadow and kick the worker */
static void t2_mark_dirty(struct drm_framebuffer *fb, const struct iosys_map *map,
			  const struct drm_rect *dirty)
{
	struct mct_t2 *t2 = to_t2(fb->dev);
	const u8 *src = map->vaddr; /* shmem GEM: always a system-memory mapping */
	int y;

	if (drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE))
		return;

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

	drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);
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
/* connector							      */

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
/* display pipe							      */

static void t2_pipe_enable(struct drm_simple_display_pipe *pipe,
			   struct drm_crtc_state *crtc_state,
			   struct drm_plane_state *plane_state)
{
	struct mct_t2 *t2 = to_t2(pipe->crtc.dev);
	struct drm_shadow_plane_state *shadow = to_drm_shadow_plane_state(plane_state);
	struct drm_rect rect = DRM_RECT_INIT(0, 0, T2_WIDTH, T2_HEIGHT);

	t2->first = true;
	t2_mark_dirty(plane_state->fb, &shadow->data[0], &rect);
}

static void t2_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct mct_t2 *t2 = to_t2(pipe->crtc.dev);

	t2_stop_updates(t2);
}

static void t2_pipe_update(struct drm_simple_display_pipe *pipe,
			   struct drm_plane_state *old_state)
{
	struct drm_plane_state *state = pipe->plane.state;
	struct drm_shadow_plane_state *shadow = to_drm_shadow_plane_state(state);
	struct drm_rect rect;

	if (state->fb && drm_atomic_helper_damage_merged(old_state, state, &rect))
		t2_mark_dirty(state->fb, &shadow->data[0], &rect);
}

static const struct drm_simple_display_pipe_funcs t2_pipe_funcs = {
	.enable = t2_pipe_enable,
	.disable = t2_pipe_disable,
	.update = t2_pipe_update,
	DRM_GEM_SIMPLE_DISPLAY_PIPE_SHADOW_PLANE_FUNCS,
};

static const u32 t2_formats[] = {
	DRM_FORMAT_XRGB8888,
};

static const u64 t2_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID
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
	.fops = &t2_fops,
	DRM_GEM_SHMEM_DRIVER_OPS,
	DRM_FBDEV_SHMEM_DRIVER_OPS,
};

static const struct drm_mode_config_funcs t2_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

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
	t2->front = T2_FB_A;

	t2->bounce = drmm_kmalloc(dev, T2_BOUNCE, GFP_KERNEL);
	t2->enc = kvmalloc(T2_ENC_MAX, GFP_KERNEL);
	t2->shadow = kvzalloc(T2_WIDTH * T2_HEIGHT * 4, GFP_KERNEL);
	ret = drmm_add_action_or_reset(dev, t2_release, NULL);
	if (ret)
		return ret;
	if (!t2->bounce || !t2->enc || !t2->shadow)
		return -ENOMEM;

	ret = t2_hw_init(t2);
	if (ret)
		return ret;

	ret = drmm_mode_config_init(dev);
	if (ret)
		return ret;
	dev->mode_config.min_width = T2_WIDTH;
	dev->mode_config.max_width = T2_WIDTH;
	dev->mode_config.min_height = T2_HEIGHT;
	dev->mode_config.max_height = T2_HEIGHT;
	dev->mode_config.funcs = &t2_mode_config_funcs;

	drm_connector_helper_add(&t2->conn, &t2_conn_helper_funcs);
	ret = drm_connector_init(dev, &t2->conn, &t2_conn_funcs, DRM_MODE_CONNECTOR_USB);
	if (ret)
		return ret;

	ret = drm_simple_display_pipe_init(dev, &t2->pipe, &t2_pipe_funcs,
					   t2_formats, ARRAY_SIZE(t2_formats),
					   t2_modifiers, &t2->conn);
	if (ret)
		return ret;
	/* let userspace pass damage rects so small changes become partial updates */
	drm_plane_enable_fb_damage_clips(&t2->pipe.plane);

	drm_mode_config_reset(dev);

	usb_set_intfdata(interface, dev);
	ret = drm_dev_register(dev, 0);
	if (ret)
		return ret;

	drm_client_setup(dev, NULL);
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
	struct mct_t2 *t2 = to_t2(dev);
	int ret;

	ret = t2_hw_init(t2);
	if (ret)
		return ret;
	t2->front = T2_FB_A;
	return drm_mode_config_helper_resume(dev);
}

static const struct usb_device_id id_table[] = {
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x520d, 0) },	/* Wacom DTU-1031X */
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
