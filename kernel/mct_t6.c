// SPDX-License-Identifier: GPL-2.0-only
/*
 * DRM driver for MCT Trigger 6 (T6) USB display adapters, e.g. the
 * "T6 USB Station" (USB 0711:5600 / 0711:5601).
 *
 * Protocol (see docs/protocol-t6.md; first worked out by the trigger6
 * driver at https://github.com/JingYenLoh/trigger6):
 *  - vendor control requests: 0x87 output status, 0x80 EDID, 0x89 mode
 *    table, 0x12 set mode, 0x03 output on/off
 *  - bulk EP 0x02 carries frames: per fragment of at most 0x19000 bytes a
 *    32-byte session header in its own transfer, then the fragment
 *  - a frame is a 48-byte video header plus the image; the header names the
 *    surface format and the device addresses to scan out from, and the
 *    session header's dest is where the data lands (header first, so the
 *    pixels start at dest + 0x30)
 *
 * Frames are converted to NV12, the T6's own surface format, and sent whole
 * into three device buffers in turn. The header's payload length must be
 * exact: the chip switches to a buffer once that many bytes have arrived.
 * A worker does the sending, so commits never
 * wait for USB and the newest image always wins.
 */

#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/unaligned.h>
#include <linux/usb.h>
#include <linux/vmalloc.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_cache.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_debugfs.h>
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
#include <drm/drm_managed.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_plane.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>
#include <drm/drm_vblank_helper.h>

#define DRIVER_NAME	"mct_t6"
#define DRIVER_DESC	"MCT Trigger 6 USB display"
#define DRIVER_MAJOR	0
#define DRIVER_MINOR	6
#define DRIVER_PATCH	1

#define T6_EP_OUT	0x02

#define T6_REQ_EDID	0x80
#define T6_REQ_STATUS	0x87
#define T6_REQ_MODES	0x89
#define T6_REQ_SETMODE	0x12
#define T6_REQ_OUTPUT	0x03
#define T6_REQ_READY	0x31		/* "software ready", after a mode set */
/* hardware cursor, per output: position, show/hide, image upload */
#define T6_REQ_CUR_POS(o)	((o) ? 0x06 : 0x04)
#define T6_REQ_CUR_SHOW(o)	((o) ? 0x07 : 0x05)
#define T6_REQ_CUR_IMG(o)	((o) ? 0x11 : 0x10)

#define T6_CUR		64		/* cursor size used here (device: up to 128) */
#define T6_CUR_BYTES	(8 + T6_CUR * T6_CUR * 4)	/* header + ARGB */

#define T6_MAX_MODES	30		/* 0x89 table: 512 + 448 bytes */
#define T6_MAX_OUTPUTS	2
#define T6_FRAG		0x19000		/* largest fragment the vendor driver sends */
#define T6_CTRL_BUF	512
#define T6_TIMEOUT	2000

#define T6_FMT_NV12	0x6

/*
 * Three frame buffers in device memory, as the vendor driver lays them out
 * for one output: frame header at the start, planes from +0x30.
 */
static const u32 t6_bufs[] = { 0x0, 0xfffec0, 0x1fffd80 };

/*
 * One entry of the device's mode table (0x89), also the 0x12 payload. The
 * pixel clock comes from a PLL: f * x >= 800 MHz with x = 4, 8, 16 or 32
 * (pll_out selects which), N + P / Q = f * x / base clock.
 */
struct t6_mode {
	__le32 clock_khz;
	u8 refresh;
	u8 rsvd;
	__le16 htotal;
	__le16 hdisplay;
	__le16 hsync_start;
	__le16 hsync_len;
	__le16 vtotal;
	__le16 vdisplay;
	__le16 vsync_start;
	__le16 vsync_len;
	__le16 pll_p;
	__le16 pll_q;
	u8 pll_n;
	u8 pll_out;
	u8 hsync_pol;
	u8 vsync_pol;
	u8 reduced;
	u8 flags;
} __packed;

/*
 * The output works up to about 350 MHz pixel clock whatever the refresh
 * rate (clean at 348 MHz, artifacts at 353, no signal from 360; see
 * docs/t6-lab-notebook.md), well above what the mode table offers.
 */
#define T6_MAX_CLOCK_KHZ	350000
/* frame buffers are 0xfffec0 apart; a frame is 0x30 bytes header + NV12 */
#define T6_BUF_SIZE		0xfffec0

/* extra refresh rates offered for the monitor's own sizes, within its range */
static const int t6_extra_hz[] = { 75, 100, 120, 144, 150, 165, 200, 240 };

struct t6_session {
	__le32 type;			/* 0 = video, 3 = audio */
	__le32 total_len;
	__le32 dest;
	__le32 frag_len;
	__le32 offset;
	__le32 more;			/* the vendor driver sets 1 on non-final fragments */
	__le32 unk[2];
} __packed;

/*
 * Session type 7: write a rectangle into the NV12 planes of a frame buffer
 * without showing it. Data: h rows of Y (w bytes), then h / 2 rows of
 * interleaved UV (w bytes).
 */
struct t6_rect_header {
	__le32 type;			/* 7 */
	__le32 len;
	__le32 y_addr;
	__le32 uv_addr;
	__le16 y_pitch;
	__le16 uv_pitch;
	__le16 width;
	__le16 height;
	__le32 rsvd[2];
} __packed;

/* rows of unchanged pixel pairs still merged into one band */
#define T6_BAND_GAP	8
#define T6_MAX_BANDS	16

struct t6_video_header {
	__le32 type;
	__le32 data_len;
	__le32 seq;
	__le32 unk4;
	__le16 width;
	__le16 height;
	__le32 start;
	__le32 end;
	__le32 unk9;
	__le32 format;
	__le32 unk11[3];
} __packed;

struct mct_t6 {
	struct drm_device dev;
	struct drm_plane primary;
	struct drm_plane cursor;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector conn;

	struct t6_mode modes[T6_MAX_MODES];
	int nmodes;
	int output;			/* index the monitor is on */
	int max_w, max_h;
	int pll_base_khz;		/* 48 MHz ("Lite") or 40 MHz ("Super Lite") */

	struct mutex io_lock;		/* USB transfers, ctrl and xfer buffers */
	u8 *ctrl;			/* DMA-able control transfer buffer */
	u8 *xfer;			/* DMA-able bulk fragment buffer */
	u8 *shadow;			/* XRGB8888 image as committed by userspace */
	u8 *img;			/* the shadow in NV12 */
	u8 *hw[3];			/* NV12 image each frame buffer holds */
	bool hw_valid[3];
	int width, height;		/* current mode */
	unsigned int buf;		/* next frame buffer to write */
	u32 seq;

