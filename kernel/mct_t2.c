// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Filip Piękoś <mail@filippiekos.com>
 *
 * DRM driver for MCT Trigger II (T2) USB displays, e.g. the Wacom DTU-1031X
 * pen display (USB 0711:520d, 1024x600).
 *
 * Protocol (reverse engineered, see docs/reference/protocol-t2.md):
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
 * Backlight brightness 0 turns the panel off the same way DPMS does.
 *
 * Several things here are specific to the DTU-1031X rather than to the
 * Trigger II: the fixed 1024x600 panel and its 180
 * degree scanout, the init sequence (replayed from Wacom's Windows driver,
 * possibly with board settings), the brightness through the pen function,
 * and the USB IDs. A plain T2 adapter (VGA/DVI) would need modes from EDID,
 * no rotation and likely a different init; these should move into a device
 * profile once such a device can be compared.
 *
 * Structure modelled on drivers/gpu/drm/udl and drivers/gpu/drm/tiny/gm12u320.c.
 */

#include <linux/backlight.h>
#include <linux/gcd.h>
#include <linux/seq_file.h>
#include <linux/module.h>
#include <linux/pm.h>
#include <linux/usb.h>
#include <linux/unaligned.h>
#include <linux/vmalloc.h>

#include <drm/drm_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_fbdev_shmem.h>
#include <drm/drm_file.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_print.h>

#include "mct.h"
#include "mct_drv.h"
#include "mct_t2_init.h"

/* default for devices probed later; each device has its own in sysfs */
static int lossy;
module_param(lossy, int, 0644);
MODULE_PARM_DESC(lossy, "Default for new devices: maximum lossy tolerance (1-8) for updates too big to send losslessly within a frame, refined to lossless once motion stops; 0 = always lossless (default). Per device: /sys/bus/usb/devices/<interface>/mct/lossy.");

#define DRIVER_NAME	"mct_t2"
#define DRIVER_DESC	"MCT Trigger II USB display"
#define DRIVER_MAJOR	0
#define DRIVER_MINOR	3
#define DRIVER_PATCH	21

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

/* rows of unchanged pixels that still get merged into one update rect */
#define T2_BAND_GAP	16
#define T2_MAX_BANDS	16

/* refine lossy frames to lossless this long after the last lossy update */
#define T2_REFINE_MS	150
/* bytes the device takes in one 60 Hz frame (~38 MB/s); beyond this, go lossy */
#define T2_FRAME_BUDGET	(600 * 1024)

struct mct_t2 {
	struct mct_device base;		/* DRM side, see mct_drv.h */

	struct mutex io_lock;		/* EP2 command sequences + bounce */
	u8 *bounce;			/* DMA-able transfer buffer */
	u8 *enc;			/* RLE output */
	u8 *snap;
	u8 enc_ch[3][1024];		/* t2_encode scratch, io_lock held */
	u8 *enc_pix[1024];			/* shadow snapshot being sent */
	u8 *hw[2];			/* what device buffers A and B hold */
	bool hw_valid[2];

	struct {			/* debugfs statistics */
		u64 lossy_tol[9];	/* lossy updates by highest tolerance used */
		u64 updates, lossy_updates, rects, px_sent, bytes, errors;
		u64 over_budget, max_payload;	/* encoded bytes per update */
		u64 latency[6];			/* damage to flip: <8 <16 <33 <50 <100 >=100 ms */
		u64 ns_diff, ns_encode, ns_usb;	/* time spent per stage */
	} stats;
	const struct drm_edid *edid;	/* read from the device at init */
	struct backlight_device *bl;
	char bl_name[32];		/* for sysfs; empty without a backlight */
	struct mutex power_lock;		/* crtc_on + bl_off -> pixel clock */
	bool crtc_on, bl_off;
	u32 front;			/* framebuffer address currently scanned out */

	int refine;			/* lossless updates still owed, under base.lock */
	int lossy;			/* sysfs lossy: maximum tolerance, 0 = lossless */

	struct {
		u8 *buf;		/* DMA-able: position packets */
		u8 *img;		/* device-format image being built */
		u8 *shown;		/* image last uploaded */
		bool uploaded, sized, visible;
	} cur;
};

#define to_t2(m) container_of(m, struct mct_t2, base)

static struct usb_device *t2_udev(struct mct_t2 *t2)
{
	return interface_to_usbdev(to_usb_interface(t2->base.dev.dev));
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
/* RLE encoder (see docs/reference/protocol-t2.md)			      */

static inline u8 t2_rle_val(u8 v)
{
	return v == 0x30 ? 0x31 : v;	/* 0x30 is the escape byte */
}

/*
 * One channel line of n values; runs "30 NN VV" = NN+1 x VV. With tol > 0,
 * values within +-tol of a run's first value join the run (lossy, like the
 * vendor encoder) and v is overwritten with what the device will show.
 */
static int t2_rle_line(u8 *v, int n, u8 *out, int tol)
{
	u8 *o = out;
	int x = 0;

	while (x < n) {
		u8 c = t2_rle_val(v[x]);
		int r = 1, i;

		while (x + r < n && r < 0xfb && abs(t2_rle_val(v[x + r]) - c) <= tol)
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
			if (tol)
				memset(v + x, c, r);
		} else {
			for (i = 0; i < r; i++) {
				*o++ = t2_rle_val(v[x + i]);
				if (tol)
					v[x + i] = t2_rle_val(v[x + i]);
			}
		}
		x += r;
	}
	return o - out;
}

