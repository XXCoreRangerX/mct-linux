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
#include <drm/drm_color_mgmt.h>
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

static char *format = "auto";
module_param(format, charp, 0644);
MODULE_PARM_DESC(format, "Pixel format sent to the device: auto (exact RGB at rest, NV12 during large changes; default), rgb24, rgb32, yuv444, nv12. The full colour formats are only used where the device can scan them out at the current mode.");

#define DRIVER_NAME	"mct_t6"
#define DRIVER_DESC	"MCT Trigger 6 USB display"
#define DRIVER_MAJOR	0
#define DRIVER_MINOR	15
#define DRIVER_PATCH	0

#define T6_EP_OUT	0x02
#define T6_EVENT_LEN	64		/* EP 0x83 interrupt packets */
/* byte 0x13 of an event: what happened */
#define T6_EV_CHANGE0	0x01		/* monitor change on output 0 */
#define T6_EV_CHANGE1	0x02		/* ... output 1 */
#define T6_EV_DONE	0x04		/* frame done, fence ID at 0x0c */

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
#define T6_URBS		16		/* bulk transfers in flight at most */
#define T6_CTRL_BUF	512
#define T6_TIMEOUT	2000

#define T6_FMT_NV12	0x6
#define T6_FMT_RGB32	0x8		/* B, G, R, X */
#define T6_FMT_BGR24	0x9		/* B, G, R */
#define T6_FMT_YUV444	0xb

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
 * rate (artifacts at 353, no signal from 360; see docs/t6-lab-notebook.md),
 * well above what the mode table offers. At 348 MHz (1080p150) a few green
 * pixels showed up in daily use, so keep a margin: 1080p144 is 333 MHz.
 */
#define T6_MAX_CLOCK_KHZ	340000
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

enum t6_kind { T6_NV12, T6_YUV444, T6_RGB24, T6_RGB32 };

static const char *const t6_kind_name[] = { "NV12", "YUV 4:4:4", "RGB24", "RGB32" };
/* bytes per pixel the device reads per refresh */
static const unsigned int t6_scan_x2[] = { 3, 6, 6, 8 };	/* times two */

/*
 * Image layout. On the host every plane is packed (row length = width x
 * bytes per pixel); in device memory the planes follow the frame header at
 * the buffer's +0x30.
 * NV12: Y, then interleaved UV at half height (2 Y rows and 1 UV row per
 * row pair). YUV 4:4:4: Y, U, V, full height, rows aligned to 32 pixels.
 * RGB24 / RGB32: one plane of B, G, R(, X).
 */
struct t6_layout {
	enum t6_kind kind;
	int planes;			/* 0: not set up */
	int rows[3];			/* rows per row pair */
	int bpp[3];			/* bytes per pixel in a row */
	size_t host[3];			/* plane offsets in img/hw */
	u32 dev[3];			/* plane offsets from buffer + 0x30 */
	int pitch;			/* device row pitch in bytes */
	u32 fmt;
};

/*
 * Scanout reads the frame buffer from the T6's own memory once per refresh.
 * 4:4:4 at 1080p glitched at 144 Hz (~900 MB/s) and was clean at 120 Hz
 * (~750 MB/s); above this the driver uses NV12 instead.
 */
#define T6_SCANOUT_MAX	(760ull * 1000 * 1000)	/* bytes per second */

/* format=auto: an update changing more than a quarter of the screen is "large" */
#define T6_SETTLE_MS	150		/* back to full colour this long after the last one */

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

struct mct_t6;