	struct {
		struct work_struct work;
		struct mutex lock;	/* protects shadow + dirty */
		bool dirty;
	} upd;

	struct {
		u8 *img;		/* header + image being built */
		u8 *shown;		/* last uploaded */
		bool uploaded, visible;
	} cur;

	struct {			/* debugfs statistics */
		u64 updates, skipped, px_damage, bytes, ns_conv, ns_usb;
		u64 rows, bands;		/* image rows and bands sent */
		u64 copies, ns_copy, ns_copy_max;	/* framebuffer -> shadow, in the commit */
		u64 ns_commit, ns_commit_max;		/* the same incl. CPU access setup */
		ktime_t since;
	} stats;
};

#define to_t6(__dev) container_of(__dev, struct mct_t6, dev)

static struct usb_device *t6_udev(struct mct_t6 *t6)
{
	return interface_to_usbdev(to_usb_interface(t6->dev.dev));
}

/* ------------------------------------------------------------------ */
/* USB								      */

/* vendor IN request into t6->ctrl; io_lock held */
static int t6_ctrl_in(struct mct_t6 *t6, u8 req, u16 value, u16 index, u16 len)
{
	struct usb_device *udev = t6_udev(t6);

	return usb_control_msg(udev, usb_rcvctrlpipe(udev, 0), req,
			       USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
			       value, index, t6->ctrl, len, T6_TIMEOUT);
}

/* vendor OUT request with len bytes of t6->ctrl; io_lock held */
static int t6_ctrl_out(struct mct_t6 *t6, u8 req, u16 value, u16 index, u16 len)
{
	struct usb_device *udev = t6_udev(t6);

	return usb_control_msg(udev, usb_sndctrlpipe(udev, 0), req,
			       USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
			       value, index, len ? t6->ctrl : NULL, len, T6_TIMEOUT);
}

static int t6_bulk(struct mct_t6 *t6, const void *data, int len)
{
	struct usb_device *udev = t6_udev(t6);
	int ret, done;

	if (data != t6->xfer)
		memcpy(t6->xfer, data, len);
	ret = usb_bulk_msg(udev, usb_sndbulkpipe(udev, T6_EP_OUT), t6->xfer, len,
			   &done, T6_TIMEOUT);
	if (!ret && done != len)
		ret = -EIO;
	return ret;
}

static int t6_output_status(struct mct_t6 *t6, int output)
{
	int ret;

	mutex_lock(&t6->io_lock);
	ret = t6_ctrl_in(t6, T6_REQ_STATUS, output, 0, 1);
	if (ret == 1)
		ret = t6->ctrl[0];
	else if (ret >= 0)
		ret = -EIO;
	mutex_unlock(&t6->io_lock);
	return ret;
}

static int t6_read_modes(struct mct_t6 *t6)
{
	u8 *table = (u8 *)t6->modes;
	int ret, i;

	mutex_lock(&t6->io_lock);
	ret = t6_ctrl_in(t6, T6_REQ_MODES, t6->output, 0, 512);
	if (ret == 512) {
		memcpy(table, t6->ctrl, 512);
		ret = t6_ctrl_in(t6, T6_REQ_MODES, t6->output, 512, 448);
		if (ret == 448)
			memcpy(table + 512, t6->ctrl, 448);
	}
	mutex_unlock(&t6->io_lock);
	if (ret < 0)
		return ret;
	if (ret != 448)
		return -EIO;

	t6->nmodes = 0;
	t6->max_w = 0;
	t6->max_h = 0;
	for (i = 0; i < T6_MAX_MODES; i++) {
		int w = le16_to_cpu(t6->modes[i].hdisplay);
		int h = le16_to_cpu(t6->modes[i].vdisplay);

		if (!w || !h || w == 0xffff)
			continue;
		t6->nmodes = i + 1;
		t6->max_w = max(t6->max_w, w);
		t6->max_h = max(t6->max_h, h);
	}
	return t6->nmodes ? 0 : -ENODEV;
}

/* device mode entry for a DRM mode; -ERANGE if the T6 can't drive it */
static int t6_mode_entry(struct mct_t6 *t6, const struct drm_display_mode *mode,
			 struct t6_mode *m)
{
	static const u8 out_sel[] = { 3, 1, 2, 0 };
	u64 vco;
	int i;

	if (mode->clock > T6_MAX_CLOCK_KHZ || mode->flags & DRM_MODE_FLAG_INTERLACE)
		return -ERANGE;
	/* the NV12 rows are sent unpadded; the vendor driver needs width % 16 too */
	if (mode->hdisplay % 16 || mode->vdisplay % 2)
		return -ERANGE;
	if (mode->hdisplay > t6->max_w || mode->vdisplay > t6->max_h ||
	    0x30 + (u64)mode->hdisplay * mode->vdisplay * 3 / 2 > T6_BUF_SIZE)
		return -ERANGE;

	memset(m, 0, sizeof(*m));
	for (i = 0; i < 4; i++) {
		vco = (u64)mode->clock << (i + 2);	/* f * 4, 8, 16, 32 in kHz */
		if (vco >= 800000)
			break;
	}
	if (i == 4)
		return -ERANGE;
	m->pll_n = div_u64(vco, t6->pll_base_khz);
	m->pll_p = cpu_to_le16(div_u64((vco - (u64)m->pll_n * t6->pll_base_khz) * 1000,
				       t6->pll_base_khz));
	m->pll_q = cpu_to_le16(1000);
	m->pll_out = out_sel[i];

	m->clock_khz = cpu_to_le32(mode->clock);
	m->refresh = drm_mode_vrefresh(mode);
	m->htotal = cpu_to_le16(mode->htotal);
	m->hdisplay = cpu_to_le16(mode->hdisplay);
	m->hsync_start = cpu_to_le16(mode->hsync_start);
	m->hsync_len = cpu_to_le16(mode->hsync_end - mode->hsync_start);
	m->vtotal = cpu_to_le16(mode->vtotal);
	m->vdisplay = cpu_to_le16(mode->vdisplay);
	m->vsync_start = cpu_to_le16(mode->vsync_start);
	m->vsync_len = cpu_to_le16(mode->vsync_end - mode->vsync_start);
	m->hsync_pol = !!(mode->flags & DRM_MODE_FLAG_PHSYNC);
	m->vsync_pol = !!(mode->flags & DRM_MODE_FLAG_PVSYNC);
	return 0;
}

static int t6_set_output(struct mct_t6 *t6, bool on)
{
	int ret;

	mutex_lock(&t6->io_lock);
	ret = t6_ctrl_out(t6, T6_REQ_OUTPUT, t6->output, on, 0);
	mutex_unlock(&t6->io_lock);
	return ret < 0 ? ret : 0;
}