/*
 * Encode device rect (dx, dy, w, h) from a visible-orientation XRGB8888 image,
 * rotating by 180 degrees. Pixels stream row-major in 1024-pixel units, each
 * unit as R, G, B lines. Rows past the panel (alignment overshoot) are black.
 */
static int t2_encode(struct mct_t2 *t2, u8 *src, int dx, int dy, int w, int h, int tol)
{
	u8 (*ch)[1024] = t2->enc_ch;	/* io_lock held */
	u8 **pix = t2->enc_pix;
	int x, y, k = 0, len = 0, c, i;

	for (y = dy; y < dy + h; y++) {
		for (x = dx; x < dx + w; x++) {
			if (y < T2_HEIGHT) {
				u8 *px = src +
					((T2_HEIGHT - 1 - y) * T2_WIDTH + (T2_WIDTH - 1 - x)) * 4;

				pix[k] = px;
				ch[0][k] = px[2];
				ch[1][k] = px[1];
				ch[2][k] = px[0];
			} else {
				pix[k] = NULL;
				ch[0][k] = 0;
				ch[1][k] = 0;
				ch[2][k] = 0;
			}
			if (++k == 1024) {
				for (c = 0; c < 3; c++)
					len += t2_rle_line(ch[c], 1024, t2->enc + len, tol);
				/* lossy: keep src equal to what the device now holds */
				for (i = 0; tol && i < 1024; i++) {
					if (!pix[i])
						continue;
					pix[i][2] = ch[0][i];
					pix[i][1] = ch[1][i];
					pix[i][0] = ch[2][i];
				}
				k = 0;
			}
		}
	}
	return len;
}

/*
 * Smallest tolerance likely to bring an update of need bytes into the frame
 * budget. Small tolerances already save a lot (tol 2 roughly halves a full
 * frame), so the picture stays close to lossless unless the update is far
 * over budget.
 */
static int t2_pick_tol(long need, int max)
{
	int tol;

	if (need <= T2_FRAME_BUDGET * 4 / 3)
		tol = 1;
	else if (need <= T2_FRAME_BUDGET * 2)
		tol = 2;
	else
		tol = max;
	return min(tol, max);
}

/*
 * tol > 0 allows lossy encoding up to that tolerance, used only if the
 * lossless result would push this update's total (*sent) past the frame
 * budget. Returns the highest tolerance actually used through *used.
 */