struct t6_urb {
	struct mct_t6 *t6;
	struct urb *urb;
	u8 *buf;			/* T6_FRAG bytes, DMA coherent */
	struct list_head node;		/* in the free list while idle */
};

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

	struct mutex io_lock;		/* USB transfers and the ctrl buffer */
	u8 *ctrl;			/* DMA-able control transfer buffer */

	/* bulk transfers are queued without waiting; the endpoint keeps their order */
	struct t6_urb urbs[T6_URBS];
	struct list_head urb_free;
	spinlock_t urb_lock;		/* urb_free */
	wait_queue_head_t urb_wait;	/* for a URB to become free */
	struct usb_anchor anchor;	/* URBs in flight */
	atomic_t urb_err;		/* last error of a completed URB */
	u8 *shadow;			/* XRGB8888 image as committed by userspace */
	bool shadow_valid;		/* holds a whole committed image */
	u8 *rowmask;			/* rows to copy from the framebuffer */
	u8 *img;			/* the shadow in NV12 */
	u8 lut[3][256];			/* gamma, R G B, applied in software */
	struct t6_layout lay;		/* of img and hw, set by the worker */
	u8 *hw[3];			/* NV12 image each frame buffer holds */
	bool hw_valid[3];
	int width, height;		/* current mode */
	int refresh;
	unsigned int buf;		/* next frame buffer to write */
	u32 seq;

	struct {
		struct work_struct work;
		struct mutex lock;	/* protects shadow + dirty */
		bool dirty;
	} upd;

	struct delayed_work settle_work;	/* back to 4:4:4 after motion */
	ktime_t last_large;		/* last large update */

	struct urb *ev_urb;		/* interrupt IN, always queued */
	struct work_struct hpd_work;

	struct {
		u8 *img;		/* header + image being built */
		u8 *shown;		/* last uploaded */
		int slot;		/* 0 or 1: which of our two slots is in use */
		bool uploaded, visible;
	} cur;

	struct {			/* debugfs statistics */
		u64 updates, skipped, px_damage, bytes, ns_conv, ns_usb;
		u64 rows, bands;		/* image rows and bands sent */
		u64 to_420;			/* switches to 4:2:0 for motion */
		u64 ev_done, ev_change, ev_other;	/* interrupt events */
		u64 no_clips, clips, clip_px, rows_copied;	/* damage clips per commit */
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

static void t6_urb_complete(struct urb *urb)
{
	struct t6_urb *u = urb->context;
	struct mct_t6 *t6 = u->t6;
	unsigned long flags;

	if (urb->status && urb->status != -ENOENT && urb->status != -ECONNRESET &&
	    urb->status != -ESHUTDOWN)
		atomic_set(&t6->urb_err, urb->status);
	spin_lock_irqsave(&t6->urb_lock, flags);
	list_add_tail(&u->node, &t6->urb_free);
	spin_unlock_irqrestore(&t6->urb_lock, flags);
	wake_up(&t6->urb_wait);
}

static struct t6_urb *t6_urb_pop(struct mct_t6 *t6)
{
	struct t6_urb *u;

	spin_lock_irq(&t6->urb_lock);
	u = list_first_entry_or_null(&t6->urb_free, struct t6_urb, node);
	if (u)
		list_del(&u->node);
	spin_unlock_irq(&t6->urb_lock);
	return u;
}

/* a free URB to fill, waiting for one to complete if all are in flight */
static struct t6_urb *t6_urb_get(struct mct_t6 *t6)
{
	struct t6_urb *u = NULL;

	wait_event_timeout(t6->urb_wait, (u = t6_urb_pop(t6)), msecs_to_jiffies(T6_TIMEOUT));
	return u;
}

static int t6_urb_send(struct mct_t6 *t6, struct t6_urb *u, int len)
{
	int ret;

	u->urb->transfer_buffer_length = len;
	usb_anchor_urb(u->urb, &t6->anchor);
	ret = usb_submit_urb(u->urb, GFP_KERNEL);
	if (ret) {
		usb_unanchor_urb(u->urb);
		spin_lock_irq(&t6->urb_lock);
		list_add_tail(&u->node, &t6->urb_free);
		spin_unlock_irq(&t6->urb_lock);
		wake_up(&t6->urb_wait);
	}
	return ret;
}

/* queue len bytes on the bulk endpoint */
static int t6_bulk(struct mct_t6 *t6, const void *data, int len)
{
	struct t6_urb *u = t6_urb_get(t6);

	if (!u)
		return -ETIMEDOUT;
	memcpy(u->buf, data, len);
	return t6_urb_send(t6, u, len);
}

static void t6_free_urbs(void *data)
{
	struct mct_t6 *t6 = data;
	struct usb_device *udev = t6_udev(t6);
	int i;

	usb_kill_anchored_urbs(&t6->anchor);
	for (i = 0; i < T6_URBS; i++) {
		if (t6->urbs[i].buf)
			usb_free_coherent(udev, T6_FRAG, t6->urbs[i].buf,
					  t6->urbs[i].urb->transfer_dma);
		usb_free_urb(t6->urbs[i].urb);
	}
}

static int t6_alloc_urbs(struct mct_t6 *t6, struct usb_interface *intf)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	int i, ret;

	INIT_LIST_HEAD(&t6->urb_free);
	spin_lock_init(&t6->urb_lock);
	init_waitqueue_head(&t6->urb_wait);
	init_usb_anchor(&t6->anchor);
	ret = devm_add_action(&intf->dev, t6_free_urbs, t6);
	if (ret)
		return ret;
	for (i = 0; i < T6_URBS; i++) {
		struct t6_urb *u = &t6->urbs[i];

		u->t6 = t6;
		u->urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!u->urb)
			return -ENOMEM;
		u->buf = usb_alloc_coherent(udev, T6_FRAG, GFP_KERNEL, &u->urb->transfer_dma);
		if (!u->buf)
			return -ENOMEM;
		usb_fill_bulk_urb(u->urb, udev, usb_sndbulkpipe(udev, T6_EP_OUT), u->buf,
				  T6_FRAG, t6_urb_complete, u);
		u->urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
		list_add_tail(&u->node, &t6->urb_free);
	}
	return 0;
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

		/* the frame paths are sized for these at most */
		if (!w || !h || w > 4096 || h > 2304)
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
	    0x30 + 4ull * round_up(mode->hdisplay, 32) * mode->vdisplay > T6_BUF_SIZE)
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

static void t6_set_layout(struct mct_t6 *t6, enum t6_kind kind)
{
	struct t6_layout *l = &t6->lay;
	int w = t6->width, h = t6->height, i;
	size_t px = (size_t)w * h;

	memset(l, 0, sizeof(*l));
	l->kind = kind;
	switch (kind) {
	case T6_NV12:
		l->planes = 2;
		l->pitch = w;
		l->fmt = T6_FMT_NV12;
		l->rows[0] = 2;
		l->rows[1] = 1;
		l->bpp[0] = 1;
		l->bpp[1] = 1;		/* U, V for every other pixel */
		l->host[1] = px;
		l->dev[1] = w * h;
		break;
	case T6_YUV444:
		l->planes = 3;
		l->pitch = round_up(w, 32);
		l->fmt = T6_FMT_YUV444;
		for (i = 0; i < 3; i++) {
			l->rows[i] = 2;
			l->bpp[i] = 1;
			l->host[i] = i * px;
			l->dev[i] = i * l->pitch * h;
		}
		break;
	case T6_RGB24:
		l->planes = 1;
		l->pitch = round_up(w, 32) * 3;
		l->fmt = T6_FMT_BGR24;
		l->rows[0] = 2;
		l->bpp[0] = 3;
		break;
	case T6_RGB32:
		l->planes = 1;
		l->pitch = w * 4;
		l->fmt = T6_FMT_RGB32;
		l->rows[0] = 2;
		l->bpp[0] = 4;
		break;
	}
	/* nothing in the frame buffers is in the new layout */
	memset(t6->hw_valid, 0, sizeof(t6->hw_valid));
}

/*
 * Shadow (XRGB8888) to t6->img in the current layout, through the gamma
 * tables. YUV is BT.601 limited range; NV12 takes the chroma of each 2x2
 * block. Modes have even sizes. upd.lock held.
 */