static int t6_ready(struct mct_t6 *t6)
{
	int ret;

	mutex_lock(&t6->io_lock);
	ret = t6_ctrl_out(t6, T6_REQ_READY, 0, 0, 0);
	mutex_unlock(&t6->io_lock);
	return ret < 0 ? ret : 0;
}

static int t6_set_mode(struct mct_t6 *t6, const struct t6_mode *m)
{
	int ret;

	mutex_lock(&t6->io_lock);
	memcpy(t6->ctrl, m, sizeof(*m));
	ret = t6_ctrl_out(t6, T6_REQ_SETMODE, t6->output, 0, sizeof(*m));
	mutex_unlock(&t6->io_lock);
	return ret < 0 ? ret : 0;
}

/* ------------------------------------------------------------------ */
/* frame updates						      */

/*
 * Shadow (XRGB8888) to NV12 in t6->img: BT.601 limited range, 4:2:0, the Y
 * plane then interleaved Cb/Cr. Modes have even sizes. upd.lock held.
 */
static void t6_convert(struct mct_t6 *t6)
{
	int w = t6->width, h = t6->height, x, y;
	u8 *py = t6->img, *puv = t6->img + (size_t)w * h;

	for (y = 0; y < h; y += 2) {
		const u8 *s0 = t6->shadow + (size_t)y * w * 4;	/* B, G, R, X */
		const u8 *s1 = s0 + w * 4;
		u8 *y0 = py + (size_t)y * w, *y1 = y0 + w;
		u8 *uv = puv + (size_t)(y / 2) * w;

		for (x = 0; x < w; x += 2, s0 += 8, s1 += 8, y0 += 2, y1 += 2, uv += 2) {
			int b = s0[0] + s0[4] + s1[0] + s1[4];
			int g = s0[1] + s0[5] + s1[1] + s1[5];
			int r = s0[2] + s0[6] + s1[2] + s1[6];

			y0[0] = ((66 * s0[2] + 129 * s0[1] + 25 * s0[0] + 128) >> 8) + 16;
			y0[1] = ((66 * s0[6] + 129 * s0[5] + 25 * s0[4] + 128) >> 8) + 16;
			y1[0] = ((66 * s1[2] + 129 * s1[1] + 25 * s1[0] + 128) >> 8) + 16;
			y1[1] = ((66 * s1[6] + 129 * s1[5] + 25 * s1[4] + 128) >> 8) + 16;
			/* chroma of the 2x2 block: sums are 4x, so shift by 10 */
			uv[0] = ((-38 * r - 74 * g + 112 * b + 512) >> 10) + 128;
			uv[1] = ((112 * r - 94 * g - 18 * b + 512) >> 10) + 128;
		}
	}
}

/*
 * Bands of row pairs (two Y rows and their UV row) where t6->img differs
 * from what frame buffer k holds, as [start, end) pairs; returns the count.
 */
static int t6_diff_bands(struct mct_t6 *t6, int k, int bands[][2])
{
	int w = t6->width, pairs = t6->height / 2, n = 0, p;
	const u8 *ny = t6->img, *oy = t6->hw[k];
	const u8 *nuv = ny + (size_t)w * t6->height, *ouv = oy + (size_t)w * t6->height;

	if (!t6->hw_valid[k]) {
		bands[0][0] = 0;
		bands[0][1] = pairs;
		return 1;
	}
	for (p = 0; p < pairs; p++) {
		size_t yo = (size_t)p * 2 * w, uvo = (size_t)p * w;

		if (!memcmp(ny + yo, oy + yo, 2 * w) && !memcmp(nuv + uvo, ouv + uvo, w))
			continue;
		if (n && p - bands[n - 1][1] <= T6_BAND_GAP) {
			bands[n - 1][1] = p + 1;
		} else if (n == T6_MAX_BANDS) {
			bands[n - 1][1] = p + 1;	/* too scattered: grow the last band */
		} else {
			bands[n][0] = p;
			bands[n][1] = p + 1;
			n++;
		}
	}
	return n;
}

/* write pairs [p0, p1) of t6->img into frame buffer k; io_lock held */
static int t6_send_band(struct mct_t6 *t6, int k, int p0, int p1)
{
	int w = t6->width, h = t6->height;
	/* each write fits the transfer buffer */
	int step = (T6_FRAG - sizeof(struct t6_rect_header)) / (3 * w);
	u32 base = t6_bufs[k] + 0x30;
	int p, n, ret = 0;

	for (p = p0; p < p1 && !ret; p += n) {
		struct t6_rect_header *r = (struct t6_rect_header *)t6->xfer;
		size_t ylen, uvlen;

		n = min(step, p1 - p);
		ylen = (size_t)n * 2 * w;
		uvlen = (size_t)n * w;
		memset(r, 0, sizeof(*r));
		r->type = cpu_to_le32(7);
		r->len = cpu_to_le32(ylen + uvlen);
		r->y_addr = cpu_to_le32(base + p * 2 * w);
		r->uv_addr = cpu_to_le32(base + w * h + p * w);
		r->y_pitch = cpu_to_le16(w);
		r->uv_pitch = cpu_to_le16(w);
		r->width = cpu_to_le16(w);
		r->height = cpu_to_le16(n * 2);
		ret = t6_bulk(t6, r, sizeof(*r));
		if (ret)
			break;
		memcpy(t6->xfer, t6->img + (size_t)p * 2 * w, ylen);
		memcpy(t6->xfer + ylen, t6->img + (size_t)w * h + (size_t)p * w, uvlen);
		ret = t6_bulk(t6, t6->xfer, ylen + uvlen);
		t6->stats.rows += n * 2;
		t6->stats.bytes += sizeof(*r) + ylen + uvlen;
	}
	return ret;
}

/* show frame buffer k: a flip header without image data; io_lock held */
static int t6_flip(struct mct_t6 *t6, int k)
{
	struct {
		struct t6_session s;
		struct t6_video_header v;
	} __packed f = { };
	int w = t6->width, h = t6->height;
	u32 base = t6_bufs[k];
	int ret;

	f.s.total_len = cpu_to_le32(sizeof(f.v));
	f.s.dest = cpu_to_le32(base);
	f.s.frag_len = cpu_to_le32(sizeof(f.v));
	f.v.type = cpu_to_le32(3);
	f.v.seq = cpu_to_le32(++t6->seq);
	f.v.unk4 = cpu_to_le32(T6_FMT_NV12);		/* surface format */
	f.v.width = cpu_to_le16(w);			/* Y pitch */
	f.v.height = cpu_to_le16(w);			/* UV pitch */
	f.v.start = cpu_to_le32(base + 0x30);		/* Y plane */
	f.v.end = cpu_to_le32(base + 0x30 + w * h);	/* UV plane */
	f.v.unk9 = cpu_to_le32(base + 0x30);		/* third plane, unused */
	f.v.format = cpu_to_le32(T6_FMT_NV12);

	ret = t6_bulk(t6, &f.s, sizeof(f.s));
	if (!ret)
		ret = t6_bulk(t6, &f.v, sizeof(f.v));
	t6->stats.bytes += sizeof(f);
	return ret;
}