static int t2_send_rect(struct mct_t2 *t2, u8 *src, u32 base, int dx, int dy, int w, int h,
			int tol, long *sent, int *used)
{
	u32 a = (base + dy * T2_STRIDE + dx * 3) << 8;
	ktime_t t0 = ktime_get(), t1;
	int len = t2_encode(t2, src, dx, dy, w, h, 0);
	u8 hdr[36];
	int ret;

	if (tol && *sent + len > T2_FRAME_BUDGET) {
		tol = t2_pick_tol(*sent + len, tol);
		len = t2_encode(t2, src, dx, dy, w, h, tol);
		*used = max(*used, tol);
	}
	*sent += len;
	t1 = ktime_get();

	t2->stats.rects++;
	t2->stats.px_sent += w * h;
	t2->stats.bytes += sizeof(hdr) + len;

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
	ret = t2_cmd(t2, hdr, sizeof(hdr), t2->enc, len);
	t2->stats.ns_encode += ktime_to_ns(ktime_sub(t1, t0));
	t2->stats.ns_usb += ktime_to_ns(ktime_sub(ktime_get(), t1));
	return ret;
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

static u32 t2_buf_addr(int buf)
{
	return buf ? T2_FB_B : T2_FB_A;
}

/*
 * Send the visible-space rect r of src into device buffer base. The device
 * wants x and w in 64-pixel units and w * h a multiple of 1024; rows are
 * rounded up but never past the buffer (that would write into the other one).
 */
static int t2_send_visible_rect(struct mct_t2 *t2, u8 *src, u32 base,
				const struct drm_rect *r, int tol, long *sent, int *used)
{
	int dx1 = round_down(T2_WIDTH - r->x2, 64), dx2 = round_up(T2_WIDTH - r->x1, 64);
	int dy1 = T2_HEIGHT - r->y2, rows = drm_rect_height(r);
	int w, h;

	if (roundup(rows, 16 / gcd((dx2 - dx1) / 64, 16)) > T2_HEIGHT && ((dx2 - dx1) / 64) & 1) {
		/* full-height rect: an even number of 64-px columns makes h = 600 legal */
		if (dx2 < T2_WIDTH)
			dx2 += 64;
		else
			dx1 -= 64;
	}
	w = dx2 - dx1;
	h = roundup(rows, 16 / gcd(w / 64, 16));
	if (dy1 + h > T2_HEIGHT)
		dy1 = T2_HEIGHT - h;
	return t2_send_rect(t2, src, base, dx1, dy1, w, h, tol, sent, used);
}

/*
 * Rows of cur that differ from old, grouped into bands (a gap of fewer than
 * T2_BAND_GAP unchanged rows keeps a band going). Returns the band count,
 * 0 if nothing changed.
 */
static int t2_diff_bands(const u8 *cur, const u8 *old, struct drm_rect *bands)
{
	int n = 0, y, x1, x2, last = -T2_BAND_GAP - 1;

	for (y = 0; y < T2_HEIGHT; y++) {
		const u32 *a = (const u32 *)(cur + y * T2_WIDTH * 4);
		const u32 *b = (const u32 *)(old + y * T2_WIDTH * 4);

		if (!memcmp(a, b, T2_WIDTH * 4))
			continue;
		for (x1 = 0; a[x1] == b[x1]; x1++)
			;
		for (x2 = T2_WIDTH; a[x2 - 1] == b[x2 - 1]; x2--)
			;
		if (n && y - last <= T2_BAND_GAP) {
			bands[n - 1].x1 = min(bands[n - 1].x1, x1);
			bands[n - 1].x2 = max(bands[n - 1].x2, x2);
			bands[n - 1].y2 = y + 1;
		} else if (n < T2_MAX_BANDS) {
			drm_rect_init(&bands[n++], x1, y, x2 - x1, 1);
		} else {
			/* too fragmented: one rect spanning everything */
			drm_rect_init(&bands[0], 0, bands[0].y1, T2_WIDTH, y + 1 - bands[0].y1);
			n = 1;
		}
		last = y;
	}
	return n;
}

/*
 * Every update goes to the back buffer followed by a flip, so there is no
 * tearing. Only pixels that differ from what the back buffer already holds
 * (the image from two updates ago) are sent.
 */
static int t2_present(struct mct_t2 *t2, bool lossless, bool *went_lossy)
{
	struct drm_rect bands[T2_MAX_BANDS];
	int back = t2->front == T2_FB_A ? 1 : 0;
	u32 base = t2_buf_addr(back);
	int i, n, tol = 0, used = 0, ret = 0;
	long sent = 0;
	u8 *tmp;

	ktime_t t0 = ktime_get();

	if (t2->hw_valid[back]) {
		n = t2_diff_bands(t2->snap, t2->hw[back], bands);
	} else {
		drm_rect_init(&bands[0], 0, 0, T2_WIDTH, T2_HEIGHT);
		n = 1;
	}
	t2->stats.ns_diff += ktime_to_ns(ktime_sub(ktime_get(), t0));

	/* updates too big for one frame may go lossy; refined once motion stops */
	if (!lossless && READ_ONCE(t2->lossy) > 0)
		tol = READ_ONCE(t2->lossy);

	t2->hw_valid[back] = false;
	for (i = 0; i < n && !ret; i++)
		ret = t2_send_visible_rect(t2, t2->snap, base, &bands[i], tol, &sent, &used);
	if (!ret)
		ret = t2_flip(t2, base);
	if (ret)
		return ret;

	t2->stats.updates++;
	if (used) {
		t2->stats.lossy_updates++;
		t2->stats.lossy_tol[used]++;
	}
	if (sent > T2_FRAME_BUDGET)
		t2->stats.over_budget++;
	t2->stats.max_payload = max_t(u64, t2->stats.max_payload, sent);
	*went_lossy = used;
	t2->front = base;
	tmp = t2->hw[back];
	t2->hw[back] = t2->snap;
	t2->snap = tmp;
	t2->hw_valid[back] = true;
	return 0;
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

/* the pixel clock runs only while the CRTC is on and the backlight isn't off */
static void t2_set_power(struct mct_t2 *t2, bool *flag, bool v)
{
	mutex_lock(&t2->power_lock);
	*flag = v;
	t2_wreg(t2, T2_REG_PLL_EN, t2->crtc_on && !t2->bl_off);
	mutex_unlock(&t2->power_lock);
}

/* ------------------------------------------------------------------ */
/* device init							      */

/* 8051 pacing, validated from a cold chip (docs/reference/init-sequence.md) */
static unsigned int t2_pace_us(u8 ep, const u8 *data)
{
	if (ep == T2_EP_FB)
		return 0;
	if (ep == T2_EP_CMD && data && data[0] == 0x02)		/* single register write */
		return data[3] == 0xfe && (data[4] == 0xb0 || data[4] == 0xb1) ? 50000 : 2000;
	return 20000;
}

static int t2_hw_init_once(struct mct_t2 *t2)
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
			long sent = 0;
			int used = 0;

			ret = t2_send_rect(t2, t2->base.shadow, T2_FB_A, 0, 0,
					   T2_WIDTH, T2_HEIGHT, 0, &sent, &used);
			if (ret)
				goto err;
			t2->front = T2_FB_A;
			memcpy(t2->hw[0], t2->base.shadow, T2_WIDTH * T2_HEIGHT * 4);
			t2->hw_valid[0] = true;
			t2->hw_valid[1] = false;
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
	drm_dbg(&t2->base.dev, "init: %d transfers\n", n);
	return 0;
err:
	mutex_unlock(&t2->io_lock);
	drm_err(&t2->base.dev, "init failed at transfer %d: %d\n", n, ret);
	return ret;
}

/*
 * Recovery for a device left in the middle of a command, e.g. when a
 * previous driver instance stopped halfway through a frame's payload.
 * The firmware handles one command at a time, so while it waits for the
 * rest of a payload it does not answer register reads. Feed it zeros until
 * a pending read is answered; any overshoot is interpreted as commands, but
 * the init sequence that follows puts the frame parser back in step.
 */
#define T2_RESYNC_MAX	(2 * 1024 * 1024)	/* longer than any payload */
#define T2_RESYNC_READ_MS	200
#define T2_RESYNC_POLL_MS	20
/*
 * Zero padding rarely matches the pixel count the cut frame still owed (any
 * RLE run in the lost part breaks it), and the firmware then sits in an
 * internal timeout of about 30 s before it takes commands again. A USB reset
 * doesn't help: the stuck chip stops answering control requests as well.
 */
#define T2_RESYNC_DEADLINE_MS	45000	/* give up and ask for a replug after this */

/* read a register with a short timeout; io_lock held */
static int t2_read_reg(struct mct_t2 *t2, u16 reg, u8 *val, int timeout_ms)
{
	struct usb_device *udev = t2_udev(t2);
	const u8 cmd[3] = { 0x05, reg & 0xff, reg >> 8 };
	int ret, done;

	ret = t2_out(t2, T2_EP_CMD, cmd, sizeof(cmd));
	if (ret)
		return ret;
	ret = usb_bulk_msg(udev, usb_rcvbulkpipe(udev, T2_EP_IN), t2->bounce, 64, &done,
			   timeout_ms);
	if (!ret && done < 1)
		ret = -EIO;
	if (!ret)
		*val = t2->bounce[0];
	return ret;
}

/* discard replies left over from earlier commands; io_lock held */
static void t2_drain_replies(struct mct_t2 *t2)
{
	struct usb_device *udev = t2_udev(t2);
	int done, i;

	for (i = 0; i < 16; i++)
		if (usb_bulk_msg(udev, usb_rcvbulkpipe(udev, T2_EP_IN), t2->bounce, 512, &done, 50))
			break;
}

static int t2_resync(struct mct_t2 *t2)
{
	struct usb_device *udev = t2_udev(t2);
	ktime_t deadline;
	int ret, sent, done;
	u8 val;

	mutex_lock(&t2->io_lock);
	t2_drain_replies(t2);
	ret = t2_read_reg(t2, 0xfeb0, &val, T2_RESYNC_READ_MS);
	if (!ret)
		goto out;
	if (ret != -ETIMEDOUT)
		goto out;

	drm_warn(&t2->base.dev, "device busy in the middle of a command, completing it (may take 30 s)\n");
	deadline = ktime_add_ms(ktime_get(), T2_RESYNC_DEADLINE_MS);
	for (sent = 0; sent < T2_RESYNC_MAX && ktime_before(ktime_get(), deadline);
	     sent += T2_CHUNK) {
		ret = t2_out(t2, T2_EP_FB, NULL, T2_CHUNK);
		/*
		 * A chunk can stall once the payload has completed inside it: the
		 * firmware answers the queued read and then parses the rest of the
		 * zeros as commands. Look for the reply before giving up; a slow
		 * chunk alone is not a failure.
		 */
		if (ret && ret != -ETIMEDOUT)
			goto out;
		/* the read queued above is answered once the payload is complete */
		ret = usb_bulk_msg(udev, usb_rcvbulkpipe(udev, T2_EP_IN), t2->bounce, 64, &done,
				   ret ? T2_TIMEOUT : T2_RESYNC_POLL_MS);
		if (!ret && done >= 1)
			goto recovered;
		if (!ret)
			ret = -EIO;
		if (ret != -ETIMEDOUT)
			goto out;
	}
	drm_err(&t2->base.dev, "device does not respond; unplug it to reset\n");
	ret = -EIO;
	goto out;

recovered:
	drm_info(&t2->base.dev, "device command parser recovered after %d bytes\n",
		 sent + T2_CHUNK);
	t2_drain_replies(t2);
	ret = 0;
out:
	mutex_unlock(&t2->io_lock);
	return ret;
}

/* registers the init sequence leaves at known values */
static bool t2_init_took(struct mct_t2 *t2)
{
	u8 a = 0, b = 0;
	int ret_a, ret_b = -EIO;

	mutex_lock(&t2->io_lock);
	t2_drain_replies(t2);
	ret_a = t2_read_reg(t2, 0xfeb0, &a, T2_TIMEOUT);
	if (!ret_a)
		ret_b = t2_read_reg(t2, T2_REG_PLL_EN, &b, T2_TIMEOUT);
	mutex_unlock(&t2->io_lock);
	if (ret_a || ret_b) {
		drm_warn(&t2->base.dev, "init check read failed: %d/%d\n", ret_a, ret_b);
		return false;
	}
	if (a != 0x40 || b != 0x01) {
		drm_warn(&t2->base.dev, "init check failed (feb0=%02x fcf6=%02x)\n", a, b);
		return false;
	}
	return true;
}

static int t2_hw_init(struct mct_t2 *t2)
{
	int ret, tries;

	ret = t2_resync(t2);
	if (ret)
		return ret;
	for (tries = 0; tries < 2; tries++) {
		ret = t2_hw_init_once(t2);
		if (ret)
			return ret;
		if (t2_init_took(t2))
			return 0;
		if (!tries)
			drm_warn(&t2->base.dev, "retrying device init\n");
	}
	drm_err(&t2->base.dev, "device init did not take\n");
	return -EIO;
}

/* ------------------------------------------------------------------ */
/* backlight (via the pen function's HID feature report 0x06)	      */

#define T2_PEN_VENDOR	0x056a
#define T2_PEN_PRODUCT	0x032f	/* the DTU-1031X's pen function */
#define T2_BL_REPORT	0x06
#define T2_BL_LEVELS	9	/* report values 0..8; the firmware clamps above 8 */
/* backlight brightness: 0 = off, 1..9 = report level 0..8 */

struct t2_pen_lookup {
	struct usb_device *hub;
	struct usb_device *pen;
};

static int t2_match_pen(struct usb_device *udev, void *data)
{
	struct t2_pen_lookup *l = data;

	if (udev->parent != l->hub ||
	    le16_to_cpu(udev->descriptor.idVendor) != T2_PEN_VENDOR ||
	    le16_to_cpu(udev->descriptor.idProduct) != T2_PEN_PRODUCT)
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
	int b = backlight_get_brightness(bd);	/* 0 also when bl_power is off */
	u8 level = b ? b - 1 : 0;
	int ret = 0;

	if (b)
		ret = t2_bl_xfer(t2, true, &level);
	t2_set_power(t2, &t2->bl_off, !b);
	return ret;
}

static int t2_bl_get_brightness(struct backlight_device *bd)
{
	struct mct_t2 *t2 = bl_get_data(bd);
	u8 level = 0;
	int ret;

	if (t2->bl_off)
		return 0;
	ret = t2_bl_xfer(t2, false, &level);
	if (ret)
		return ret;
	return min_t(int, level, T2_BL_LEVELS - 1) + 1;
}

static const struct backlight_ops t2_bl_ops = {
	.update_status = t2_bl_update_status,
	.get_brightness = t2_bl_get_brightness,
};

/*
 * The backlight hangs off the DRM connector (like laptop panels do), so
 * userspace can tell which output it belongs to.
 */
static int t2_backlight_register(struct mct_t2 *t2)
{
	struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.max_brightness = T2_BL_LEVELS,
		.brightness = 8,	/* report level 7, the power-on default */
		.scale = BACKLIGHT_SCALE_NON_LINEAR,
	};
	struct backlight_device *bd;
	char name[sizeof(t2->bl_name)];
	u8 level;

	/*
	 * After a replug the pen function may enumerate after us, so the pen
	 * is looked up on every access; keep the default if it isn't there yet.
	 */
	if (!t2_bl_xfer(t2, false, &level))
		props.brightness = min_t(int, level, T2_BL_LEVELS - 1) + 1;
	snprintf(name, sizeof(name), "mct_t2-%d", t2->base.dev.primary->index);
	bd = backlight_device_register(name, t2->base.conn.kdev, t2, &t2_bl_ops, &props);
	if (IS_ERR(bd)) {
		drm_warn(&t2->base.dev, "backlight registration failed: %ld\n", PTR_ERR(bd));
		return 0;	/* not fatal */
	}
	t2->bl = bd;
	strscpy(t2->bl_name, name);
	return 0;
}