static void t6_convert(struct mct_t6 *t6)
{
	const u8 *lr = t6->lut[0], *lg = t6->lut[1], *lb = t6->lut[2];
	int w = t6->width, h = t6->height, x, y, i;
	size_t px = (size_t)w * h;
	const u8 *s = t6->shadow;	/* B, G, R, X */
	u8 *py = t6->img;

	switch (t6->lay.kind) {
	case T6_RGB24:
		for (i = 0; i < px; i++, s += 4, py += 3) {
			py[0] = lb[s[0]];
			py[1] = lg[s[1]];
			py[2] = lr[s[2]];
		}
		return;
	case T6_RGB32:
		for (i = 0; i < px; i++, s += 4, py += 4) {
			py[0] = lb[s[0]];
			py[1] = lg[s[1]];
			py[2] = lr[s[2]];
			py[3] = 0;
		}
		return;
	case T6_YUV444: {
		u8 *pu = py + px, *pv = pu + px;

		for (i = 0; i < px; i++, s += 4) {
			int r = lr[s[2]], g = lg[s[1]], b = lb[s[0]];

			py[i] = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
			pu[i] = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
			pv[i] = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
		}
		return;
	}
	case T6_NV12:
		break;
	}

	for (y = 0; y < h; y += 2) {
		const u8 *s0 = t6->shadow + (size_t)y * w * 4;
		const u8 *s1 = s0 + w * 4;
		u8 *y0 = py + (size_t)y * w, *y1 = y0 + w;
		u8 *uv = py + px + (size_t)(y / 2) * w;

		for (x = 0; x < w; x += 2, s0 += 8, s1 += 8, y0 += 2, y1 += 2, uv += 2) {
			/* the 2x2 block: top left, top right, bottom left, bottom right */
			const u8 *p4[4] = { s0, s0 + 4, s1, s1 + 4 };
			u8 *out[4] = { y0, y0 + 1, y1, y1 + 1 };
			int r = 0, g = 0, b = 0;

			for (i = 0; i < 4; i++) {
				int pr = lr[p4[i][2]], pg = lg[p4[i][1]], pb = lb[p4[i][0]];

				*out[i] = ((66 * pr + 129 * pg + 25 * pb + 128) >> 8) + 16;
				r += pr;
				g += pg;
				b += pb;
			}
			/* chroma of the 2x2 block: sums are 4x, so shift by 10 */
			uv[0] = ((-38 * r - 74 * g + 112 * b + 512) >> 10) + 128;
			uv[1] = ((112 * r - 94 * g - 18 * b + 512) >> 10) + 128;
		}
	}
}

struct t6_band {
	int p0, p1;			/* row pairs [p0, p1) */
	int x0, x1;			/* columns [x0, x1), multiples of 16 */
};

/* first and last differing byte of two rows, as [*lo, *hi); false if equal */
static bool t6_row_diff(const u8 *a, const u8 *b, int n, int *lo, int *hi)
{
	int l = 0, h = n;

	if (!memcmp(a, b, n))
		return false;
	while (l + 8 <= n && !memcmp(a + l, b + l, 8))
		l += 8;
	while (h - 8 >= l && !memcmp(a + h - 8, b + h - 8, 8))
		h -= 8;
	*lo = min(*lo, l);
	*hi = max(*hi, h);
	return true;
}

/* host offset of row r of row pair p in plane i */
static size_t t6_row(const struct mct_t6 *t6, int i, int p, int r)
{
	return t6->lay.host[i] + ((size_t)p * t6->lay.rows[i] + r) * t6->width * t6->lay.bpp[i];
}

/*
 * Bands of row pairs where t6->img differs from what frame buffer k holds,
 * each with the columns that changed; returns the count.
 */
static int t6_diff_bands(struct mct_t6 *t6, int k, struct t6_band *bands)
{
	int w = t6->width, pairs = t6->height / 2, n = 0, p, i, r;

	if (!t6->hw_valid[k]) {
		bands[0] = (struct t6_band){ 0, pairs, 0, w };
		return 1;
	}
	for (p = 0; p < pairs; p++) {
		int lo = w, hi = 0;
		bool d = false;

		for (i = 0; i < t6->lay.planes; i++) {
			int bpp = t6->lay.bpp[i], l = w * bpp, hb = 0;

			for (r = 0; r < t6->lay.rows[i]; r++) {
				size_t o = t6_row(t6, i, p, r);

				if (t6_row_diff(t6->img + o, t6->hw[k] + o, w * bpp, &l, &hb)) {
					d = true;
					lo = min(lo, l / bpp);
					hi = max(hi, DIV_ROUND_UP(hb, bpp));
				}
			}
		}
		if (!d)
			continue;
		lo = round_down(lo, 16);
		hi = min(round_up(hi, 16), w);
		if (n && (p - bands[n - 1].p1 <= T6_BAND_GAP || n == T6_MAX_BANDS)) {
			bands[n - 1].p1 = p + 1;
			bands[n - 1].x0 = min(bands[n - 1].x0, lo);
			bands[n - 1].x1 = max(bands[n - 1].x1, hi);
		} else {
			bands[n++] = (struct t6_band){ p, p + 1, lo, hi };
		}
	}
	return n;
}

/*
 * One session 7 write: rows [ya, ya + 2n) of host plane ia to device plane
 * ia, then n rows from row yb of host plane ib to device plane ib (the
 * write's "UV" part, half the height). Both planes have the same bytes per
 * pixel. io_lock held.
 */