/*
 * Write what changed into the next of the three frame buffers (never the
 * one on screen), then flip to it. io_lock held.
 */
static int t6_present(struct mct_t6 *t6)
{
	int bands[T6_MAX_BANDS][2];
	int k = t6->buf, w = t6->width, h = t6->height;
	int n, i, ret = 0;

	n = t6_diff_bands(t6, k, bands);
	t6->hw_valid[k] = false;
	for (i = 0; i < n && !ret; i++)
		ret = t6_send_band(t6, k, bands[i][0], bands[i][1]);
	if (!ret)
		ret = t6_flip(t6, k);
	if (ret)
		return ret;

	for (i = 0; i < n; i++) {
		size_t yo = (size_t)bands[i][0] * 2 * w, uvo = (size_t)w * h + bands[i][0] * w;
		size_t pairs = bands[i][1] - bands[i][0];

		memcpy(t6->hw[k] + yo, t6->img + yo, pairs * 2 * w);
		memcpy(t6->hw[k] + uvo, t6->img + uvo, pairs * w);
	}
	t6->hw_valid[k] = true;
	t6->stats.bands += n;
	t6->buf = (k + 1) % ARRAY_SIZE(t6_bufs);
	return 0;
}

static void t6_update_work(struct work_struct *work)
{
	struct mct_t6 *t6 = container_of(work, struct mct_t6, upd.work);
	ktime_t t0, t1;
	int idx, ret;

	mutex_lock(&t6->upd.lock);
	if (!t6->upd.dirty || !t6->width) {
		mutex_unlock(&t6->upd.lock);
		return;
	}
	t6->upd.dirty = false;
	mutex_lock(&t6->io_lock);
	t0 = ktime_get();
	t6_convert(t6);
	/* commits only wait for the conversion above, never for USB */
	mutex_unlock(&t6->upd.lock);
	t1 = ktime_get();

	if (drm_dev_enter(&t6->dev, &idx)) {
		ret = t6_present(t6);
		if (ret)
			drm_err_ratelimited(&t6->dev, "frame update error: %d\n", ret);
		else
			t6->stats.updates++;
		drm_dev_exit(idx);
	}
	t6->stats.ns_conv += ktime_to_ns(ktime_sub(t1, t0));
	t6->stats.ns_usb += ktime_to_ns(ktime_sub(ktime_get(), t1));
	mutex_unlock(&t6->io_lock);
}

static void t6_mark_dirty(struct mct_t6 *t6, struct drm_framebuffer *fb,
			  const struct iosys_map *map, const struct drm_rect *dirty)
{
	const u8 *src = map->vaddr;	/* shmem GEM: always a system-memory mapping */
	ktime_t t0;
	u64 ns;
	int y;

	mutex_lock(&t6->upd.lock);
	t0 = ktime_get();
	if (t6->upd.dirty)
		t6->stats.skipped++;	/* the previous image never went out */
	t6->stats.px_damage += drm_rect_width(dirty) * drm_rect_height(dirty);
	/*
	 * Buffers shared by a GPU are usually mapped write-combined, where plain
	 * reads crawl (~90 MB/s measured with amdgpu); the streaming-load copy
	 * is many times faster.
	 */
	for (y = dirty->y1; y < dirty->y2; y++) {
		struct iosys_map d = IOSYS_MAP_INIT_VADDR(t6->shadow +
							  (y * t6->width + dirty->x1) * 4);
		struct iosys_map sm = IOSYS_MAP_INIT_VADDR((void *)(src + y * fb->pitches[0] +
								    dirty->x1 * 4));

		drm_memcpy_from_wc(&d, &sm, drm_rect_width(dirty) * 4);
	}
	ns = ktime_to_ns(ktime_sub(ktime_get(), t0));
	t6->stats.copies++;
	t6->stats.ns_copy += ns;
	t6->stats.ns_copy_max = max(t6->stats.ns_copy_max, ns);
	t6->upd.dirty = true;
	mutex_unlock(&t6->upd.lock);

	queue_work(system_long_wq, &t6->upd.work);
}

static void t6_stop_updates(struct mct_t6 *t6)
{
	cancel_work_sync(&t6->upd.work);
	mutex_lock(&t6->upd.lock);
	t6->upd.dirty = false;
	mutex_unlock(&t6->upd.lock);
}

/* ------------------------------------------------------------------ */
/* primary plane						      */

static const u32 t6_formats[] = {
	DRM_FORMAT_XRGB8888,
};

static const u64 t6_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID
};

static int t6_primary_atomic_check(struct drm_plane *plane, struct drm_atomic_commit *state)
{
	struct drm_plane_state *new_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc_state *crtc_state = NULL;

	if (new_state->crtc)
		crtc_state = drm_atomic_get_new_crtc_state(state, new_state->crtc);
	return drm_atomic_helper_check_plane_state(new_state, crtc_state,
						   DRM_PLANE_NO_SCALING, DRM_PLANE_NO_SCALING,
						   false, false);
}

static void t6_primary_atomic_update(struct drm_plane *plane, struct drm_atomic_commit *state)
{
	struct mct_t6 *t6 = to_t6(plane->dev);
	struct drm_plane_state *new_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_shadow_plane_state *shadow = to_drm_shadow_plane_state(new_state);
	struct drm_framebuffer *fb = new_state->fb;
	struct drm_rect rect;
	ktime_t t0;
	u64 ns;

	if (!fb || !new_state->visible || !t6->width)
		return;
	if (fb->width != t6->width || fb->height != t6->height)
		return;
	/*
	 * Every frame is sent whole, so copy the whole framebuffer. The damage
	 * clips are not used: niri's clips for a rotated output did not cover
	 * what changed, which left stale parts on screen.
	 */
	drm_rect_init(&rect, 0, 0, fb->width, fb->height);
	t0 = ktime_get();
	if (drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE))
		return;
	t6_mark_dirty(t6, fb, &shadow->data[0], &rect);
	drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);
	ns = ktime_to_ns(ktime_sub(ktime_get(), t0));
	t6->stats.ns_commit += ns;
	t6->stats.ns_commit_max = max(t6->stats.ns_commit_max, ns);
}