static void t2_backlight_unregister(struct mct_t2 *t2)
{
	t2->bl_name[0] = 0;
	backlight_device_unregister(t2->bl);
	t2->bl = NULL;
}

/* ------------------------------------------------------------------ */
/* frame updates						      */

static void t2_update(struct mct_device *mdev)
{
	struct mct_t2 *t2 = to_t2(mdev);
	bool refine, went_lossy = false;
	ktime_t damaged;
	int idx, ret = -ENODEV;

	mutex_lock(&t2->base.lock);
	if (!t2->base.dirty || !t2->base.width) {
		mutex_unlock(&t2->base.lock);
		return;
	}
	t2->base.dirty = false;
	damaged = t2->base.dirty_since;
	t2->base.dirty_since = 0;
	refine = t2->refine > 0 && !delayed_work_pending(&t2->base.settle_work);
	if (refine && --t2->refine) {
		t2->base.dirty = true;	/* second pass for the other buffer */
		mct_kick(mdev);
	}
	mutex_lock(&t2->io_lock);
	memcpy(t2->snap, t2->base.shadow, T2_WIDTH * T2_HEIGHT * 4);
	mutex_unlock(&t2->base.lock);

	if (drm_dev_enter(&t2->base.dev, &idx)) {
		ret = t2_present(t2, refine, &went_lossy);
		/* an interrupted frame leaves the firmware mid-command: always report */
		if (ret) {
			t2->stats.errors++;
			drm_err_ratelimited(&t2->base.dev, "frame update error: %d\n", ret);
		}
		drm_dev_exit(idx);
	}
	mutex_unlock(&t2->io_lock);

	/* finish the cut command on the device, then send whole frames again */
	if (ret && ret != -ENODEV && ret != -ESHUTDOWN && drm_dev_enter(&t2->base.dev, &idx)) {
		if (!t2_resync(t2)) {
			mutex_lock(&t2->io_lock);
			t2->hw_valid[0] = false;
			t2->hw_valid[1] = false;
			mutex_unlock(&t2->io_lock);
			mutex_lock(&t2->base.lock);
			t2->base.dirty = true;
			mutex_unlock(&t2->base.lock);
			mct_kick(mdev);
		}
		drm_dev_exit(idx);
	}

	if (!ret && damaged) {
		static const int bounds[] = { 8, 16, 33, 50, 100 };
		s64 ms = ktime_ms_delta(ktime_get(), damaged);
		int b = 0;

		while (b < ARRAY_SIZE(bounds) && ms >= bounds[b])
			b++;
		t2->stats.latency[b]++;
	}

	if (went_lossy) {
		/* two lossless passes once motion stops: one per buffer */
		mutex_lock(&t2->base.lock);
		t2->refine = 2;
		mutex_unlock(&t2->base.lock);
		mct_settle(mdev, T2_REFINE_MS);
	}
}