static int t6_write_rect(struct mct_t6 *t6, int k, const struct t6_band *b,
			 int ia, int ya, int ib, int yb, int n)
{
	int bpp = t6->lay.bpp[ia], pitch = t6->lay.pitch, i;
	int rb = t6->width * bpp, bw = (b->x1 - b->x0) * bpp, xo = b->x0 * bpp;
	u32 base = t6_bufs[k] + 0x30;
	struct t6_rect_header r = { };
	struct t6_urb *u;
	u8 *d;
	int ret;

	r.type = cpu_to_le32(7);
	r.len = cpu_to_le32(3 * n * bw);
	r.y_addr = cpu_to_le32(base + t6->lay.dev[ia] + ya * pitch + xo);
	r.uv_addr = cpu_to_le32(base + t6->lay.dev[ib] + yb * pitch + xo);
	r.y_pitch = cpu_to_le16(pitch);
	r.uv_pitch = cpu_to_le16(pitch);
	r.width = cpu_to_le16(bw);
	r.height = cpu_to_le16(2 * n);
	ret = t6_bulk(t6, &r, sizeof(r));
	if (ret)
		return ret;
	u = t6_urb_get(t6);
	if (!u)
		return -ETIMEDOUT;
	d = u->buf;
	for (i = 0; i < 2 * n; i++, d += bw)
		memcpy(d, t6->img + t6->lay.host[ia] + (size_t)(ya + i) * rb + xo, bw);
	for (i = 0; i < n; i++, d += bw)
		memcpy(d, t6->img + t6->lay.host[ib] + (size_t)(yb + i) * rb + xo, bw);
	t6->stats.bytes += sizeof(r) + 3 * n * bw;
	return t6_urb_send(t6, u, 3 * n * bw);
}

/*
 * Write a band of t6->img into frame buffer k with session 7 writes, which
 * fill a Y plane and a half-height UV plane. NV12 needs one per piece; 4:4:4
 * two: Y with the first half of U, V with the second half. io_lock held.
 */