static const struct drm_plane_helper_funcs t6_primary_helper_funcs = {
	DRM_GEM_SHADOW_PLANE_HELPER_FUNCS,
	.atomic_check = t6_primary_atomic_check,
	.atomic_update = t6_primary_atomic_update,
};

static const struct drm_plane_funcs t6_primary_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	DRM_GEM_SHADOW_PLANE_FUNCS,
};

/* ------------------------------------------------------------------ */
/* cursor plane							      */

static const u32 t6_cursor_formats[] = {
	DRM_FORMAT_ARGB8888,
};

/* the vendor driver gives output 1 the cursor slots from 5 on */
static int t6_cursor_slot(struct mct_t6 *t6)
{
	return t6->output ? 5 : 0;
}

static int t6_cursor_show(struct mct_t6 *t6, bool show)
{
	int ret;

	if (t6->cur.visible == show)
		return 0;
	mutex_lock(&t6->io_lock);
	ret = t6_ctrl_out(t6, T6_REQ_CUR_SHOW(t6->output), t6_cursor_slot(t6), show, 0);
	mutex_unlock(&t6->io_lock);
	if (ret < 0)
		return ret;
	t6->cur.visible = show;
	return 0;
}

/* image in chunks of up to 512 bytes, wIndex = byte offset */
static int t6_cursor_upload(struct mct_t6 *t6)
{
	int off, n, ret = 0;

	mutex_lock(&t6->io_lock);
	for (off = 0; off < T6_CUR_BYTES && ret >= 0; off += n) {
		n = min(T6_CUR_BYTES - off, T6_CTRL_BUF);
		memcpy(t6->ctrl, t6->cur.img + off, n);
		ret = t6_ctrl_out(t6, T6_REQ_CUR_IMG(t6->output), t6_cursor_slot(t6), off, n);
	}
	mutex_unlock(&t6->io_lock);
	if (ret < 0)
		return ret;
	memcpy(t6->cur.shown, t6->cur.img, T6_CUR_BYTES);
	t6->cur.uploaded = true;
	return 0;
}

static int t6_cursor_move(struct mct_t6 *t6, int x, int y)
{
	int ret;

	mutex_lock(&t6->io_lock);
	ret = t6_ctrl_out(t6, T6_REQ_CUR_POS(t6->output), x, y, 0);
	mutex_unlock(&t6->io_lock);
	return ret < 0 ? ret : 0;
}

static int t6_cursor_atomic_check(struct drm_plane *plane, struct drm_atomic_commit *state)
{
	struct drm_plane_state *new_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_framebuffer *fb = new_state->fb;
	struct drm_crtc_state *crtc_state = NULL;

	if (new_state->crtc)
		crtc_state = drm_atomic_get_new_crtc_state(state, new_state->crtc);
	if (fb && (fb->width > T6_CUR || fb->height > T6_CUR))
		return -EINVAL;
	return drm_atomic_helper_check_plane_state(new_state, crtc_state,
						   DRM_PLANE_NO_SCALING, DRM_PLANE_NO_SCALING,
						   true, true);
}

static void t6_cursor_atomic_update(struct drm_plane *plane, struct drm_atomic_commit *state)
{
	struct mct_t6 *t6 = to_t6(plane->dev);
	struct drm_plane_state *new_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_shadow_plane_state *shadow = to_drm_shadow_plane_state(new_state);
	struct drm_framebuffer *fb = new_state->fb;
	int x, y, sx, sy, i, j, idx;
	const u8 *src;
	u8 *dst;

	if (!drm_dev_enter(plane->dev, &idx))
		return;

	if (!fb || !new_state->visible) {
		t6_cursor_show(t6, false);
		goto out;
	}

	/*
	 * Position is the top left of the box and must not be negative: if
	 * the box hangs off the left/top edge, shift the image instead.
	 */
	x = new_state->crtc_x;
	y = new_state->crtc_y;
	sx = max(-x, 0);
	sy = max(-y, 0);
	x = max(x, 0);
	y = max(y, 0);
	if (sx >= T6_CUR || sy >= T6_CUR) {
		t6_cursor_show(t6, false);
		goto out;
	}

	/* header {kind 1 = ARGB with alpha, w, h, pitch}, then B, G, R, A pixels */
	put_unaligned_le16(1, t6->cur.img);
	put_unaligned_le16(T6_CUR, t6->cur.img + 2);
	put_unaligned_le16(T6_CUR, t6->cur.img + 4);
	put_unaligned_le16(T6_CUR * 4, t6->cur.img + 6);
	if (drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE))
		goto out;
	src = shadow->data[0].vaddr;
	dst = t6->cur.img + 8;
	for (j = 0; j < T6_CUR; j++) {
		for (i = 0; i < T6_CUR; i++, dst += 4) {
			int vx = i + sx, vy = j + sy;

			if (vx >= fb->width || vy >= fb->height)
				memset(dst, 0, 4);
			else
				memcpy(dst, src + vy * fb->pitches[0] + vx * 4, 4);
		}
	}
	drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);

	if (!t6->cur.uploaded || memcmp(t6->cur.img, t6->cur.shown, T6_CUR_BYTES))
		if (t6_cursor_upload(t6))
			goto out;
	t6_cursor_move(t6, x, y);
	t6_cursor_show(t6, true);
out:
	drm_dev_exit(idx);
}

static const struct drm_plane_helper_funcs t6_cursor_helper_funcs = {
	DRM_GEM_SHADOW_PLANE_HELPER_FUNCS,
	.atomic_check = t6_cursor_atomic_check,
	.atomic_update = t6_cursor_atomic_update,
};

static const struct drm_plane_funcs t6_cursor_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	DRM_GEM_SHADOW_PLANE_FUNCS,
};

/* ------------------------------------------------------------------ */
/* CRTC / encoder / connector					      */

static int t6_crtc_atomic_check(struct drm_crtc *crtc, struct drm_atomic_commit *state)
{
	struct mct_t6 *t6 = to_t6(crtc->dev);
	struct drm_crtc_state *crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	struct t6_mode m;

	if (crtc_state->enable && t6_mode_entry(t6, &crtc_state->mode, &m))
		return -EINVAL;
	return drm_crtc_helper_atomic_check(crtc, state);
}