/* ------------------------------------------------------------------ */
/* cursor plane (hardware cursor, see docs/reference/protocol-t2.md)	      */

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

static void t2_cursor_hide_hook(struct mct_device *mdev)
{
	t2_cursor_hide(to_t2(mdev));
}

static void t2_cursor_set(struct mct_device *mdev, const u8 *bgra, int x, int y)
{
	struct mct_t2 *t2 = to_t2(mdev);
	int dx, dy, sx, sy, i, j;
	u8 *dst;

	/*
	 * The visible 64x64 box at (x, y) lands, rotated by 180 degrees, at
	 * device (dx, dy). Coordinates must be >= 0: if the box hangs off the
	 * visible right/bottom edge, shift the image instead.
	 */
	dx = T2_WIDTH - (x + T2_CUR);
	dy = T2_HEIGHT - (y + T2_CUR);
	sx = max(-dx, 0);
	sy = max(-dy, 0);
	dx = max(dx, 0);
	dy = max(dy, 0);
	if (sx >= T2_CUR || sy >= T2_CUR || dx >= T2_WIDTH || dy >= T2_HEIGHT) {
		t2_cursor_hide(t2);
		return;
	}

	/* B, G, R, A (visible orientation) -> A, B, G, R bytes, rotated 180, shifted */
	dst = t2->cur.img;
	for (j = 0; j < T2_CUR; j++) {
		for (i = 0; i < T2_CUR; i++, dst += 4) {
			/* device (i + sx, j + sy) is visible (63 - (i + sx), 63 - (j + sy)) */
			int vx = T2_CUR - 1 - (i + sx), vy = T2_CUR - 1 - (j + sy);
			const u8 *px;

			if (vx < 0 || vy < 0) {
				memset(dst, 0, 4);
				continue;
			}
			px = bgra + vy * T2_CUR * 4 + vx * 4;
			dst[0] = min_t(u8, px[3], 0xfe);	/* vendor uses 0xfe for opaque */
			dst[1] = px[0];
			dst[2] = px[1];
			dst[3] = px[2];
		}
	}

	if (!t2->cur.uploaded || memcmp(t2->cur.img, t2->cur.shown, T2_CUR_BYTES))
		if (t2_cursor_upload(t2))
			return;
	t2_cursor_regs(t2, dx, dy, true);
}