static int t6_send_band(struct mct_t6 *t6, int k, const struct t6_band *b)
{
	int bw = (b->x1 - b->x0) * t6->lay.bpp[0];
	/* each write fits a transfer: 2n rows + n rows of bw bytes */
	int step = max(T6_FRAG / (3 * bw), 1);
	int p, n, y, ys, y1, ret = 0;

	if (t6->lay.planes == 1) {
		/*
		 * Packed RGB: one plane, so each write takes 3n consecutive rows
		 * (2n in the first part, n in the second). The last write may
		 * start earlier to stay on screen; rewriting rows is harmless.
		 */
		y1 = 2 * b->p1;
		for (y = 2 * b->p0; y < y1 && !ret; y += 3 * n) {
			n = min(step, DIV_ROUND_UP(y1 - y, 3));
			ys = min(y, t6->height - 3 * n);
			ret = t6_write_rect(t6, k, b, 0, ys, 0, ys + 2 * n, n);
			t6->stats.rows += 3 * n;
		}
		return ret;
	}
	for (p = b->p0; p < b->p1 && !ret; p += n) {
		n = min(step, b->p1 - p);
		if (t6->lay.kind == T6_NV12) {
			ret = t6_write_rect(t6, k, b, 0, 2 * p, 1, p, n);
		} else {
			ret = t6_write_rect(t6, k, b, 0, 2 * p, 1, 2 * p, n);
			if (!ret)
				ret = t6_write_rect(t6, k, b, 2, 2 * p, 1, 2 * p + n, n);
		}
		t6->stats.rows += 2 * n;
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
	const struct t6_layout *l = &t6->lay;
	u32 base = t6_bufs[k] + 0x30;
	int ret;

	f.s.total_len = cpu_to_le32(sizeof(f.v));
	f.s.dest = cpu_to_le32(t6_bufs[k]);
	f.s.frag_len = cpu_to_le32(sizeof(f.v));
	f.v.type = cpu_to_le32(3);
	f.v.seq = cpu_to_le32(++t6->seq);
	f.v.unk4 = cpu_to_le32(l->fmt);			/* surface format */
	f.v.width = cpu_to_le16(l->pitch);		/* plane pitches */
	f.v.height = cpu_to_le16(l->planes > 1 ? l->pitch : 0);
	f.v.start = cpu_to_le32(base + l->dev[0]);	/* plane addresses */
	f.v.end = cpu_to_le32(base + l->dev[l->planes > 1]);
	f.v.unk9 = cpu_to_le32(l->planes > 2 ? base + l->dev[2] :
			       l->planes > 1 ? base : 0);
	f.v.format = cpu_to_le32(l->fmt);

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
static int t6_present(struct mct_t6 *t6, const struct t6_band *bands, int n)
{
	int k = t6->buf;
	int i, j, p, r, ret = 0;

	t6->hw_valid[k] = false;
	for (i = 0; i < n && !ret; i++)
		ret = t6_send_band(t6, k, &bands[i]);
	if (!ret)
		ret = t6_flip(t6, k);
	if (ret)
		return ret;

	for (i = 0; i < n; i++) {
		for (p = bands[i].p0; p < bands[i].p1; p++)
			for (j = 0; j < t6->lay.planes; j++) {
				int bpp = t6->lay.bpp[j], bw = (bands[i].x1 - bands[i].x0) * bpp;

				for (r = 0; r < t6->lay.rows[j]; r++) {
					size_t o = t6_row(t6, j, p, r) + bands[i].x0 * bpp;

					memcpy(t6->hw[k] + o, t6->img + o, bw);
				}
			}
	}
	t6->hw_valid[k] = true;
	t6->stats.bands += n;
	t6->buf = (k + 1) % ARRAY_SIZE(t6_bufs);
	return 0;
}

/* motion stopped: send the image again, now in the full colour format */
static void t6_settle_work(struct work_struct *work)
{
	struct mct_t6 *t6 = container_of(to_delayed_work(work), struct mct_t6, settle_work);

	mutex_lock(&t6->upd.lock);
	t6->upd.dirty = true;
	mutex_unlock(&t6->upd.lock);
	queue_work(system_long_wq, &t6->upd.work);
}

/* the format= parameter: the full colour format, and whether to adapt */
static enum t6_kind t6_requested_format(bool *adaptive)
{
	static const struct { const char *name; enum t6_kind kind; } names[] = {
		{ "nv12", T6_NV12 }, { "yuv444", T6_YUV444 },
		{ "rgb24", T6_RGB24 }, { "rgb32", T6_RGB32 },
	};
	enum t6_kind kind = T6_RGB24;
	int i;

	*adaptive = true;
	kernel_param_lock(THIS_MODULE);
	for (i = 0; i < ARRAY_SIZE(names); i++) {
		if (format && sysfs_streq(format, names[i].name)) {
			kind = names[i].kind;
			*adaptive = false;
		}
	}
	kernel_param_unlock(THIS_MODULE);
	return kind;
}

static bool t6_large(const struct mct_t6 *t6, const struct t6_band *bands, int n)
{
	u64 px = 0;
	int i;

	for (i = 0; i < n; i++)
		px += (u64)(bands[i].p1 - bands[i].p0) * 2 * (bands[i].x1 - bands[i].x0);
	return px * 4 > (u64)t6->width * t6->height;
}

static void t6_update_work(struct work_struct *work)
{
	struct mct_t6 *t6 = container_of(work, struct mct_t6, upd.work);
	struct t6_band bands[T6_MAX_BANDS];
	enum t6_kind full, want;
	bool fits, adaptive, known;
	int n, idx, ret;
	ktime_t t0, t1;

	/*
	 * Take the USB lock first: a commit waits on upd.lock, and must not end
	 * up waiting behind a cursor upload or connector poll on io_lock.
	 */
	mutex_lock(&t6->io_lock);
	mutex_lock(&t6->upd.lock);
	if (!t6->upd.dirty || !t6->width) {
		mutex_unlock(&t6->upd.lock);
		mutex_unlock(&t6->io_lock);
		return;
	}
	t6->upd.dirty = false;
	t0 = ktime_get();

	/* a failed transfer may have left any buffer half written */
	ret = atomic_xchg(&t6->urb_err, 0);
	if (ret) {
		drm_err_ratelimited(&t6->dev, "frame transfer error: %d\n", ret);
		memset(t6->hw_valid, 0, sizeof(t6->hw_valid));
	}

	full = t6_requested_format(&adaptive);
	fits = (u64)t6_scan_x2[full] * t6->width * t6->height * t6->refresh <=
	       2 * T6_SCANOUT_MAX;
	if (!fits)
		want = T6_NV12;
	else if (adaptive && ktime_ms_delta(t0, t6->last_large) < T6_SETTLE_MS)
		want = T6_NV12;
	else
		want = full;
	if (want != t6->lay.kind || !t6->lay.planes)
		t6_set_layout(t6, want);
	t6_convert(t6);
	known = t6->hw_valid[t6->buf];
	n = t6_diff_bands(t6, t6->buf, bands);
	/* a whole buffer to refill after a switch doesn't count as motion */
	if (adaptive && fits && known && t6_large(t6, bands, n)) {
		t6->last_large = t0;
		if (t6->lay.kind != T6_NV12) {
			t6->stats.to_420++;
			t6_set_layout(t6, T6_NV12);
			t6_convert(t6);
			n = t6_diff_bands(t6, t6->buf, bands);
		}
		mod_delayed_work(system_long_wq, &t6->settle_work,
				 msecs_to_jiffies(T6_SETTLE_MS));
	}
	/* commits only wait for the conversion above, never for USB */
	mutex_unlock(&t6->upd.lock);
	t1 = ktime_get();

	if (drm_dev_enter(&t6->dev, &idx)) {
		ret = t6_present(t6, bands, n);
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

/* copy the rows set in t6->rowmask from the (mapped) framebuffer, kick the worker */
static void t6_mark_dirty(struct mct_t6 *t6, struct drm_framebuffer *fb,
			  const struct iosys_map *map)
{
	int w = t6->width, y;
	ktime_t t0;
	u64 ns;

	mutex_lock(&t6->upd.lock);
	t0 = ktime_get();
	if (t6->upd.dirty)
		t6->stats.skipped++;	/* the previous image never went out */
	/*
	 * Buffers shared by a GPU are usually mapped write-combined, where plain
	 * reads crawl (~90 MB/s measured with amdgpu); the streaming-load copy
	 * is many times faster.
	 */
	for (y = 0; y < t6->height; ) {
		int n = 0;

		while (y + n < t6->height && t6->rowmask[y + n])
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
			struct iosys_map d = IOSYS_MAP_INIT_VADDR(t6->shadow + (size_t)y * w * 4);
			struct iosys_map sm = *map;

			iosys_map_incr(&sm, (size_t)y * fb->pitches[0]);
			drm_memcpy_from_wc(&d, &sm, (size_t)n * w * 4);
		} else {
			int i;

			for (i = y; i < y + n; i++) {
				u8 *row = t6->shadow + (size_t)i * w * 4;
				struct iosys_map d = IOSYS_MAP_INIT_VADDR(row);
				struct iosys_map sm = *map;

				iosys_map_incr(&sm, (size_t)i * fb->pitches[0]);
				drm_memcpy_from_wc(&d, &sm, w * 4);
			}
		}
		t6->stats.px_damage += (u64)n * w;
		t6->stats.rows_copied += n;
		y += n;
	}
	t6->shadow_valid = true;
	ns = ktime_to_ns(ktime_sub(ktime_get(), t0));
	t6->stats.copies++;
	t6->stats.ns_copy += ns;
	t6->stats.ns_copy_max = max(t6->stats.ns_copy_max, ns);
	t6->upd.dirty = true;
	mutex_unlock(&t6->upd.lock);

	queue_work(system_long_wq, &t6->upd.work);
}

/*
 * Rows to copy for this commit, in t6->rowmask; false if nothing changed.
 *
 * smithay (niri 26.04) computes FB_DAMAGE_CLIPS for a rotated output in the
 * output's rotated coordinates without mapping them to the framebuffer, and
 * the kernel then clips away what lies outside it. So take the raw clips and
 * mark the rows each one would cover under any rotation or flip: correct
 * clips give a few extra rows, unrotated ones still cover the change.
 */
static bool t6_damage_rows(struct mct_t6 *t6, const struct drm_plane_state *state)
{
	unsigned int n = drm_plane_get_damage_clips_count(state), i;
	const struct drm_mode_rect *clips = drm_plane_get_damage_clips(state);
	int h = t6->height, j, k;
	bool any = false;

	if (!n)
		t6->stats.no_clips++;
	t6->stats.clips += n;
	for (i = 0; i < n; i++)
		t6->stats.clip_px += (u64)max(clips[i].x2 - clips[i].x1, 0) *
				     max(clips[i].y2 - clips[i].y1, 0);
	if (!n || !t6->shadow_valid) {
		memset(t6->rowmask, 1, h);
		return true;
	}
	memset(t6->rowmask, 0, h);
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
				t6->rowmask[k] = 1;
				any = true;
			}
		}
	}
	return any;
}