static void t6_crtc_atomic_enable(struct drm_crtc *crtc, struct drm_atomic_commit *state)
{
	struct mct_t6 *t6 = to_t6(crtc->dev);
	struct drm_crtc_state *crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	struct t6_mode m;
	int idx;

	if (t6_mode_entry(t6, &crtc_state->mode, &m))
		return;
	if (drm_dev_enter(crtc->dev, &idx)) {
		if (t6_set_mode(t6, &m) || t6_ready(t6) || t6_set_output(t6, true))
			drm_err(&t6->dev, "could not set mode %dx%d\n",
				crtc_state->mode.hdisplay, crtc_state->mode.vdisplay);
		drm_dev_exit(idx);
	}
	t6->cur.uploaded = false;	/* upload the image again after a mode set */
	t6->cur.visible = false;
	mutex_lock(&t6->upd.lock);
	/* nothing is known about the frame buffers after a mode set */
	memset(t6->hw_valid, 0, sizeof(t6->hw_valid));
	t6->buf = 0;
	t6->width = crtc_state->mode.hdisplay;
	t6->height = crtc_state->mode.vdisplay;
	memset(t6->shadow, 0, (size_t)t6->width * t6->height * 4);
	mutex_unlock(&t6->upd.lock);
	drm_crtc_vblank_on(crtc);
}

static void t6_crtc_atomic_disable(struct drm_crtc *crtc, struct drm_atomic_commit *state)
{
	struct mct_t6 *t6 = to_t6(crtc->dev);
	int idx;

	drm_crtc_vblank_off(crtc);
	t6_stop_updates(t6);
	mutex_lock(&t6->upd.lock);
	t6->width = 0;
	t6->height = 0;
	mutex_unlock(&t6->upd.lock);
	if (drm_dev_enter(crtc->dev, &idx)) {
		t6_cursor_show(t6, false);
		t6_set_output(t6, false);
		drm_dev_exit(idx);
	}
}

/*
 * No vblank interrupt is used; a hrtimer at the mode's refresh rate (DRM
 * vblank timer helpers) paces page-flip events.
 */
static const struct drm_crtc_helper_funcs t6_crtc_helper_funcs = {
	.atomic_check = t6_crtc_atomic_check,
	.atomic_flush = drm_crtc_vblank_atomic_flush,
	.atomic_enable = t6_crtc_atomic_enable,
	.atomic_disable = t6_crtc_atomic_disable,
};

static const struct drm_crtc_funcs t6_crtc_funcs = {
	.reset = drm_atomic_helper_crtc_reset,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
	DRM_CRTC_VBLANK_TIMER_FUNCS,
};

static const struct drm_encoder_funcs t6_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

static int t6_read_edid_block(void *data, u8 *buf, unsigned int block, size_t len)
{
	struct mct_t6 *t6 = data;
	int ret;

	mutex_lock(&t6->io_lock);
	ret = t6_ctrl_in(t6, T6_REQ_EDID, block * EDID_LENGTH, t6->output, len);
	if (ret == len)
		memcpy(buf, t6->ctrl, len);
	mutex_unlock(&t6->io_lock);
	return ret == len ? 0 : -EIO;
}

/*
 * Reduced blanking as tested on the T6 (80 pixels horizontal blanking, at
 * least 460 us vertical blanking): low pixel clocks for high refresh rates.
 */
static struct drm_display_mode *t6_rb_mode(struct drm_device *dev, int w, int h, int hz)
{
	struct drm_display_mode *mode;
	int vtotal = h + 20;

	while ((u64)(vtotal - h) * 1000000 < (u64)460 * hz * vtotal)
		vtotal++;
	mode = drm_mode_create(dev);
	if (!mode)
		return NULL;
	mode->hdisplay = w;
	mode->hsync_start = w + 8;
	mode->hsync_end = w + 40;
	mode->htotal = w + 80;
	mode->vdisplay = h;
	mode->vsync_start = vtotal - 14;
	mode->vsync_end = vtotal - 6;
	mode->vtotal = vtotal;
	mode->clock = DIV_ROUND_CLOSEST(mode->htotal * vtotal * hz, 1000);
	mode->flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC;
	mode->type = DRM_MODE_TYPE_DRIVER;
	drm_mode_set_name(mode);
	return mode;
}

/*
 * Whether the EDID already brings a usable mode of this size and rate. One
 * the T6 can't drive doesn't count: the LG 27GN750's 1080p 144 Hz timing
 * needs 350.4 MHz, while the reduced blanking one here needs 333 MHz.
 */
static bool t6_has_mode(struct drm_connector *connector, int w, int h, int hz)
{
	struct mct_t6 *t6 = to_t6(connector->dev);
	struct drm_display_mode *mode;
	struct t6_mode m;

	list_for_each_entry(mode, &connector->probed_modes, head)
		if (mode->hdisplay == w && mode->vdisplay == h && drm_mode_vrefresh(mode) == hz &&
		    !t6_mode_entry(t6, mode, &m))
			return true;
	return false;
}

/*
 * Higher refresh rates than the EDID lists, up to the monitor's maximum:
 * the range descriptor if the kernel parsed one (EDID 1.4 only), else the
 * highest refresh rate among the EDID's own modes.
 */
static int t6_add_extra_modes(struct drm_connector *connector, int w, int h)
{
	const struct drm_monitor_range_info *range = &connector->display_info.monitor_range;
	struct mct_t6 *t6 = to_t6(connector->dev);
	int min_hz = range->min_vfreq, max_hz = range->max_vfreq;
	struct drm_display_mode *mode;
	struct t6_mode m;
	int i, count = 0;

	if (!max_hz) {
		list_for_each_entry(mode, &connector->probed_modes, head)
			max_hz = max(max_hz, drm_mode_vrefresh(mode));
		min_hz = 0;
	}
	for (i = 0; i < ARRAY_SIZE(t6_extra_hz); i++) {
		int hz = t6_extra_hz[i];

		if (hz < min_hz || hz > max_hz || t6_has_mode(connector, w, h, hz))
			continue;
		mode = t6_rb_mode(connector->dev, w, h, hz);
		if (!mode)
			break;
		if (t6_mode_entry(t6, mode, &m)) {
			drm_mode_destroy(connector->dev, mode);
			continue;
		}
		drm_mode_probed_add(connector, mode);
		count++;
	}
	return count;
}

static int t6_conn_get_modes(struct drm_connector *connector)
{
	struct mct_t6 *t6 = to_t6(connector->dev);
	struct drm_display_mode *mode;
	const struct drm_edid *edid;
	int count, w = 0, h = 0;
	bool has_720p = false;

	edid = drm_edid_read_custom(connector, t6_read_edid_block, t6);
	drm_edid_connector_update(connector, edid);
	count = drm_edid_connector_add_modes(connector);
	drm_edid_free(edid);

	list_for_each_entry(mode, &connector->probed_modes, head) {
		if (mode->type & DRM_MODE_TYPE_PREFERRED && !w) {
			w = mode->hdisplay;
			h = mode->vdisplay;
		}
		if (mode->hdisplay == 1280 && mode->vdisplay == 720)
			has_720p = true;
	}
	if (w)
		count += t6_add_extra_modes(connector, w, h);
	if (has_720p && (w != 1280 || h != 720))
		count += t6_add_extra_modes(connector, 1280, 720);
	return count;
}