/* ------------------------------------------------------------------ */
/* display on/off, modes					      */

static int t2_enable(struct mct_device *mdev, const struct drm_display_mode *mode)
{
	struct mct_t2 *t2 = to_t2(mdev);
	int idx;

	if (drm_dev_enter(&mdev->dev, &idx)) {
		t2_set_power(t2, &t2->crtc_on, true);
		drm_dev_exit(idx);
	}
	return 0;
}

static void t2_disable(struct mct_device *mdev)
{
	struct mct_t2 *t2 = to_t2(mdev);

	t2_cursor_hide(t2);
	t2_set_power(t2, &t2->crtc_on, false);	/* panel/backlight off */
}

static int t2_get_modes(struct mct_device *mdev, struct drm_connector *connector)
{
	struct mct_t2 *t2 = to_t2(mdev);
	struct drm_display_mode *mode;
	int count = 0;

	if (t2->edid) {
		drm_edid_connector_update(connector, t2->edid);
		count = drm_edid_connector_add_modes(connector);
	}
	if (!count)
		drm_edid_connector_update(connector, NULL);
	/*
	 * No EDID, or one without the panel's size: offer 1024x600 anyway (the
	 * init programs the panel's timing, the mode only names the size).
	 */
	list_for_each_entry(mode, &connector->probed_modes, head)
		if (mode->hdisplay == T2_WIDTH && mode->vdisplay == T2_HEIGHT)
			return count;
	mode = drm_cvt_mode(connector->dev, T2_WIDTH, T2_HEIGHT, 60, false, false, false);
	if (!mode)
		return count;
	mode->type |= DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);
	return count + 1;
}