static void t6_stop_updates(struct mct_t6 *t6)
{
	/* the worker re-arms the settle timer and the timer queues the worker */
	do {
		cancel_work_sync(&t6->upd.work);
	} while (cancel_delayed_work_sync(&t6->settle_work));
	cancel_work_sync(&t6->upd.work);
	if (!usb_wait_anchor_empty_timeout(&t6->anchor, T6_TIMEOUT))
		usb_kill_anchored_urbs(&t6->anchor);
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
	/* the whole framebuffer is the image: no panning, no larger buffers */
	if (fb->width != crtc_state->mode.hdisplay || fb->height != crtc_state->mode.vdisplay ||
	    new_state->src_x || new_state->src_y || fb->offsets[0])
		return -EINVAL;
	return 0;
}

static void t6_primary_atomic_update(struct drm_plane *plane, struct drm_atomic_commit *state)
{
	struct mct_t6 *t6 = to_t6(plane->dev);
	struct drm_plane_state *new_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_shadow_plane_state *shadow = to_drm_shadow_plane_state(new_state);
	struct drm_framebuffer *fb = new_state->fb;
	ktime_t t0;
	u64 ns;

	if (!fb || !new_state->visible || !t6->width)
		return;
	if (fb->width != t6->width || fb->height != t6->height)
		goto invalid;
	if (!t6_damage_rows(t6, new_state))
		return;
	t0 = ktime_get();
	if (drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE))
		goto invalid;
	t6_mark_dirty(t6, fb, &shadow->data[0]);
	drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);
	ns = ktime_to_ns(ktime_sub(ktime_get(), t0));
	t6->stats.ns_commit += ns;
	t6->stats.ns_commit_max = max(t6->stats.ns_commit_max, ns);
	return;

invalid:
	/* this commit's changes never reached the shadow: copy all next time */
	mutex_lock(&t6->upd.lock);
	t6->shadow_valid = false;
	mutex_unlock(&t6->upd.lock);
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

/*
 * The device has 5 cursor slots per output (output 1 from slot 5 on). A new
 * image goes into the slot not on screen and is then selected with the show
 * request: overwriting the visible slot left it scrambled until the next
 * show.
 */
static int t6_cursor_slot(struct mct_t6 *t6, int which)
{
	return (t6->output ? 5 : 0) + which;
}

static int t6_cursor_show(struct mct_t6 *t6, bool show)
{
	int ret;

	if (t6->cur.visible == show)
		return 0;
	mutex_lock(&t6->io_lock);
	ret = t6_ctrl_out(t6, T6_REQ_CUR_SHOW(t6->output), t6_cursor_slot(t6, t6->cur.slot),
			  show, 0);
	mutex_unlock(&t6->io_lock);
	if (ret < 0)
		return ret;
	t6->cur.visible = show;
	return 0;
}

/*
 * Image in chunks of up to 512 bytes (wIndex = byte offset) into the other
 * slot, then select that slot; leaves the cursor visible.
 */
static int t6_cursor_upload(struct mct_t6 *t6)
{
	int next = !t6->cur.slot, slot = t6_cursor_slot(t6, next);
	int off, n, ret = 0;

	mutex_lock(&t6->io_lock);
	for (off = 0; off < T6_CUR_BYTES && ret >= 0; off += n) {
		n = min(T6_CUR_BYTES - off, T6_CTRL_BUF);
		memcpy(t6->ctrl, t6->cur.img + off, n);
		ret = t6_ctrl_out(t6, T6_REQ_CUR_IMG(t6->output), slot, off, n);
	}
	if (ret >= 0)
		ret = t6_ctrl_out(t6, T6_REQ_CUR_SHOW(t6->output), slot, 1, 0);
	mutex_unlock(&t6->io_lock);
	if (ret < 0)
		return ret;
	memcpy(t6->cur.shown, t6->cur.img, T6_CUR_BYTES);
	t6->cur.slot = next;
	t6->cur.uploaded = true;
	t6->cur.visible = true;
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
	dst = t6->cur.img + 8;
	for (j = 0; j < T6_CUR; j++) {
		for (i = 0; i < T6_CUR; i++, dst += 4) {
			int vx = i + sx, vy = j + sy;

			if (vx >= fb->width || vy >= fb->height)
				memset(dst, 0, 4);
			else
				iosys_map_memcpy_from(dst, &shadow->data[0],
						      vy * fb->pitches[0] + vx * 4, 4);
		}
	}
	drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);

	t6_cursor_move(t6, x, y);
	if (!t6->cur.uploaded || memcmp(t6->cur.img, t6->cur.shown, T6_CUR_BYTES))
		if (t6_cursor_upload(t6))
			goto out;
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
	if (crtc_state->gamma_lut && drm_color_lut_size(crtc_state->gamma_lut) != 256)
		return -EINVAL;
	return drm_crtc_helper_atomic_check(crtc, state);
}