static enum drm_mode_status t6_conn_mode_valid(struct drm_connector *connector,
					       const struct drm_display_mode *mode)
{
	struct t6_mode m;

	return t6_mode_entry(to_t6(connector->dev), mode, &m) ? MODE_CLOCK_RANGE : MODE_OK;
}

static int t6_conn_detect(struct drm_connector *connector, struct drm_modeset_acquire_ctx *ctx,
			  bool force)
{
	struct mct_t6 *t6 = to_t6(connector->dev);
	int idx, ret = connector_status_unknown;

	if (!drm_dev_enter(connector->dev, &idx))
		return connector_status_disconnected;
	ret = t6_output_status(t6, t6->output);
	drm_dev_exit(idx);
	if (ret < 0)
		return connector_status_unknown;
	return ret == 1 ? connector_status_connected : connector_status_disconnected;
}

static const struct drm_connector_helper_funcs t6_conn_helper_funcs = {
	.get_modes = t6_conn_get_modes,
	.mode_valid = t6_conn_mode_valid,
	.detect_ctx = t6_conn_detect,
};

static const struct drm_connector_funcs t6_conn_funcs = {
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

/* ------------------------------------------------------------------ */
/* debugfs							      */

static int t6_stats_show(struct seq_file *m, void *data)
{
	struct drm_debugfs_entry *entry = m->private;
	struct mct_t6 *t6 = to_t6(entry->dev);
	u64 ms = max_t(u64, ktime_ms_delta(ktime_get(), t6->stats.since), 1);
	u64 n = max_t(u64, t6->stats.updates, 1);

	seq_printf(m, "seconds          %llu\n", ms / 1000);
	seq_printf(m, "mode             %dx%d (output %d)\n", t6->width, t6->height, t6->output);
	seq_printf(m, "updates          %llu (%llu.%llu per second)\n", t6->stats.updates,
		   div64_u64(t6->stats.updates * 1000, ms),
		   div64_u64(t6->stats.updates * 10000, ms) % 10);
	seq_printf(m, "skipped images   %llu\n", t6->stats.skipped);
	seq_printf(m, "pixels damaged   %llu\n", t6->stats.px_damage);
	seq_printf(m, "rows sent        %llu in %llu bands (%llu per update)\n",
		   t6->stats.rows, t6->stats.bands, div64_u64(t6->stats.rows, n));
	seq_printf(m, "usb bytes        %llu (avg %llu KB/s)\n",
		   t6->stats.bytes, div64_u64(t6->stats.bytes, ms));
	seq_printf(m, "ms per update    convert %llu.%02llu, usb %llu.%02llu\n",
		   div64_u64(t6->stats.ns_conv, n * 1000000),
		   div64_u64(t6->stats.ns_conv, n * 10000) % 100,
		   div64_u64(t6->stats.ns_usb, n * 1000000),
		   div64_u64(t6->stats.ns_usb, n * 10000) % 100);
	seq_printf(m, "ms per fb copy   %llu.%02llu (max %llu.%02llu) over %llu copies\n",
		   div64_u64(t6->stats.ns_copy, max_t(u64, t6->stats.copies, 1) * 1000000),
		   div64_u64(t6->stats.ns_copy, max_t(u64, t6->stats.copies, 1) * 10000) % 100,
		   div64_u64(t6->stats.ns_copy_max, 1000000),
		   div64_u64(t6->stats.ns_copy_max, 10000) % 100, t6->stats.copies);
	seq_printf(m, "ms per commit    %llu.%02llu (max %llu.%02llu) incl. cpu access setup\n",
		   div64_u64(t6->stats.ns_commit, max_t(u64, t6->stats.copies, 1) * 1000000),
		   div64_u64(t6->stats.ns_commit, max_t(u64, t6->stats.copies, 1) * 10000) % 100,
		   div64_u64(t6->stats.ns_commit_max, 1000000),
		   div64_u64(t6->stats.ns_commit_max, 10000) % 100);
	seq_printf(m, "usb MB/s while busy %llu\n",
		   div64_u64(t6->stats.bytes * 1000, max_t(u64, t6->stats.ns_usb, 1)));
	return 0;
}

/* ------------------------------------------------------------------ */
/* driver							      */

DEFINE_DRM_GEM_FOPS(t6_fops);

static const struct drm_driver t6_drm_driver = {
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC,
	.name = DRIVER_NAME,
	.desc = DRIVER_DESC,
	.major = DRIVER_MAJOR,
	.minor = DRIVER_MINOR,
	.patchlevel = DRIVER_PATCH,
	.fops = &t6_fops,
	DRM_GEM_SHMEM_DRIVER_OPS,
	DRM_FBDEV_SHMEM_DRIVER_OPS,
};

static const struct drm_mode_config_funcs t6_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static int t6_modeset_init(struct mct_t6 *t6)
{
	struct drm_device *dev = &t6->dev;
	int ret;

	ret = drmm_mode_config_init(dev);
	if (ret)
		return ret;
	dev->mode_config.min_width = 64;
	dev->mode_config.max_width = t6->max_w;
	dev->mode_config.min_height = 64;
	dev->mode_config.max_height = t6->max_h;
	dev->mode_config.cursor_width = T6_CUR;
	dev->mode_config.cursor_height = T6_CUR;
	dev->mode_config.funcs = &t6_mode_config_funcs;

	ret = drm_universal_plane_init(dev, &t6->primary, 0, &t6_primary_funcs,
				       t6_formats, ARRAY_SIZE(t6_formats),
				       t6_modifiers, DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ret;
	drm_plane_helper_add(&t6->primary, &t6_primary_helper_funcs);
	drm_plane_enable_fb_damage_clips(&t6->primary);

	ret = drm_universal_plane_init(dev, &t6->cursor, 0, &t6_cursor_funcs,
				       t6_cursor_formats, ARRAY_SIZE(t6_cursor_formats),
				       t6_modifiers, DRM_PLANE_TYPE_CURSOR, NULL);
	if (ret)
		return ret;
	drm_plane_helper_add(&t6->cursor, &t6_cursor_helper_funcs);

	ret = drm_crtc_init_with_planes(dev, &t6->crtc, &t6->primary, &t6->cursor,
					&t6_crtc_funcs, NULL);
	if (ret)
		return ret;
	drm_crtc_helper_add(&t6->crtc, &t6_crtc_helper_funcs);

	ret = drm_encoder_init(dev, &t6->encoder, &t6_encoder_funcs, DRM_MODE_ENCODER_TMDS, NULL);
	if (ret)
		return ret;
	t6->encoder.possible_crtcs = drm_crtc_mask(&t6->crtc);

	drm_connector_helper_add(&t6->conn, &t6_conn_helper_funcs);
	ret = drm_connector_init(dev, &t6->conn, &t6_conn_funcs, DRM_MODE_CONNECTOR_HDMIA);
	if (ret)
		return ret;
	t6->conn.polled = DRM_CONNECTOR_POLL_CONNECT | DRM_CONNECTOR_POLL_DISCONNECT;
	ret = drm_connector_attach_encoder(&t6->conn, &t6->encoder);
	if (ret)
		return ret;

	ret = drm_vblank_init(dev, 1);
	if (ret)
		return ret;

	drm_mode_config_reset(dev);
	return 0;
}

static void t6_release(struct drm_device *dev, void *res)
{
	struct mct_t6 *t6 = to_t6(dev);

	kvfree(t6->shadow);
	kvfree(t6->img);
	kvfree(t6->hw[0]);
	kvfree(t6->hw[1]);
	kvfree(t6->hw[2]);
}

/* 0xb0 index 0: 0 = "Lite" with a 48 MHz PLL base, 1 = "Super Lite", 40 MHz */
static void t6_read_platform(struct mct_t6 *t6)
{
	int ret;

	mutex_lock(&t6->io_lock);
	ret = t6_ctrl_in(t6, 0xb0, 0, 0, 4);
	t6->pll_base_khz = (ret == 4 && t6->ctrl[0] == 0) ? 48000 : 40000;
	mutex_unlock(&t6->io_lock);
}

/*
 * The 5601 in the vendor capture reports its HDMI port as output 1, the
 * 5600 here as output 0: use the first one with a monitor, else 0.
 */
static void t6_pick_output(struct mct_t6 *t6)
{
	int i;

	t6->output = 0;
	for (i = 0; i < T6_MAX_OUTPUTS; i++) {
		if (t6_output_status(t6, i) == 1) {
			t6->output = i;
			return;
		}
	}
}

static int t6_usb_probe(struct usb_interface *interface, const struct usb_device_id *id)
{
	struct mct_t6 *t6;
	struct drm_device *dev;
	size_t px;
	int ret;

	BUILD_BUG_ON(sizeof(struct t6_mode) != 32);
	BUILD_BUG_ON(sizeof(struct t6_session) != 32);
	BUILD_BUG_ON(sizeof(struct t6_video_header) != 48);

	if (interface->cur_altsetting->desc.bInterfaceNumber != 0)
		return -ENODEV;

	t6 = devm_drm_dev_alloc(&interface->dev, &t6_drm_driver, struct mct_t6, dev);
	if (IS_ERR(t6))
		return PTR_ERR(t6);
	dev = &t6->dev;

	INIT_WORK(&t6->upd.work, t6_update_work);
	mutex_init(&t6->upd.lock);
	mutex_init(&t6->io_lock);

	t6->ctrl = drmm_kmalloc(dev, T6_CTRL_BUF, GFP_KERNEL);
	t6->xfer = drmm_kmalloc(dev, T6_FRAG, GFP_KERNEL);
	t6->cur.img = drmm_kmalloc(dev, T6_CUR_BYTES, GFP_KERNEL);
	t6->cur.shown = drmm_kmalloc(dev, T6_CUR_BYTES, GFP_KERNEL);
	if (!t6->ctrl || !t6->xfer || !t6->cur.img || !t6->cur.shown)
		return -ENOMEM;

	t6_pick_output(t6);
	t6_read_platform(t6);
	ret = t6_read_modes(t6);
	if (ret) {
		drm_err(dev, "could not read the mode table: %d\n", ret);
		return ret;
	}

	px = (size_t)t6->max_w * t6->max_h;
	t6->img = kvmalloc(px * 3 / 2, GFP_KERNEL);
	t6->hw[0] = kvmalloc(px * 3 / 2, GFP_KERNEL);
	t6->hw[1] = kvmalloc(px * 3 / 2, GFP_KERNEL);
	t6->hw[2] = kvmalloc(px * 3 / 2, GFP_KERNEL);
	t6->shadow = kvzalloc(px * 4, GFP_KERNEL);
	ret = drmm_add_action_or_reset(dev, t6_release, NULL);
	if (ret)
		return ret;
	if (!t6->shadow || !t6->img || !t6->hw[0] || !t6->hw[1] || !t6->hw[2])
		return -ENOMEM;

	ret = t6_modeset_init(t6);
	if (ret)
		return ret;

	t6->stats.since = ktime_get();
	drm_debugfs_add_file(dev, "mct_t6_stats", t6_stats_show, NULL);

	usb_set_intfdata(interface, dev);
	drm_kms_helper_poll_init(dev);
	ret = drm_dev_register(dev, 0);
	if (ret)
		return ret;

	drm_client_setup(dev, NULL);
	return 0;
}

static void t6_usb_disconnect(struct usb_interface *interface)
{
	struct drm_device *dev = usb_get_intfdata(interface);

	drm_kms_helper_poll_fini(dev);
	drm_dev_unplug(dev);
	drm_atomic_helper_shutdown(dev);
}

static int t6_suspend(struct usb_interface *interface, pm_message_t message)
{
	return drm_mode_config_helper_suspend(usb_get_intfdata(interface));
}

static int t6_resume(struct usb_interface *interface)
{
	return drm_mode_config_helper_resume(usb_get_intfdata(interface));
}

static const struct usb_device_id id_table[] = {
	{ USB_DEVICE_AND_INTERFACE_INFO(0x0711, 0x5600, 0xff, 0x00, 0x00) },
	{ USB_DEVICE_AND_INTERFACE_INFO(0x0711, 0x5601, 0xff, 0x00, 0x00) },
	{},
};
MODULE_DEVICE_TABLE(usb, id_table);

static struct usb_driver t6_usb_driver = {
	.name = DRIVER_NAME,
	.probe = t6_usb_probe,
	.disconnect = t6_usb_disconnect,
	.id_table = id_table,
	.soft_unbind = 1,
	.suspend = pm_ptr(t6_suspend),
	.resume = pm_ptr(t6_resume),
	.reset_resume = pm_ptr(t6_resume),
};

module_usb_driver(t6_usb_driver);
MODULE_DESCRIPTION("DRM driver for MCT Trigger 6 USB displays");
MODULE_LICENSE("GPL");