static enum drm_mode_status t2_mode_valid(struct mct_device *mdev,
					  const struct drm_display_mode *mode)
{
	if (mode->hdisplay != T2_WIDTH || mode->vdisplay != T2_HEIGHT)
		return MODE_BAD;
	return MODE_OK;
}

static int t2_late_register(struct mct_device *mdev)
{
	return t2_backlight_register(to_t2(mdev));
}

static void t2_early_unregister(struct mct_device *mdev)
{
	t2_backlight_unregister(to_t2(mdev));
}

/* ------------------------------------------------------------------ */
/* debugfs							      */

static void t2_stats(struct mct_device *mdev, struct seq_file *m)
{
	struct mct_t2 *t2 = to_t2(mdev);
	u64 ms = max_t(u64, ktime_ms_delta(ktime_get(), mdev->stats.since), 1);
	u64 full = (u64)T2_WIDTH * T2_HEIGHT;
	int i;

	seq_printf(m, "updates          %llu\n", t2->stats.updates);
	seq_printf(m, "lossy updates    %llu (by tolerance:", t2->stats.lossy_updates);
	for (i = 1; i < ARRAY_SIZE(t2->stats.lossy_tol); i++)
		if (t2->stats.lossy_tol[i])
			seq_printf(m, " %d:%llu", i, t2->stats.lossy_tol[i]);
	seq_puts(m, ")\n");
	seq_printf(m, "rects            %llu\n", t2->stats.rects);
	seq_printf(m, "pixels sent      %llu (%llu full frames)\n",
		   t2->stats.px_sent, div64_u64(t2->stats.px_sent, full));
	seq_printf(m, "sent/damaged     %llu%%\n",
		   div64_u64(t2->stats.px_sent * 100, max_t(u64, t2->base.stats.px_damage, 1)));
	seq_printf(m, "usb bytes        %llu (avg %llu KB/s)\n",
		   t2->stats.bytes, div64_u64(t2->stats.bytes, ms));
	seq_printf(m, "ms diff/encode/usb  %llu / %llu / %llu\n",
		   div64_u64(t2->stats.ns_diff, 1000000), div64_u64(t2->stats.ns_encode, 1000000),
		   div64_u64(t2->stats.ns_usb, 1000000));
	seq_printf(m, "usb MB/s while busy %llu\n",
		   div64_u64(t2->stats.bytes * 1000, max_t(u64, t2->stats.ns_usb, 1)));
	seq_printf(m, "over budget      %llu updates (max payload %llu KB)\n",
		   t2->stats.over_budget, div64_u64(t2->stats.max_payload, 1024));
	seq_printf(m, "latency ms       <8:%llu <16:%llu <33:%llu <50:%llu <100:%llu >=100:%llu\n",
		   t2->stats.latency[0], t2->stats.latency[1], t2->stats.latency[2],
		   t2->stats.latency[3], t2->stats.latency[4], t2->stats.latency[5]);
}

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
	MCT_COMPAT_DRIVER_OPS
};

/* sysfs (docs/driver/companion-app.md) */

/* NULL once the interface is unbound, see mct_from_dev() */
static struct mct_t2 *t2_from_dev(struct device *dev)
{
	struct mct_device *mdev = mct_from_dev(dev);

	return mdev ? to_t2(mdev) : NULL;
}

#define T2_SHOW_U64(_name, _expr)						\
static ssize_t _name##_show(struct device *dev, struct device_attribute *attr, char *buf) \
{										\
	struct mct_t2 *t2 = t2_from_dev(dev);					\
										\
	if (!t2)								\
		return -ENODEV;							\
	return sysfs_emit(buf, "%llu\n", (u64)(_expr));			\
}										\
static DEVICE_ATTR_RO(_name)

T2_SHOW_U64(updates, t2->stats.updates);
T2_SHOW_U64(bytes, t2->stats.bytes);
T2_SHOW_U64(errors, t2->stats.errors);

static ssize_t backlight_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mct_t2 *t2 = t2_from_dev(dev);

	if (!t2)
		return -ENODEV;
	return sysfs_emit(buf, "%s\n", t2->bl_name[0] ? t2->bl_name : "-");
}
static DEVICE_ATTR_RO(backlight);

static ssize_t lossy_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mct_t2 *t2 = t2_from_dev(dev);

	return t2 ? sysfs_emit(buf, "%d\n", READ_ONCE(t2->lossy)) : -ENODEV;
}

static ssize_t lossy_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t len)
{
	struct mct_t2 *t2 = t2_from_dev(dev);
	int v, ret = kstrtoint(buf, 0, &v);

	if (!t2)
		return -ENODEV;
	if (ret)
		return ret;
	if (v < 0 || v > 8)
		return -EINVAL;
	WRITE_ONCE(t2->lossy, v);
	return len;
}
static DEVICE_ATTR_RW(lossy);

static struct attribute *t2_attrs[] = {
	&dev_attr_updates.attr,
	&dev_attr_bytes.attr,
	&dev_attr_errors.attr,
	&dev_attr_backlight.attr,
	&dev_attr_lossy.attr,
	NULL
};