/*
 * The T6 in hand has no gamma tables (feature level 6; the request stalls),
 * so the DRM gamma LUT is applied during the NV12 conversion. A change
 * reconverts the whole image; the diff then sends what changed.
 */
static void t6_set_gamma(struct mct_t6 *t6, const struct drm_property_blob *blob)
{
	const struct drm_color_lut *lut = blob ? blob->data : NULL;
	bool kick;
	int i;

	mutex_lock(&t6->upd.lock);
	for (i = 0; i < 256; i++) {
		t6->lut[0][i] = lut ? drm_color_lut_extract(lut[i].red, 8) : i;
		t6->lut[1][i] = lut ? drm_color_lut_extract(lut[i].green, 8) : i;
		t6->lut[2][i] = lut ? drm_color_lut_extract(lut[i].blue, 8) : i;
	}
	kick = t6->width && t6->shadow_valid;
	if (kick)
		t6->upd.dirty = true;
	mutex_unlock(&t6->upd.lock);
	if (kick)
		queue_work(system_long_wq, &t6->upd.work);
}

static void t6_crtc_atomic_flush(struct drm_crtc *crtc, struct drm_atomic_commit *state)
{
	struct drm_crtc_state *crtc_state = drm_atomic_get_new_crtc_state(state, crtc);

	if (crtc_state->color_mgmt_changed)
		t6_set_gamma(to_t6(crtc->dev), crtc_state->gamma_lut);
	drm_crtc_vblank_atomic_flush(crtc, state);
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
	t6->lay.planes = 0;		/* the worker sets it up for the new size */
	t6->buf = 0;
	t6->shadow_valid = false;
	t6->refresh = drm_mode_vrefresh(&crtc_state->mode);
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
	.atomic_flush = t6_crtc_atomic_flush,
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
	mode->clock = DIV_ROUND_CLOSEST_ULL((u64)mode->htotal * vtotal * hz, 1000);
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
/* interrupt events						      */

static void t6_hpd_work(struct work_struct *work)
{
	struct mct_t6 *t6 = container_of(work, struct mct_t6, hpd_work);

	drm_helper_hpd_irq_event(&t6->dev);
}

static void t6_event_complete(struct urb *urb)
{
	struct mct_t6 *t6 = urb->context;
	const u8 *ev = urb->transfer_buffer;

	switch (urb->status) {
	case 0:
		break;
	case -ENOENT:
	case -ECONNRESET:
	case -ESHUTDOWN:
	case -EPROTO:
	case -EILSEQ:
	case -EPIPE:
		return;		/* killed, gone or broken: don't spin */
	default:
		goto resubmit;
	}
	if (urb->actual_length > 0x13) {
		if (ev[0x13] & T6_EV_DONE)
			t6->stats.ev_done++;
		if (ev[0x13] & (T6_EV_CHANGE0 | T6_EV_CHANGE1)) {
			t6->stats.ev_change++;
			schedule_work(&t6->hpd_work);
		}
		if (!(ev[0x13] & (T6_EV_DONE | T6_EV_CHANGE0 | T6_EV_CHANGE1)))
			t6->stats.ev_other++;
	}
resubmit:
	usb_submit_urb(urb, GFP_ATOMIC);
}

/* keep an interrupt transfer queued on the event endpoint, as the vendor driver does */
static int t6_events_init(struct mct_t6 *t6, struct usb_interface *intf)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct usb_endpoint_descriptor *ep;
	u8 *buf;
	int ret;

	ret = usb_find_int_in_endpoint(intf->cur_altsetting, &ep);
	if (ret)
		return ret;
	t6->ev_urb = usb_alloc_urb(0, GFP_KERNEL);
	buf = drmm_kmalloc(&t6->dev, T6_EVENT_LEN, GFP_KERNEL);
	if (!t6->ev_urb || !buf)
		return -ENOMEM;
	usb_fill_int_urb(t6->ev_urb, udev, usb_rcvintpipe(udev, ep->bEndpointAddress), buf,
			 T6_EVENT_LEN, t6_event_complete, t6, ep->bInterval);
	return 0;
}

/* ------------------------------------------------------------------ */
/* debugfs							      */

static int t6_stats_show(struct seq_file *m, void *data)
{
	struct drm_debugfs_entry *entry = m->private;
	struct mct_t6 *t6 = to_t6(entry->dev);
	u64 ms = max_t(u64, ktime_ms_delta(ktime_get(), t6->stats.since), 1);
	u64 n = max_t(u64, t6->stats.updates, 1);

	seq_printf(m, "seconds          %llu\n", ms / 1000);
	seq_printf(m, "mode             %dx%d@%d (output %d), %s\n", t6->width, t6->height,
		   t6->refresh, t6->output, t6->lay.planes ? t6_kind_name[t6->lay.kind] : "-");
	seq_printf(m, "updates          %llu (%llu.%llu per second)\n", t6->stats.updates,
		   div64_u64(t6->stats.updates * 1000, ms),
		   div64_u64(t6->stats.updates * 10000, ms) % 10);
	seq_printf(m, "skipped images   %llu\n", t6->stats.skipped);
	seq_printf(m, "pixels damaged   %llu\n", t6->stats.px_damage);
	seq_printf(m, "damage clips     %llu commits without, %llu clips, %llu px in clips\n",
		   t6->stats.no_clips, t6->stats.clips, t6->stats.clip_px);
	seq_printf(m, "rows copied      %llu per commit\n",
		   div64_u64(t6->stats.rows_copied, max_t(u64, t6->stats.copies, 1)));
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
	seq_printf(m, "motion -> NV12   %llu times\n", t6->stats.to_420);
	seq_printf(m, "events           %llu frame done, %llu monitor change, %llu other\n",
		   t6->stats.ev_done, t6->stats.ev_change, t6->stats.ev_other);
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
	/*
	 * Shared buffers are imported without a DMA mapping and read through
	 * the exporter's vmap. With an amdgpu dGPU that is VRAM over PCIe
	 * (~1 GB/s with streaming loads). An attached import would get the
	 * buffer pinned in system memory, but amdgpu's vmap then insists on
	 * pinning it to its preferred VRAM and fails (see the lab notebook).
	 */
	DRM_GEM_SHMEM_DRIVER_OPS,
	DRM_FBDEV_SHMEM_DRIVER_OPS,
};

/*
 * Enable the CRTC before updating the planes: the mode set clears what the
 * driver knows about the device's frame buffers and cursor, so the planes
 * of the same commit must come after it, or the first frame and the cursor
 * after a mode set or resume would be dropped.
 */
static void t6_commit_tail(struct drm_atomic_commit *state)
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

static const struct drm_mode_config_helper_funcs t6_mode_config_helpers = {
	.atomic_commit_tail = t6_commit_tail,
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
	dev->mode_config.helper_private = &t6_mode_config_helpers;

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
	drm_crtc_enable_color_mgmt(&t6->crtc, 0, false, 256);
	ret = drm_mode_crtc_set_gamma_size(&t6->crtc, 256);
	if (ret)
		return ret;

	ret = drm_encoder_init(dev, &t6->encoder, &t6_encoder_funcs, DRM_MODE_ENCODER_TMDS, NULL);
	if (ret)
		return ret;
	t6->encoder.possible_crtcs = drm_crtc_mask(&t6->crtc);

	drm_connector_helper_add(&t6->conn, &t6_conn_helper_funcs);
	ret = drm_connector_init(dev, &t6->conn, &t6_conn_funcs, DRM_MODE_CONNECTOR_HDMIA);
	if (ret)
		return ret;
	/* events for changes, polling as well in case a model doesn't send them */
	t6->conn.polled = DRM_CONNECTOR_POLL_HPD | DRM_CONNECTOR_POLL_CONNECT |
			  DRM_CONNECTOR_POLL_DISCONNECT;
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

	usb_free_urb(t6->ev_urb);
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
	int ret, i;

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
	INIT_WORK(&t6->hpd_work, t6_hpd_work);
	INIT_DELAYED_WORK(&t6->settle_work, t6_settle_work);
	for (i = 0; i < 256; i++) {
		t6->lut[0][i] = i;
		t6->lut[1][i] = i;
		t6->lut[2][i] = i;
	}
	mutex_init(&t6->upd.lock);
	mutex_init(&t6->io_lock);

	t6->ctrl = drmm_kmalloc(dev, T6_CTRL_BUF, GFP_KERNEL);
	t6->cur.img = drmm_kmalloc(dev, T6_CUR_BYTES, GFP_KERNEL);
	t6->cur.shown = drmm_kmalloc(dev, T6_CUR_BYTES, GFP_KERNEL);
	if (!t6->ctrl || !t6->cur.img || !t6->cur.shown)
		return -ENOMEM;
	ret = t6_alloc_urbs(t6, interface);
	if (ret)
		return ret;

	t6_pick_output(t6);
	t6_read_platform(t6);
	ret = t6_read_modes(t6);
	if (ret) {
		drm_err(dev, "could not read the mode table: %d\n", ret);
		return ret;
	}

	px = (size_t)t6->max_w * t6->max_h;
	t6->img = kvmalloc(px * 4, GFP_KERNEL);
	t6->hw[0] = kvmalloc(px * 4, GFP_KERNEL);
	t6->hw[1] = kvmalloc(px * 4, GFP_KERNEL);
	t6->hw[2] = kvmalloc(px * 4, GFP_KERNEL);
	t6->shadow = kvzalloc(px * 4, GFP_KERNEL);
	t6->rowmask = drmm_kzalloc(dev, t6->max_h, GFP_KERNEL);
	ret = drmm_add_action_or_reset(dev, t6_release, NULL);
	if (ret)
		return ret;
	if (!t6->shadow || !t6->rowmask || !t6->img || !t6->hw[0] || !t6->hw[1] || !t6->hw[2])
		return -ENOMEM;

	ret = t6_modeset_init(t6);
	if (ret)
		return ret;
	ret = t6_events_init(t6, interface);
	if (ret)
		return ret;

	t6->stats.since = ktime_get();
	drm_debugfs_add_file(dev, "mct_t6_stats", t6_stats_show, NULL);

	usb_set_intfdata(interface, dev);
	drmm_kms_helper_poll_init(dev);
	ret = drm_dev_register(dev, 0);
	if (ret)
		return ret;
	if (usb_submit_urb(t6->ev_urb, GFP_KERNEL))
		drm_warn(dev, "no monitor change events, polling only\n");

	drm_client_setup(dev, NULL);
	return 0;
}

static void t6_usb_disconnect(struct usb_interface *interface)
{
	struct drm_device *dev = usb_get_intfdata(interface);
	struct mct_t6 *t6 = to_t6(dev);

	usb_kill_urb(t6->ev_urb);
	cancel_work_sync(&t6->hpd_work);
	drm_dev_unplug(dev);
	drm_atomic_helper_shutdown(dev);
}

static int t6_suspend(struct usb_interface *interface, pm_message_t message)
{
	struct drm_device *dev = usb_get_intfdata(interface);
	int ret;

	ret = drm_mode_config_helper_suspend(dev);
	usb_kill_urb(to_t6(dev)->ev_urb);
	cancel_work_sync(&to_t6(dev)->hpd_work);
	return ret;
}

static int t6_resume(struct usb_interface *interface)
{
	struct drm_device *dev = usb_get_intfdata(interface);

	usb_submit_urb(to_t6(dev)->ev_urb, GFP_KERNEL);
	return drm_mode_config_helper_resume(dev);
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