static const struct mct_funcs t2_funcs = {
	.get_modes = t2_get_modes,
	.mode_valid = t2_mode_valid,
	.late_register = t2_late_register,
	.early_unregister = t2_early_unregister,
	.enable = t2_enable,
	.disable = t2_disable,
	.update = t2_update,
	.cursor_set = t2_cursor_set,
	.cursor_hide = t2_cursor_hide_hook,
	.stats = t2_stats,
};

static void t2_release(struct drm_device *dev, void *res)
{
	struct mct_t2 *t2 = to_t2(to_mct(dev));

	kvfree(t2->enc);
	kvfree(t2->snap);
	kvfree(t2->hw[0]);
	kvfree(t2->hw[1]);
	drm_edid_free(t2->edid);
}

static int t2_usb_probe(struct usb_interface *interface, const struct usb_device_id *id)
{
	static const struct mct_config cfg = {
		.funcs = &t2_funcs,
		.min_w = T2_WIDTH,
		.min_h = T2_HEIGHT,
		.max_w = T2_WIDTH,
		.max_h = T2_HEIGHT,
		.connector_type = DRM_MODE_CONNECTOR_USB,
		.cursor_size = T2_CUR,
		.encoder_type = DRM_MODE_ENCODER_NONE,
		.stats_name = "mct_t2_stats",
		.chip = "t2",
		.attrs = t2_attrs,
	};
	struct mct_t2 *t2;
	struct drm_device *dev;
	struct device *dma_dev;
	int ret;
	static const u8 bulk_eps[] = { T2_EP_FB, T2_EP_CMD, T2_EP_MCU, T2_EP_IN, 0 };

	if (interface->cur_altsetting->desc.bInterfaceNumber != 0)
		return -ENODEV;
	/* refuse look-alikes without the expected endpoints */
	if (!usb_check_bulk_endpoints(interface, bulk_eps))
		return -ENODEV;

	t2 = devm_drm_dev_alloc(&interface->dev, &t2_drm_driver, struct mct_t2, base.dev);
	if (IS_ERR(t2))
		return PTR_ERR(t2);
	dev = &t2->base.dev;

	dma_dev = usb_intf_get_dma_device(interface);
	if (dma_dev) {
		drm_dev_set_dma_dev(dev, dma_dev);
		put_device(dma_dev);
	} else {
		drm_warn(dev, "buffer sharing not supported");
	}

	ret = drmm_mutex_init(dev, &t2->io_lock) ?: drmm_mutex_init(dev, &t2->power_lock);
	if (ret)
		return ret;
	/* the module parameter is the default this device starts with */
	t2->lossy = clamp(READ_ONCE(lossy), 0, 8);
	t2->front = T2_FB_A;

	t2->bounce = drmm_kmalloc(dev, T2_BOUNCE, GFP_KERNEL);
	t2->cur.buf = drmm_kmalloc(dev, 64, GFP_KERNEL);
	t2->cur.img = drmm_kmalloc(dev, T2_CUR_BYTES, GFP_KERNEL);
	t2->cur.shown = drmm_kmalloc(dev, T2_CUR_BYTES, GFP_KERNEL);
	t2->enc = kvmalloc(T2_ENC_MAX, GFP_KERNEL);
	t2->snap = kvmalloc(T2_WIDTH * T2_HEIGHT * 4, GFP_KERNEL);
	t2->hw[0] = kvmalloc(T2_WIDTH * T2_HEIGHT * 4, GFP_KERNEL);
	t2->hw[1] = kvmalloc(T2_WIDTH * T2_HEIGHT * 4, GFP_KERNEL);
	ret = drmm_add_action_or_reset(dev, t2_release, NULL);
	if (ret)
		return ret;
	if (!t2->bounce || !t2->cur.buf || !t2->cur.img || !t2->cur.shown ||
	    !t2->enc || !t2->snap || !t2->hw[0] || !t2->hw[1])
		return -ENOMEM;

	/* the init sends the (blank) shadow, so it exists from the start */
	ret = mct_init(&t2->base, &cfg);
	if (ret)
		return ret;
	ret = mct_shadow_alloc(&t2->base, T2_WIDTH, T2_HEIGHT);
	if (ret)
		return ret;
	ret = t2_hw_init(t2);
	if (ret)
		return ret;
	return mct_register(&t2->base, interface);
}

static void t2_usb_disconnect(struct usb_interface *interface)
{
	mct_unplug(to_mct(usb_get_intfdata(interface)));
}

static int t2_suspend(struct usb_interface *interface, pm_message_t message)
{
	return mct_suspend(to_mct(usb_get_intfdata(interface)));
}

static int t2_resume(struct usb_interface *interface)
{
	struct mct_device *mdev = to_mct(usb_get_intfdata(interface));
	int ret;

	/* resume the DRM side even if the device stays dark */
	ret = t2_hw_init(to_t2(mdev));
	return mct_resume(mdev) ?: ret;
}

const struct mct_backend mct_t2_backend = {
	.probe = t2_usb_probe,
	.disconnect = t2_usb_disconnect,
	.suspend = t2_suspend,
	.resume = t2_resume,
};
