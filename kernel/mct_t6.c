// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Filip Piękoś <mail@filippiekos.com>
 *
 * DRM driver for MCT Trigger 6 (T6) USB display adapters, e.g. the
 * "T6 USB Station" (USB 0711:5600 / 0711:5601).
 *
 * Protocol (see docs/reference/protocol-t6.md; first worked out by the trigger6
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
 * The worker converts the shadow image to the surface format in use (exact
 * RGB or YUV 4:4:4 while the screen is still, NV12 during large changes),
 * writes the rows that differ from the next of three device frame buffers
 * as rectangle writes (session type 7), and switches to it with an empty
 * flip. Commits never wait for USB and the newest image always wins.
 */

#include <linux/ctype.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/unaligned.h>
#include <linux/usb.h>
#include <linux/vmalloc.h>

#include <drm/drm_color_mgmt.h>
#include <drm/drm_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_fbdev_shmem.h>
#include <drm/drm_file.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>

#include "mct.h"
#include "mct_drv.h"

/* defaults for devices probed later; each device has its own in sysfs */
static char *format = "auto";
module_param(format, charp, 0644);
MODULE_PARM_DESC(format, "Default pixel format for new devices: auto (exact RGB at rest, NV12 during large changes; default), rgb24, rgb32, yuv444, nv12. Per device: /sys/bus/usb/devices/<interface>/mct/format.");

static bool all_modes;
module_param(all_modes, bool, 0644);
MODULE_PARM_DESC(all_modes, "Default for new devices: also offer modes larger than the monitor's native resolution; off by default. Per device: .../mct/all_modes.");

#define DRIVER_NAME	"mct_t6"
#define DRIVER_DESC	"MCT Trigger 6 USB display"
#define DRIVER_MAJOR	0
#define DRIVER_MINOR	28
#define DRIVER_PATCH	0

#define T6_EP_OUT	0x02
#define T6_EVENT_LEN	64		/* EP 0x83 interrupt packets */
/* byte 0x13 of an event: what happened */
#define T6_EV_CHANGE0	0x01		/* monitor change on output 0 */
#define T6_EV_CHANGE1	0x02		/* ... output 1 */
#define T6_EV_DONE	0x04		/* frame done, fence ID at 0x0c */
#define T6_EV_REDRAW	0x10		/* the device lost the picture: send it all */

#define T6_HPD_MS	300		/* monitor status re-read interval after a change */
#define T6_HPD_SAME	3		/* equal readings before a change counts */

#define T6_REQ_EDID	0x80
#define T6_REQ_STATUS	0x87
#define T6_REQ_MODES	0x89
#define T6_REQ_SETMODE	0x12
#define T6_REQ_OUTPUT	0x03
#define T6_REQ_READY	0x31		/* "software ready", after a mode set */
#define T6_REQ_CANCEL	0x14		/* drop a bulk transfer cut off halfway */
/* hardware cursor, per output: position, show/hide, image upload */
#define T6_REQ_CUR_POS(o)	((o) ? 0x06 : 0x04)
#define T6_REQ_CUR_SHOW(o)	((o) ? 0x07 : 0x05)
#define T6_REQ_CUR_IMG(o)	((o) ? 0x11 : 0x10)

/*
 * Cursor images are 64, 96 or 128 pixels square: header + B, G, R, A. The
 * upload offset is 16 bits, so a 128 pixel image is sent up to 0x10000
 * bytes; its last two pixels (bottom right) are never sent.
 */
#define T6_CUR		128
#define T6_CUR_BYTES	(8 + T6_CUR * T6_CUR * 4)
#define T6_CUR_MAX_SEND	0x10000

#define T6_MAX_MODES	30		/* 0x89 table: 512 + 448 bytes */
#define T6_MAX_OUTPUTS	2
#define T6_FRAG		0x19000		/* largest fragment the vendor driver sends */
#define T6_FRAG_HS	0x6400		/* ... on a USB 2 link */
#define T6_HS_MAX_W	2048		/* widest mode on a USB 2 link, as the vendor's */
#define T6_URBS		16		/* bulk transfers in flight at most */
#define T6_CTRL_BUF	4096		/* also the cursor upload chunk size */
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
 * Pixel clock limit, well above what the device's mode table offers; it
 * starts to show artifacts around 350 MHz. Limits are in docs/reference/t6-limits.md.
 */
#define T6_MAX_CLOCK_KHZ	340000
/* frame buffers are 0xfffec0 apart; a frame is a 0x30 byte header + planes */
#define T6_BUF_SIZE		0xfffec0
/*
 * Largest image beyond the device's own table, and a lower clock limit for
 * modes from 2560 pixels wide, where higher clocks were not reliable.
 */
#define T6_MAX_IMG_W		3840
#define T6_MAX_IMG_H		2160
#define T6_WIDE_W		2560
#define T6_WIDE_MAX_CLOCK_KHZ	316000
/* computed extra refresh rates only for sizes up to this */
#define T6_EXTRA_MAX_W		1920
#define T6_EXTRA_MAX_H		1080

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
/* format values in sysfs and the module parameter, by kind */
static const char *const t6_kind_param[] = { "nv12", "yuv444", "rgb24", "rgb32" };
static const char *const t6_kind_sysfs[] = { "NV12", "YUV444", "RGB24", "RGB32" };
#define T6_FMT_AUTO	-1
/*
 * Scanout reads the frame buffer from the device's memory once per refresh,
 * and how much it can read depends on the format. Bytes per second at most:
 */
static const u64 t6_scan_max[] = {
	[T6_NV12] = U64_MAX,
	[T6_YUV444] = 760000000,
	[T6_RGB24] = 500000000,
	[T6_RGB32] = 500000000,
};

/* bytes per pixel the device reads per refresh, times two */
static const unsigned int t6_scan_x2[] = { 3, 6, 6, 8 };

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
	struct mct_device base;		/* DRM side, see mct_drv.h */

	struct t6_mode modes[T6_MAX_MODES];
	int nmodes;
	int output;			/* index the monitor is on */
	int max_w, max_h;
	int platform;			/* 0 "Lite", 1 "Super Lite" */
	int pll_base_khz;		/* 48 MHz ("Lite") or 40 MHz ("Super Lite") */
	u32 boot_code, image_code;	/* firmware versions, 0 if unknown */
	char project[17];		/* board ("project code"), "" if unknown */
	u8 serial[8];
	bool has_serial;
	struct {
		u16 id;
		char name[33];
	} func[2];			/* function descriptors (0xb1 index 0, 3) */
	int level;			/* feature level, -1 if unknown */
	int vram_mb;
	u32 disp_func;			/* DISP descriptor display function flags */
	int mon_max_khz, mon_max_hfreq;	/* monitor limits from its EDID, 0 if none */
	const struct drm_edid *last_edid; /* last EDID read, for failed reads */
	int fmt_req;			/* T6_FMT_AUTO or the fixed kind (sysfs format) */
	bool all_modes;			/* sysfs all_modes */
	int frag;			/* largest bulk write: smaller on USB 2 */
	bool usb2;

	struct mutex io_lock;		/* USB transfers and the ctrl buffer */
	u8 *ctrl;			/* DMA-able control transfer buffer */

	/* bulk transfers are queued without waiting; the endpoint keeps their order */
	struct t6_urb urbs[T6_URBS];
	struct list_head urb_free;
	spinlock_t urb_lock;		/* urb_free */
	wait_queue_head_t urb_wait;	/* for a URB to become free */
	struct usb_anchor anchor;	/* URBs in flight */
	atomic_t urb_err;		/* last error of a completed URB */
	u8 *stale[3];			/* rows where img may differ from frame buffer k */
	u8 *img;			/* the shadow in NV12 */
	u8 lut[3][256];			/* gamma, R G B, applied in software */
	struct t6_layout lay;		/* of img and hw, set by the worker */
	u8 *hw[3];			/* NV12 image each frame buffer holds */
	size_t img_size;		/* of img and hw[], for the current mode */
	bool hw_valid[3];
	unsigned int buf;		/* next frame buffer to write */
	u32 seq;

	ktime_t last_large;		/* last large update */

	struct urb *ev_urb;		/* interrupt IN, always queued */
	struct delayed_work ev_retry;	/* resubmit it after an error */
	atomic_t ev_fails;		/* errors in a row */
	int ev_err;			/* the last one */
	struct delayed_work hpd_work;	/* monitor status, debounced */
	int hpd_last;			/* last reading */
	atomic_t hpd_same;		/* how often in a row; 0 restarts the count */
	int hpd_state;			/* reported status: 1 connected, 0 not, -1 unknown */
	bool hpd_events;		/* the device sends change events */
	struct work_struct redraw_work;
	atomic_t redraw;		/* resend everything with the next update */
	atomic_t inject;		/* debugfs: break the next update on purpose */
	atomic_t no_cancel;		/* ... and skip the cancel request */
	struct mutex mode_lock;		/* mode_entry, enabled */
	struct t6_mode mode_entry;	/* the mode set at the last enable */
	bool enabled;			/* the output is on with mode_entry */

	struct {
		struct mutex lock;	/* cursor state: plane updates and redraws */
		u8 *img;		/* header + image being built */
		u8 *shown;		/* last uploaded */
		int bytes, shown_bytes;	/* of the two images */
		int slot;		/* 0 or 1: which of our two slots is in use */
		int x, y;		/* last position sent */
		bool uploaded, visible;
	} cur;

	struct {			/* debugfs statistics */
		u64 updates, bytes, ns_conv, ns_usb;
		u64 rows, bands;		/* image rows and bands sent */
		u64 to_420;			/* switches to 4:2:0 for motion */
		u64 ev_done, ev_change, ev_redraw, ev_other;	/* interrupt events */
		u64 ev_errors, recoveries, xfer_errors;
	} stats;
};

#define to_t6(m) container_of(m, struct mct_t6, base)

static struct usb_device *t6_udev(struct mct_t6 *t6)
{
	return interface_to_usbdev(to_usb_interface(t6->base.dev.dev));
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
	t6->max_w = max(t6->max_w, T6_MAX_IMG_W);
	t6->max_h = max(t6->max_h, T6_MAX_IMG_H);
	return t6->nmodes ? 0 : -ENODEV;
}

/* device mode entry for a DRM mode; -ERANGE if the T6 can't drive it */
static int t6_mode_entry(struct mct_t6 *t6, const struct drm_display_mode *mode,
			 struct t6_mode *m)
{
	static const u8 out_sel[] = { 3, 1, 2, 0 };
	u64 vco;
	int i;

	if (mode->clock > T6_MAX_CLOCK_KHZ ||
	    mode->flags & (DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLSCAN) ||
	    drm_mode_vrefresh(mode) > 255)
		return -ERANGE;
	if (mode->hdisplay >= T6_WIDE_W && mode->clock > T6_WIDE_MAX_CLOCK_KHZ)
		return -ERANGE;
	/* the NV12 rows are sent unpadded; the vendor driver needs width % 16 too */
	if (mode->hdisplay % 16 || mode->vdisplay % 2 || mode->hdisplay < 64 ||
	    mode->vdisplay < 64)
		return -ERANGE;
	if (t6->usb2 && mode->hdisplay > T6_HS_MAX_W)
		return -ERANGE;
	/* NV12 has to fit a frame buffer; the other formats are used where they fit */
	if (mode->hdisplay > t6->max_w || mode->vdisplay > t6->max_h ||
	    0x30 + 3ull * mode->hdisplay * mode->vdisplay / 2 > T6_BUF_SIZE)
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
	int w = t6->base.width, h = t6->base.height, i;
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
	/* nothing in the frame buffers is in the new layout, convert it all */
	memset(t6->hw_valid, 0, sizeof(t6->hw_valid));
	memset(t6->base.changed, 1, h);
}

/*
 * Shadow (XRGB8888) to t6->img in the current layout, through the gamma
 * tables. YUV is BT.601 full range (JFIF), as the vendor driver converts it:
 * limited range came out greyish next to RGB. NV12 takes the chroma of each
 * 2x2 block. Modes have even sizes. base.lock held.
 */
static inline u8 t6_y(int r, int g, int b)
{
	return (77 * r + 150 * g + 29 * b + 128) >> 8;
}

/* chroma of sums of n pixels (n = 1 or 4), clamped */
static inline u8 t6_u(int r, int g, int b, int shift)
{
	return clamp(((-43 * r - 85 * g + 128 * b + (128 << shift)) >> (8 + shift)) + 128, 0, 255);
}

static inline u8 t6_v(int r, int g, int b, int shift)
{
	return clamp(((128 * r - 107 * g - 21 * b + (128 << shift)) >> (8 + shift)) + 128, 0, 255);
}

/* one row of packed or 4:4:4 output */
static void t6_convert_row(struct mct_t6 *t6, int y)
{
	const u8 *lr = t6->lut[0], *lg = t6->lut[1], *lb = t6->lut[2];
	int w = t6->base.width, x;
	size_t px = (size_t)w * t6->base.height;
	const u8 *s = t6->base.shadow + (size_t)y * w * 4;	/* B, G, R, X */
	u8 *py = t6->img + (size_t)y * w * t6->lay.bpp[0];

	switch (t6->lay.kind) {
	case T6_RGB24:
		for (x = 0; x < w; x++, s += 4, py += 3) {
			py[0] = lb[s[0]];
			py[1] = lg[s[1]];
			py[2] = lr[s[2]];
		}
		break;
	case T6_RGB32:
		for (x = 0; x < w; x++, s += 4, py += 4) {
			py[0] = lb[s[0]];
			py[1] = lg[s[1]];
			py[2] = lr[s[2]];
			py[3] = 0;
		}
		break;
	case T6_YUV444:
		for (x = 0; x < w; x++, s += 4) {
			int r = lr[s[2]], g = lg[s[1]], b = lb[s[0]];

			py[x] = t6_y(r, g, b);
			py[px + x] = t6_u(r, g, b, 0);
			py[2 * px + x] = t6_v(r, g, b, 0);
		}
		break;
	case T6_NV12:
		break;
	}
}

/* one row pair of NV12: two Y rows and the UV row */
static void t6_convert_pair(struct mct_t6 *t6, int y)
{
	const u8 *lr = t6->lut[0], *lg = t6->lut[1], *lb = t6->lut[2];
	int w = t6->base.width, x, i;
	const u8 *s0 = t6->base.shadow + (size_t)y * w * 4;	/* B, G, R, X */
	const u8 *s1 = s0 + w * 4;
	u8 *y0 = t6->img + (size_t)y * w, *y1 = y0 + w;
	u8 *uv = t6->img + (size_t)w * t6->base.height + (size_t)(y / 2) * w;

	for (x = 0; x < w; x += 2, s0 += 8, s1 += 8, y0 += 2, y1 += 2, uv += 2) {
		/* the 2x2 block: top left, top right, bottom left, bottom right */
		const u8 *p4[4] = { s0, s0 + 4, s1, s1 + 4 };
		u8 *out[4] = { y0, y0 + 1, y1, y1 + 1 };
		int r = 0, g = 0, b = 0;

		for (i = 0; i < 4; i++) {
			int pr = lr[p4[i][2]], pg = lg[p4[i][1]], pb = lb[p4[i][0]];

			*out[i] = t6_y(pr, pg, pb);
			r += pr;
			g += pg;
			b += pb;
		}
		/* chroma of the 2x2 block: sums of 4, so shift 2 more */
		uv[0] = t6_u(r, g, b, 2);
		uv[1] = t6_v(r, g, b, 2);
	}
}

/*
 * Convert the rows changed since the last conversion (t6->base.changed) and mark
 * them stale in every frame buffer. base.lock held.
 */
static void t6_convert(struct mct_t6 *t6)
{
	int h = t6->base.height, y, k;

	for (y = 0; y < h; y += 2) {
		if (!t6->base.changed[y] && !t6->base.changed[y + 1])
			continue;
		if (t6->lay.kind == T6_NV12) {
			t6_convert_pair(t6, y);
		} else {
			t6_convert_row(t6, y);
			t6_convert_row(t6, y + 1);
		}
		t6->base.changed[y] = 1;
		t6->base.changed[y + 1] = 1;
	}
	for (k = 0; k < ARRAY_SIZE(t6->stale); k++)
		for (y = 0; y < h; y++)
			t6->stale[k][y] |= t6->base.changed[y];
	memset(t6->base.changed, 0, h);
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
	return t6->lay.host[i] +
	       ((size_t)p * t6->lay.rows[i] + r) * t6->base.width * t6->lay.bpp[i];
}

/*
 * Bands of row pairs where t6->img differs from what frame buffer k holds,
 * each with the columns that changed; returns the count.
 */
static int t6_diff_bands(struct mct_t6 *t6, int k, struct t6_band *bands)
{
	int w = t6->base.width, pairs = t6->base.height / 2, n = 0, p, i, r;

	if (!t6->hw_valid[k]) {
		bands[0] = (struct t6_band){ 0, pairs, 0, w };
		return 1;
	}
	for (p = 0; p < pairs; p++) {
		int lo = w, hi = 0;
		bool d = false;

		if (!t6->stale[k][2 * p] && !t6->stale[k][2 * p + 1])
			continue;

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
	int rb = t6->base.width * bpp, bw = (b->x1 - b->x0) * bpp, xo = b->x0 * bpp;
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
	int step = max(t6->frag / (3 * bw), 1);
	int p, n, y, ys, y1, ret = 0;

	if (t6->lay.planes == 1) {
		/*
		 * Packed RGB: one plane, so each write takes 3n consecutive rows
		 * (2n in the first part, n in the second). The last write may
		 * start earlier to stay on screen; rewriting rows is harmless.
		 */
		y1 = 2 * b->p1;
		for (y = 2 * b->p0; y < y1 && !ret; y += 3 * n) {
			n = min3(step, DIV_ROUND_UP(y1 - y, 3), t6->base.height / 3);
			ys = min(y, t6->base.height - 3 * n);
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
	f.v.type = cpu_to_le32(3 + t6->output);	/* flip output 0 / 1 */
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
 * A bulk transfer failed or got stuck: a session cut off halfway leaves the
 * device waiting for the rest, and later transfers time out. Drop what is
 * queued, clear a stall and send the cancel request, as the vendor drivers
 * do after a failed write. On the 5600 that request restarts the device:
 * it leaves the bus and comes back about 3 s later, and is probed again.
 * io_lock held.
 */
static void t6_recover(struct mct_t6 *t6, int err)
{
	struct usb_device *udev = t6_udev(t6);

	if (udev->state == USB_STATE_NOTATTACHED)
		return;		/* unplugged: nothing to recover */
	t6->stats.xfer_errors++;
	usb_kill_anchored_urbs(&t6->anchor);
	if (err == -EPIPE)
		usb_clear_halt(udev, usb_sndbulkpipe(udev, T6_EP_OUT));
	if (atomic_xchg(&t6->no_cancel, 0))
		drm_info(&t6->base.dev, "test: no cancel request\n");
	else
		t6_ctrl_out(t6, T6_REQ_CANCEL, 0, 0, 0);
	memset(t6->hw_valid, 0, sizeof(t6->hw_valid));
	t6->stats.recoveries++;
	drm_err_ratelimited(&t6->base.dev, "frame transfer error %d, transfer cancelled\n", err);
}

/*
 * Test hook (debugfs mct_t6_inject): announce a 64 KiB rectangle write
 * into frame buffer k, send only 256 bytes of it and fail the update.
 * Writing 1 does that with the usual recovery, 3 without the cancel
 * request, 2 only sends the cancel request.
 */
static int t6_inject_cut(struct mct_t6 *t6, int k)
{
	struct t6_rect_header r = { };
	struct t6_urb *u;
	int ret;

	r.type = cpu_to_le32(7);
	r.len = cpu_to_le32(0x10000);
	r.y_addr = cpu_to_le32(t6_bufs[k] + 0x30);
	r.uv_addr = cpu_to_le32(t6_bufs[k] + 0x30 + 0x8000);
	r.y_pitch = cpu_to_le16(0x100);
	r.uv_pitch = cpu_to_le16(0x100);
	r.width = cpu_to_le16(0x100);
	r.height = cpu_to_le16(0xaa);
	ret = t6_bulk(t6, &r, sizeof(r));
	if (ret)
		return ret;
	u = t6_urb_get(t6);
	if (!u)
		return -ETIMEDOUT;
	memset(u->buf, 0x80, 0x100);
	ret = t6_urb_send(t6, u, 0x100);
	return ret ?: -EIO;
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
	switch (atomic_xchg(&t6->inject, 0)) {
	case 3:
		atomic_set(&t6->no_cancel, 1);
		fallthrough;
	case 1:
		return t6_inject_cut(t6, k);
	case 2:
		drm_info(&t6->base.dev, "test: cancel request only\n");
		t6_ctrl_out(t6, T6_REQ_CANCEL, 0, 0, 0);
		break;
	}
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
	memset(t6->stale[k], 0, t6->base.height);
	t6->stats.bands += n;
	t6->buf = (k + 1) % ARRAY_SIZE(t6_bufs);
	return 0;
}

/* bytes of one frame in device memory */
static u64 t6_frame_bytes(int w, int h, enum t6_kind kind)
{
	switch (kind) {
	case T6_NV12:
		return 3ull * w * h / 2;
	case T6_YUV444:
	case T6_RGB24:
		return 3ull * round_up(w, 32) * h;
	default:
		return 4ull * w * h;
	}
}

/* the format's frame fits a buffer, and its scanout is within the limit */
static bool t6_fits(const struct mct_t6 *t6, enum t6_kind kind)
{
	int w = t6->base.width, h = t6->base.height;

	return 0x30 + t6_frame_bytes(w, h, kind) <= T6_BUF_SIZE &&
	       (u64)t6_scan_x2[kind] * w * h * t6->base.refresh / 2 <= t6_scan_max[kind];
}

/*
 * The format= parameter: the full colour format to use at rest, and whether
 * to drop to NV12 during large changes. auto takes the best that fits the
 * mode: exact RGB, else YUV 4:4:4, else NV12.
 */
/* "auto" or a format name -> T6_FMT_AUTO or the kind; -EINVAL otherwise */
static int t6_parse_format(const char *s)
{
	int i;

	if (sysfs_streq(s, "auto"))
		return T6_FMT_AUTO;
	for (i = 0; i < ARRAY_SIZE(t6_kind_param); i++)
		if (sysfs_streq(s, t6_kind_param[i]))
			return i;
	return -EINVAL;
}

static enum t6_kind t6_requested_format(const struct mct_t6 *t6, bool *adaptive)
{
	int req = READ_ONCE(t6->fmt_req);

	*adaptive = req == T6_FMT_AUTO;
	if (req != T6_FMT_AUTO)
		return req;
	return t6_fits(t6, T6_RGB24) ? T6_RGB24 : t6_fits(t6, T6_YUV444) ? T6_YUV444 : T6_NV12;
}

static bool t6_large(const struct mct_t6 *t6, const struct t6_band *bands, int n)
{
	u64 px = 0;
	int i;

	for (i = 0; i < n; i++)
		px += (u64)(bands[i].p1 - bands[i].p0) * 2 * (bands[i].x1 - bands[i].x0);
	return px * 4 > (u64)t6->base.width * t6->base.height;
}

static void t6_update(struct mct_device *mdev)
{
	struct mct_t6 *t6 = to_t6(mdev);
	struct t6_band bands[T6_MAX_BANDS];
	enum t6_kind full, want;
	bool fits, adaptive, known;
	int n, idx, ret;
	ktime_t t0, t1;

	/*
	 * Take the USB lock first: a commit waits on base.lock, and must not end
	 * up waiting behind a cursor upload or connector poll on io_lock.
	 */
	mutex_lock(&t6->io_lock);
	mutex_lock(&t6->base.lock);
	if (!t6->base.dirty || !t6->base.width) {
		mutex_unlock(&t6->base.lock);
		mutex_unlock(&t6->io_lock);
		return;
	}
	t6->base.dirty = false;
	t0 = ktime_get();

	/* a failed transfer may have left the device mid-session */
	ret = atomic_xchg(&t6->urb_err, 0);
	if (ret && drm_dev_enter(&t6->base.dev, &idx)) {
		t6_recover(t6, ret);
		drm_dev_exit(idx);
	}
	if (atomic_xchg(&t6->redraw, 0))
		memset(t6->hw_valid, 0, sizeof(t6->hw_valid));

	full = t6_requested_format(t6, &adaptive);
	fits = full != T6_NV12 && t6_fits(t6, full);
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
		mct_settle(&t6->base, T6_SETTLE_MS);
	}
	/* commits only wait for the conversion above, never for USB */
	mutex_unlock(&t6->base.lock);
	t1 = ktime_get();

	if (drm_dev_enter(&t6->base.dev, &idx)) {
		ret = t6_present(t6, bands, n);
		if (ret && ret != -ENODEV && ret != -ESHUTDOWN) {	/* not unplugged */
			t6_recover(t6, ret);
			/* the whole image again in a moment, without hammering a dead device */
			mct_settle(&t6->base, 100);
		} else if (!ret) {
			t6->stats.updates++;
		}
		drm_dev_exit(idx);
	}
	t6->stats.ns_conv += ktime_to_ns(ktime_sub(t1, t0));
	t6->stats.ns_usb += ktime_to_ns(ktime_sub(ktime_get(), t1));
	mutex_unlock(&t6->io_lock);
}

/* updates stopped: let the queued transfers finish */
static void t6_stop(struct mct_device *mdev)
{
	struct mct_t6 *t6 = to_t6(mdev);

	if (!usb_wait_anchor_empty_timeout(&t6->anchor, T6_TIMEOUT))
		usb_kill_anchored_urbs(&t6->anchor);
}

/* ------------------------------------------------------------------ */
/* cursor plane							      */

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
 * Image in chunks (wIndex = byte offset) into the other slot, then select
 * that slot; leaves the cursor visible.
 */
static int t6_cursor_upload(struct mct_t6 *t6)
{
	int next = !t6->cur.slot, slot = t6_cursor_slot(t6, next);
	int off, n, ret = 0;

	mutex_lock(&t6->io_lock);
	for (off = 0; off < min(t6->cur.bytes, T6_CUR_MAX_SEND) && ret >= 0; off += n) {
		n = min3(t6->cur.bytes - off, T6_CUR_MAX_SEND - off, T6_CTRL_BUF);
		memcpy(t6->ctrl, t6->cur.img + off, n);
		ret = t6_ctrl_out(t6, T6_REQ_CUR_IMG(t6->output), slot, off, n);
	}
	if (ret >= 0)
		ret = t6_ctrl_out(t6, T6_REQ_CUR_SHOW(t6->output), slot, 1, 0);
	mutex_unlock(&t6->io_lock);
	if (ret < 0)
		return ret;
	memcpy(t6->cur.shown, t6->cur.img, t6->cur.bytes);
	t6->cur.shown_bytes = t6->cur.bytes;
	t6->cur.slot = next;
	t6->cur.uploaded = true;
	t6->cur.visible = true;
	return 0;
}

/* top left of the image; negative values are sent as v - 1, as the vendor does */
static int t6_cursor_move(struct mct_t6 *t6, int x, int y)
{
	int ret;

	mutex_lock(&t6->io_lock);
	ret = t6_ctrl_out(t6, T6_REQ_CUR_POS(t6->output), x < 0 ? (x - 1) & 0xffff : x,
			  y < 0 ? (y - 1) & 0xffff : y, 0);
	mutex_unlock(&t6->io_lock);
	t6->cur.x = x;
	t6->cur.y = y;
	return ret < 0 ? ret : 0;
}

static void t6_cursor_hide(struct mct_device *mdev)
{
	struct mct_t6 *t6 = to_t6(mdev);

	mutex_lock(&t6->cur.lock);
	t6_cursor_show(t6, false);
	mutex_unlock(&t6->cur.lock);
}

static void t6_cursor_set(struct mct_device *mdev, const u8 *bgra, int x, int y)
{
	struct mct_t6 *t6 = to_t6(mdev);
	int size = 64, i, j;
	u8 *dst;

	/* the smallest image (64, 96, 128) that holds everything visible */
	for (j = 0; j < T6_CUR; j++)
		for (i = 0; i < T6_CUR; i++)
			if (bgra[(j * T6_CUR + i) * 4 + 3] && max(i, j) >= size)
				size = max(i, j) < 96 ? 96 : 128;

	mutex_lock(&t6->cur.lock);
	if (x <= -size || y <= -size) {
		t6_cursor_show(t6, false);
		goto out;
	}
	/* header {kind 1 = ARGB with alpha, w, h, pitch}, then B, G, R, A pixels */
	put_unaligned_le16(1, t6->cur.img);
	put_unaligned_le16(size, t6->cur.img + 2);
	put_unaligned_le16(size, t6->cur.img + 4);
	put_unaligned_le16(size * 4, t6->cur.img + 6);
	dst = t6->cur.img + 8;
	for (j = 0; j < size; j++, dst += size * 4)
		memcpy(dst, bgra + j * T6_CUR * 4, size * 4);
	t6->cur.bytes = 8 + size * size * 4;

	t6_cursor_move(t6, x, y);
	if (!t6->cur.uploaded || t6->cur.bytes != t6->cur.shown_bytes ||
	    memcmp(t6->cur.img, t6->cur.shown, t6->cur.bytes))
		if (t6_cursor_upload(t6))
			goto out;
	t6_cursor_show(t6, true);
out:
	mutex_unlock(&t6->cur.lock);
}

/* ------------------------------------------------------------------ */
/* display on/off, gamma, monitor				      */

/*
 * Feature level 6 devices have no gamma tables (the request stalls), so the
 * DRM gamma LUT is applied during the conversion. A change reconverts the
 * whole image; the diff then sends what changed.
 */
static void t6_gamma(struct mct_device *mdev, const struct drm_color_lut *lut)
{
	struct mct_t6 *t6 = to_t6(mdev);
	bool kick;
	int i;

	mutex_lock(&mdev->lock);
	for (i = 0; i < 256; i++) {
		t6->lut[0][i] = lut ? drm_color_lut_extract(lut[i].red, 8) : i;
		t6->lut[1][i] = lut ? drm_color_lut_extract(lut[i].green, 8) : i;
		t6->lut[2][i] = lut ? drm_color_lut_extract(lut[i].blue, 8) : i;
	}
	kick = mdev->width && mdev->shadow_valid;
	if (kick) {
		memset(mdev->changed, 1, mdev->height);
		mdev->dirty = true;
	}
	mutex_unlock(&mdev->lock);
	if (kick)
		mct_kick(mdev);
}

/*
 * img and hw[] for a w x h mode: up to 4 bytes per pixel in any layout.
 * Called with updates stopped; kept while the size stays the same.
 */
static int t6_alloc_images(struct mct_t6 *t6, int w, int h)
{
	size_t size = (size_t)w * h * 4;
	int i;

	if (size == t6->img_size)
		return 0;
	kvfree(t6->img);
	t6->img = kvmalloc(size, GFP_KERNEL);
	for (i = 0; i < ARRAY_SIZE(t6->hw); i++) {
		kvfree(t6->hw[i]);
		t6->hw[i] = kvmalloc(size, GFP_KERNEL);
	}
	t6->img_size = size;
	if (t6->img && t6->hw[0] && t6->hw[1] && t6->hw[2])
		return 0;
	t6->img_size = 0;	/* try again next time */
	return -ENOMEM;
}

static int t6_enable(struct mct_device *mdev, const struct drm_display_mode *mode)
{
	struct mct_t6 *t6 = to_t6(mdev);
	struct t6_mode m;
	int idx, ret;

	ret = t6_mode_entry(t6, mode, &m);
	if (ret)
		return ret;
	ret = t6_alloc_images(t6, mode->hdisplay, mode->vdisplay);
	if (ret)
		return ret;
	mutex_lock(&t6->mode_lock);
	t6->mode_entry = m;
	msleep(50);	/* after output off, before the new mode, as the vendor driver does */
	if (drm_dev_enter(&mdev->dev, &idx)) {
		if (t6_set_mode(t6, &m) || t6_ready(t6) || t6_set_output(t6, true))
			drm_err(&mdev->dev, "could not set mode %dx%d\n",
				mode->hdisplay, mode->vdisplay);
		drm_dev_exit(idx);
	}
	t6->enabled = true;
	mutex_unlock(&t6->mode_lock);
	mutex_lock(&t6->cur.lock);
	t6->cur.uploaded = false;	/* upload the image again after a mode set */
	t6->cur.visible = false;
	mutex_unlock(&t6->cur.lock);
	mutex_lock(&mdev->lock);
	/* nothing is known about the frame buffers after a mode set */
	memset(t6->hw_valid, 0, sizeof(t6->hw_valid));
	t6->lay.planes = 0;		/* the worker sets it up for the new size */
	t6->buf = 0;
	mutex_unlock(&mdev->lock);
	return 0;
}

static void t6_disable(struct mct_device *mdev)
{
	struct mct_t6 *t6 = to_t6(mdev);

	mutex_lock(&t6->cur.lock);
	t6_cursor_show(t6, false);
	mutex_unlock(&t6->cur.lock);
	mutex_lock(&t6->mode_lock);
	t6->enabled = false;
	t6_set_output(t6, false);
	mutex_unlock(&t6->mode_lock);
}

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
 * Reduced blanking (80 pixels horizontal, at least 460 us vertical): low
 * pixel clocks for high refresh rates.
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
 * A mode of this size and rate the T6 and the monitor can take: standard
 * reduced blanking (CVT-RB, 160 px horizontal blanking) if it fits, else
 * the tighter one above. NULL if neither does.
 */
static struct drm_display_mode *t6_timing(struct mct_t6 *t6, struct drm_device *dev,
					  int w, int h, int hz)
{
	struct drm_display_mode *mode;
	struct t6_mode m;
	int i;

	for (i = 0; i < 2; i++) {
		mode = i ? t6_rb_mode(dev, w, h, hz) :
			   drm_cvt_mode(dev, w, h, hz, true, false, false);
		if (!mode)
			continue;
		/* CVT rounds the clock to 0.25 MHz; the T6's PLL doesn't need that */
		mode->clock = DIV_ROUND_CLOSEST_ULL((u64)mode->htotal * mode->vtotal * hz, 1000);
		if (!t6_mode_entry(t6, mode, &m) &&
		    (!t6->mon_max_khz || mode->clock <= t6->mon_max_khz) &&
		    (!t6->mon_max_hfreq || mode->clock <= t6->mon_max_hfreq * mode->htotal)) {
			mode->type = DRM_MODE_TYPE_DRIVER;
			return mode;
		}
		drm_mode_destroy(dev, mode);
	}
	return NULL;
}

/*
 * The monitor's limits from the EDID range descriptor: highest pixel clock
 * and line rate. Like the vendor's macOS driver, raise them to the EDID's
 * own modes when those exceed them (a wrong descriptor).
 */
static void t6_monitor_limits(struct mct_t6 *t6, struct drm_connector *connector,
			      const struct drm_edid *drm_edid)
{
	const u8 *raw = drm_edid ? (const u8 *)drm_edid_raw(drm_edid) : NULL;
	struct drm_display_mode *mode;
	int i;

	t6->mon_max_khz = 0;
	t6->mon_max_hfreq = 0;
	for (i = 0; raw && i < 4; i++) {
		const u8 *d = raw + 54 + 18 * i;

		if (d[0] || d[1] || d[2] || d[3] != 0xfd)
			continue;
		t6->mon_max_hfreq = d[8] + (d[4] & 0x08 ? 255 : 0);
		t6->mon_max_khz = d[9] * 10000;
	}
	if (!t6->mon_max_khz)
		return;
	list_for_each_entry(mode, &connector->probed_modes, head) {
		t6->mon_max_khz = max(t6->mon_max_khz, mode->clock);
		t6->mon_max_hfreq = max(t6->mon_max_hfreq,
					DIV_ROUND_UP(mode->clock, mode->htotal));
	}
}

/*
 * Whether the EDID already brings a usable mode of this size and rate. One
 * the T6 can't drive doesn't count (a monitor's own timing may need a
 * higher pixel clock than the reduced blanking one here).
 */
static bool t6_has_mode(struct drm_connector *connector, int w, int h, int hz)
{
	struct mct_t6 *t6 = to_t6(to_mct(connector->dev));
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
	struct mct_t6 *t6 = to_t6(to_mct(connector->dev));
	int min_hz = range->min_vfreq, max_hz = range->max_vfreq;
	struct drm_display_mode *mode;
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
		mode = t6_timing(t6, connector->dev, w, h, hz);
		if (!mode)
			continue;
		drm_mode_probed_add(connector, mode);
		count++;
	}
	return count;
}

/*
 * Nothing usable at the monitor's own size: offer 2560x1440@60 to 16:9
 * panels as preferred, so the largest mode left (often 4:3) doesn't get
 * picked.
 */
static int t6_add_smaller_preferred(struct mct_t6 *t6, struct drm_connector *connector,
				    const struct drm_display_mode *pref, bool have_pref)
{
	struct drm_display_mode *mode;

	if (have_pref || pref->hdisplay * 9 != pref->vdisplay * 16 || pref->hdisplay <= 2560)
		return 0;
	mode = t6_timing(t6, connector->dev, 2560, 1440, 60);
	if (!mode)
		return 0;
	mode->type |= DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);
	return 1;
}

/*
 * When the T6 can't drive the monitor's preferred mode, offer instead: the
 * same size with reduced blanking at the highest refresh up to the
 * monitor's that the T6 drives (preferred if it is the monitor's own rate),
 * and the monitor's timing at the highest rate where YUV 4:4:4 still fits
 * the scanout, if the monitor's range allows it.
 */
static int t6_add_preferred_fallbacks(struct mct_t6 *t6, struct drm_connector *connector)
{
	int min_hz = connector->display_info.monitor_range.min_vfreq;
	struct drm_display_mode *pref = NULL, *mode;
	bool have_pref = false;
	struct t6_mode m;
	int i, hz, rb_hz, full_hz, count = 0;

	list_for_each_entry(mode, &connector->probed_modes, head) {
		if (mode->type & DRM_MODE_TYPE_PREFERRED) {
			pref = mode;
			break;
		}
	}
	if (!pref || !t6_mode_entry(t6, pref, &m) || pref->flags & DRM_MODE_FLAG_INTERLACE)
		return 0;
	hz = drm_mode_vrefresh(pref);

	full_hz = div_u64(t6_scan_max[T6_YUV444] * 2,
			  (u64)t6_scan_x2[T6_YUV444] * pref->hdisplay * pref->vdisplay);
	/* a 4:4:4 frame has to fit a frame buffer too (not at 4K) */
	if (0x30 + t6_frame_bytes(pref->hdisplay, pref->vdisplay, T6_YUV444) > T6_BUF_SIZE)
		full_hz = 0;

	/* the monitor's rate, else the highest common one below it */
	for (i = ARRAY_SIZE(t6_extra_hz); i >= -1; i--) {
		rb_hz = i == ARRAY_SIZE(t6_extra_hz) ? hz : i >= 0 ? t6_extra_hz[i] : 60;
		/* not worth it within a few Hz of the full colour mode */
		if (rb_hz > hz || rb_hz <= full_hz + 4 || rb_hz < min_hz)
			continue;
		if (t6_has_mode(connector, pref->hdisplay, pref->vdisplay, rb_hz))
			break;
		mode = t6_timing(t6, connector->dev, pref->hdisplay, pref->vdisplay, rb_hz);
		if (!mode)
			continue;
		if (rb_hz == hz) {
			mode->type |= DRM_MODE_TYPE_PREFERRED;
			have_pref = true;
		}
		drm_mode_probed_add(connector, mode);
		count++;
		break;
	}

	full_hz = min(full_hz, hz - 1);
	/* as the default, an existing mode up to 10% slower beats an odd rate */
	if (full_hz >= max(min_hz, 24)) {
		struct drm_display_mode *best = NULL;
		struct t6_mode m2;

		list_for_each_entry(mode, &connector->probed_modes, head) {
			int r = drm_mode_vrefresh(mode);

			if (mode->hdisplay != pref->hdisplay || mode->vdisplay != pref->vdisplay ||
			    r > full_hz || r * 10 < full_hz * 9 || t6_mode_entry(t6, mode, &m2))
				continue;
			if (!best || r > drm_mode_vrefresh(best))
				best = mode;
		}
		/* the exact maximum stays available next to it */
		if (best && !have_pref) {
			best->type |= DRM_MODE_TYPE_PREFERRED;
			have_pref = true;
		}
		if (best && drm_mode_vrefresh(best) == full_hz)
			return count;
	}
	mode = full_hz >= max(min_hz, 24) ? drm_mode_duplicate(connector->dev, pref) : NULL;
	if (mode) {
		mode->clock = DIV_ROUND_CLOSEST_ULL((u64)mode->htotal * mode->vtotal * full_hz,
						    1000);
		mode->type = DRM_MODE_TYPE_DRIVER | (have_pref ? 0 : DRM_MODE_TYPE_PREFERRED);
		drm_mode_set_name(mode);
		if (t6_mode_entry(t6, mode, &m)) {
			drm_mode_destroy(connector->dev, mode);
		} else {
			drm_mode_probed_add(connector, mode);
			have_pref = true;
			count++;
		}
	}
	return count + t6_add_smaller_preferred(t6, connector, pref, have_pref);
}

/*
 * No usable EDID (some KVMs and switches): the device's own table up to
 * 1920x1080, 1080p60 preferred, as the vendor drivers fall back to,
 * instead of the 1024x768 DRM would offer.
 */
static int t6_add_table_modes(struct mct_t6 *t6, struct drm_connector *connector)
{
	struct drm_display_mode *mode;
	struct t6_mode m;
	int i, count = 0;

	for (i = 0; i < t6->nmodes; i++) {
		const struct t6_mode *e = &t6->modes[i];
		int w = le16_to_cpu(e->hdisplay), h = le16_to_cpu(e->vdisplay);

		if (!w || !h || w > 1920 || h > 1080)
			continue;
		mode = drm_mode_create(connector->dev);
		if (!mode)
			break;
		mode->clock = le32_to_cpu(e->clock_khz);
		mode->hdisplay = w;
		mode->hsync_start = le16_to_cpu(e->hsync_start);
		mode->hsync_end = mode->hsync_start + le16_to_cpu(e->hsync_len);
		mode->htotal = le16_to_cpu(e->htotal);
		mode->vdisplay = h;
		mode->vsync_start = le16_to_cpu(e->vsync_start);
		mode->vsync_end = mode->vsync_start + le16_to_cpu(e->vsync_len);
		mode->vtotal = le16_to_cpu(e->vtotal);
		mode->flags = (e->hsync_pol ? DRM_MODE_FLAG_PHSYNC : DRM_MODE_FLAG_NHSYNC) |
			      (e->vsync_pol ? DRM_MODE_FLAG_PVSYNC : DRM_MODE_FLAG_NVSYNC);
		mode->type = DRM_MODE_TYPE_DRIVER;
		drm_mode_set_name(mode);
		if (t6_mode_entry(t6, mode, &m) || drm_mode_validate_driver(connector->dev, mode)) {
			drm_mode_destroy(connector->dev, mode);
			continue;
		}
		if (w == 1920 && h == 1080 && drm_mode_vrefresh(mode) == 60)
			mode->type |= DRM_MODE_TYPE_PREFERRED;
		drm_mode_probed_add(connector, mode);
		count++;
	}
	return count;
}

/*
 * Monitors often list larger modes than their panel (4K on a 1080p
 * monitor) and scale them down; over the T6 those only cost bandwidth.
 * Drop modes larger than the native size (the EDID's preferred mode)
 * unless all_modes is set.
 */
static int t6_drop_above_native(struct mct_t6 *t6, struct drm_connector *connector,
				int w, int h)
{
	struct drm_display_mode *mode, *tmp;
	int n = 0;

	if (READ_ONCE(t6->all_modes) || !w || !h)
		return 0;
	list_for_each_entry_safe(mode, tmp, &connector->probed_modes, head) {
		if (mode->hdisplay <= w && mode->vdisplay <= h)
			continue;
		list_del(&mode->head);
		drm_mode_destroy(connector->dev, mode);
		n++;
	}
	return n;
}

/*
 * Nothing the T6 drives is marked preferred (a monitor too wide for a USB 2
 * link, odd panel sizes): prefer the largest mode with the monitor's aspect
 * ratio, at the refresh rate closest to 60 Hz, so userspace doesn't take
 * an arbitrary one.
 */
static void t6_ensure_preferred(struct mct_t6 *t6, struct drm_connector *connector,
				int w, int h)
{
	struct drm_display_mode *mode, *best = NULL;
	struct t6_mode m;

	list_for_each_entry(mode, &connector->probed_modes, head)
		if (mode->type & DRM_MODE_TYPE_PREFERRED && !t6_mode_entry(t6, mode, &m))
			return;
	list_for_each_entry(mode, &connector->probed_modes, head) {
		u64 area = (u64)mode->hdisplay * mode->vdisplay;
		bool aspect = !w || abs(mode->hdisplay * h - w * mode->vdisplay) * 100 <=
			      3 * w * mode->vdisplay;

		if (t6_mode_entry(t6, mode, &m) || !aspect)
			continue;
		if (best) {
			u64 best_area = (u64)best->hdisplay * best->vdisplay;

			if (area < best_area)
				continue;
			if (area == best_area && abs(drm_mode_vrefresh(mode) - 60) >=
						 abs(drm_mode_vrefresh(best) - 60))
				continue;
		}
		best = mode;
	}
	if (best)
		best->type |= DRM_MODE_TYPE_PREFERRED;
	else if (w)
		t6_ensure_preferred(t6, connector, 0, 0);	/* any aspect ratio */
}

static int t6_get_modes(struct mct_device *mdev, struct drm_connector *connector)
{
	struct mct_t6 *t6 = to_t6(mdev);
	struct drm_display_mode *mode;
	const struct drm_edid *edid;
	int count, w = 0, h = 0, idx, i;
	bool has_720p = false;

	/* some monitors answer DDC late after waking up: try a few times */
	edid = NULL;
	for (i = 0; i < 5 && !edid; i++) {
		if (i)
			msleep(200);
		if (!drm_dev_enter(connector->dev, &idx))
			break;
		edid = drm_edid_read_custom(connector, t6_read_edid_block, t6);
		drm_dev_exit(idx);
	}
	/*
	 * A monitor that stops answering DDC (asleep, showing another input)
	 * is most likely still the same monitor: keep its modes.
	 */
	if (edid) {
		drm_edid_free(t6->last_edid);
		t6->last_edid = drm_edid_dup(edid);
	} else if (t6->last_edid) {
		drm_warn(connector->dev, "could not read the monitor's EDID, using the last one read\n");
		edid = drm_edid_dup(t6->last_edid);
	} else {
		drm_warn(connector->dev, "could not read the monitor's EDID, offering the adapter's modes\n");
	}
	drm_edid_connector_update(connector, edid);
	count = drm_edid_connector_add_modes(connector);
	t6_monitor_limits(t6, connector, edid);
	drm_edid_free(edid);
	if (!count)
		return t6_add_table_modes(t6, connector);

	list_for_each_entry(mode, &connector->probed_modes, head) {
		if (mode->type & DRM_MODE_TYPE_PREFERRED && !w) {
			w = mode->hdisplay;
			h = mode->vdisplay;
		}
		if (mode->hdisplay == 1280 && mode->vdisplay == 720)
			has_720p = true;
	}
	if (w && w <= T6_EXTRA_MAX_W && h <= T6_EXTRA_MAX_H)
		count += t6_add_extra_modes(connector, w, h);
	if (has_720p && (w != 1280 || h != 720))
		count += t6_add_extra_modes(connector, 1280, 720);
	count += t6_add_preferred_fallbacks(t6, connector);
	count -= t6_drop_above_native(t6, connector, w, h);
	t6_ensure_preferred(t6, connector, w, h);
	return count;
}

static enum drm_mode_status t6_mode_valid(struct mct_device *mdev,
					  const struct drm_display_mode *mode)
{
	struct t6_mode m;

	return t6_mode_entry(to_t6(mdev), mode, &m) ? MODE_CLOCK_RANGE : MODE_OK;
}

static int t6_detect(struct mct_device *mdev)
{
	struct mct_t6 *t6 = to_t6(mdev);
	int ret = t6->hpd_state;

	/* with change events, the debounced state; else ask (polling) */
	if (!t6->hpd_events || ret < 0)
		ret = t6_output_status(t6, t6->output);
	if (ret < 0)
		return connector_status_unknown;
	if (t6->hpd_events)
		t6->hpd_state = ret;
	return ret == 1 ? connector_status_connected : connector_status_disconnected;
}

/* ------------------------------------------------------------------ */
/* interrupt events						      */

/*
 * After a monitor change event, read the status until it reads the same
 * T6_HPD_SAME times in a row (monitors toggle it while they switch modes
 * or wake up), then report. A monitor that is (back) on gets the mode and
 * the whole picture again, and userspace re-probes in any case, so a swap
 * to another monitor is seen too.
 */
static void t6_hpd_work(struct work_struct *work)
{
	struct mct_t6 *t6 = container_of(to_delayed_work(work), struct mct_t6, hpd_work);
	int idx, st, prev;

	if (!drm_dev_enter(&t6->base.dev, &idx))
		return;
	st = t6_output_status(t6, t6->output);
	if (st >= 0 && st == t6->hpd_last && atomic_read(&t6->hpd_same)) {
		atomic_inc(&t6->hpd_same);
	} else {
		t6->hpd_last = st;
		atomic_set(&t6->hpd_same, 1);
	}
	if (st < 0 || atomic_read(&t6->hpd_same) < T6_HPD_SAME) {
		schedule_delayed_work(&t6->hpd_work, msecs_to_jiffies(T6_HPD_MS));
		goto out;
	}
	prev = t6->hpd_state;
	t6->hpd_state = st;
	mutex_lock(&t6->mode_lock);
	/* a monitor (back) on: mode and picture again; not on mere HPD blips */
	if (st == 1 && prev != 1 && t6->enabled) {
		mutex_lock(&t6->io_lock);
		memcpy(t6->ctrl, &t6->mode_entry, sizeof(t6->mode_entry));
		if (t6_ctrl_out(t6, T6_REQ_SETMODE, t6->output, 0, sizeof(t6->mode_entry)) >= 0)
			t6_ctrl_out(t6, T6_REQ_READY, 0, 0, 0);
		t6_ctrl_out(t6, T6_REQ_OUTPUT, t6->output, 1, 0);
		mutex_unlock(&t6->io_lock);
		schedule_work(&t6->redraw_work);
	}
	mutex_unlock(&t6->mode_lock);
	drm_kms_helper_hotplug_event(&t6->base.dev);
out:
	drm_dev_exit(idx);
}

/* resend the picture and the cursor: the device asked, or got a new mode */
static void t6_redraw_work(struct work_struct *work)
{
	struct mct_t6 *t6 = container_of(work, struct mct_t6, redraw_work);
	int idx;

	if (!drm_dev_enter(&t6->base.dev, &idx))
		return;
	atomic_set(&t6->redraw, 1);
	mutex_lock(&t6->base.lock);
	t6->base.dirty = true;
	mutex_unlock(&t6->base.lock);
	mct_kick(&t6->base);
	mutex_lock(&t6->cur.lock);
	/* the slots may be lost too: upload again, now or when it shows next */
	t6->cur.uploaded = false;
	if (t6->cur.visible && !t6_cursor_upload(t6))
		t6_cursor_move(t6, t6->cur.x, t6->cur.y);
	mutex_unlock(&t6->cur.lock);
	drm_dev_exit(idx);
}

/* events stopped for good: poll the monitor status instead */
static void t6_ev_give_up(struct mct_t6 *t6)
{
	drm_warn(&t6->base.dev, "monitor change events stopped (error %d), polling\n",
		 t6->ev_err);
	t6->hpd_events = false;
	t6->hpd_state = -1;
}

/*
 * After an error on the event endpoint: reset it (clears a stall and the
 * data toggle, like the vendor's pipe reset) and queue the transfer again.
 */
static void t6_ev_retry(struct work_struct *work)
{
	struct mct_t6 *t6 = container_of(to_delayed_work(work), struct mct_t6, ev_retry);
	int idx, ret;

	if (!drm_dev_enter(&t6->base.dev, &idx))
		return;
	usb_clear_halt(t6_udev(t6), t6->ev_urb->pipe);
	if (atomic_read(&t6->ev_fails) >= 10) {
		t6_ev_give_up(t6);
	} else {
		ret = usb_submit_urb(t6->ev_urb, GFP_KERNEL);
		/* -EPERM: stopped on purpose (suspend, unplug), not a failure */
		if (ret && ret != -EPERM)
			t6_ev_give_up(t6);
	}
	drm_dev_exit(idx);
}

static void t6_event_complete(struct urb *urb)
{
	struct mct_t6 *t6 = urb->context;
	const u8 *ev = urb->transfer_buffer;
	int fails;

	switch (urb->status) {
	case 0:
		atomic_set(&t6->ev_fails, 0);
		break;
	case -ENOENT:
	case -ECONNRESET:
	case -ESHUTDOWN:
	case -ENODEV:
		return;		/* killed or gone */
	default:
		/* stall or line errors: try again later, slower each time */
		t6->stats.ev_errors++;
		t6->ev_err = urb->status;
		fails = atomic_inc_return(&t6->ev_fails) - 1;
		schedule_delayed_work(&t6->ev_retry, msecs_to_jiffies(100 << min(fails, 5)));
		return;
	}
	if (urb->actual_length > 0x13) {
		if (ev[0x13] & T6_EV_DONE)
			t6->stats.ev_done++;
		if (ev[0x13] & (T6_EV_CHANGE0 | T6_EV_CHANGE1)) {
			t6->stats.ev_change++;
			atomic_set(&t6->hpd_same, 0);
			mod_delayed_work(system_percpu_wq, &t6->hpd_work,
					 msecs_to_jiffies(T6_HPD_MS));
		}
		if (ev[0x13] & T6_EV_REDRAW) {
			t6->stats.ev_redraw++;
			schedule_work(&t6->redraw_work);
		}
		if (!(ev[0x13] & (T6_EV_DONE | T6_EV_CHANGE0 | T6_EV_CHANGE1 | T6_EV_REDRAW)))
			t6->stats.ev_other++;
	}
	if (usb_submit_urb(urb, GFP_ATOMIC)) {
		t6->ev_err = -EIO;
		schedule_delayed_work(&t6->ev_retry, msecs_to_jiffies(100));
	}
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
	buf = drmm_kmalloc(&t6->base.dev, T6_EVENT_LEN, GFP_KERNEL);
	if (!t6->ev_urb || !buf)
		return -ENOMEM;
	usb_fill_int_urb(t6->ev_urb, udev, usb_rcvintpipe(udev, ep->bEndpointAddress), buf,
			 T6_EVENT_LEN, t6_event_complete, t6, ep->bInterval);
	return 0;
}

/* ------------------------------------------------------------------ */
/* debugfs							      */

static void t6_stats(struct mct_device *mdev, struct seq_file *m)
{
	struct mct_t6 *t6 = to_t6(mdev);
	u64 ms = max_t(u64, ktime_ms_delta(ktime_get(), mdev->stats.since), 1);
	u64 n = max_t(u64, t6->stats.updates, 1);

	seq_printf(m, "mode             %dx%d@%d (output %d), %s\n", mdev->width, mdev->height,
		   mdev->refresh, t6->output, t6->lay.planes ? t6_kind_name[t6->lay.kind] : "-");
	seq_printf(m, "device           feature level %d, %d MiB, display flags %#x, USB %s\n",
		   t6->level, t6->vram_mb, t6->disp_func, t6->usb2 ? "2" : "3");
	seq_printf(m, "updates          %llu (%llu.%llu per second)\n", t6->stats.updates,
		   div64_u64(t6->stats.updates * 1000, ms),
		   div64_u64(t6->stats.updates * 10000, ms) % 10);
	seq_printf(m, "rows sent        %llu in %llu bands (%llu per update)\n",
		   t6->stats.rows, t6->stats.bands, div64_u64(t6->stats.rows, n));
	seq_printf(m, "usb bytes        %llu (avg %llu KB/s)\n",
		   t6->stats.bytes, div64_u64(t6->stats.bytes, ms));
	seq_printf(m, "ms per update    convert %llu.%02llu, usb %llu.%02llu\n",
		   div64_u64(t6->stats.ns_conv, n * 1000000),
		   div64_u64(t6->stats.ns_conv, n * 10000) % 100,
		   div64_u64(t6->stats.ns_usb, n * 1000000),
		   div64_u64(t6->stats.ns_usb, n * 10000) % 100);
	seq_printf(m, "motion -> NV12   %llu times\n", t6->stats.to_420);
	seq_printf(m, "events           %llu frame done, %llu monitor change, %llu redraw, %llu other, %llu errors (last %d)%s\n",
		   t6->stats.ev_done, t6->stats.ev_change, t6->stats.ev_redraw,
		   t6->stats.ev_other, t6->stats.ev_errors, t6->ev_err,
		   t6->hpd_events ? "" : ", polling");
	seq_printf(m, "recoveries       %llu\n", t6->stats.recoveries);
	seq_printf(m, "usb MB/s while busy %llu\n",
		   div64_u64(t6->stats.bytes * 1000, max_t(u64, t6->stats.ns_usb, 1)));
}

/* ------------------------------------------------------------------ */
/* sysfs (docs/driver/companion-app.md)				      */

/* NULL once the interface is unbound, see mct_from_dev() */
static struct mct_t6 *t6_from_dev(struct device *dev)
{
	struct mct_device *mdev = mct_from_dev(dev);

	return mdev ? to_t6(mdev) : NULL;
}

#define T6_SHOW_U64(_name, _expr)						\
static ssize_t _name##_show(struct device *dev, struct device_attribute *attr, char *buf) \
{										\
	struct mct_t6 *t6 = t6_from_dev(dev);					\
										\
	if (!t6)								\
		return -ENODEV;							\
	return sysfs_emit(buf, "%llu\n", (u64)(_expr));			\
}										\
static DEVICE_ATTR_RO(_name)

T6_SHOW_U64(updates, t6->stats.updates);
T6_SHOW_U64(bytes, t6->stats.bytes);
T6_SHOW_U64(errors, t6->stats.xfer_errors);
T6_SHOW_U64(recoveries, t6->stats.recoveries);
T6_SHOW_U64(vram_mb, t6->vram_mb);

static ssize_t feature_level_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mct_t6 *t6 = t6_from_dev(dev);

	return t6 ? sysfs_emit(buf, "%d\n", t6->level) : -ENODEV;
}
static DEVICE_ATTR_RO(feature_level);

static ssize_t usb_speed_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mct_t6 *t6 = t6_from_dev(dev);

	return t6 ? sysfs_emit(buf, "%s\n", t6->usb2 ? "usb2" : "usb3") : -ENODEV;
}
static DEVICE_ATTR_RO(usb_speed);

static ssize_t platform_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mct_t6 *t6 = t6_from_dev(dev);

	return t6 ? sysfs_emit(buf, "%s\n", t6->platform ? "super-lite" : "lite") : -ENODEV;
}
static DEVICE_ATTR_RO(platform);

static ssize_t board_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mct_t6 *t6 = t6_from_dev(dev);

	return t6 ? sysfs_emit(buf, "%s\n", t6->project[0] ? t6->project : "-") : -ENODEV;
}
static DEVICE_ATTR_RO(board);

static ssize_t firmware_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mct_t6 *t6 = t6_from_dev(dev);

	return t6 ? sysfs_emit(buf, "%08x %08x\n", t6->boot_code, t6->image_code) : -ENODEV;
}
static DEVICE_ATTR_RO(firmware);

static ssize_t serial_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mct_t6 *t6 = t6_from_dev(dev);

	if (!t6)
		return -ENODEV;
	return t6->has_serial ? sysfs_emit(buf, "%8phN\n", t6->serial) : sysfs_emit(buf, "-\n");
}
static DEVICE_ATTR_RO(serial);

static ssize_t display_flags_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mct_t6 *t6 = t6_from_dev(dev);

	return t6 ? sysfs_emit(buf, "%#x\n", t6->disp_func) : -ENODEV;
}
static DEVICE_ATTR_RO(display_flags);

/* the device's functions, one per line: id (hex) and name */
static ssize_t functions_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mct_t6 *t6 = t6_from_dev(dev);
	int len = 0, i;

	if (!t6)
		return -ENODEV;
	for (i = 0; i < ARRAY_SIZE(t6->func); i++)
		if (t6->func[i].id)
			len += sysfs_emit_at(buf, len, "%04x %s\n", t6->func[i].id,
					     t6->func[i].name);
	return len;
}
static DEVICE_ATTR_RO(functions);

/* the device's own mode table, one mode per line: WxH@Hz and pixel clock in kHz */
static ssize_t device_modes_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mct_t6 *t6 = t6_from_dev(dev);
	int len = 0, i;

	if (!t6)
		return -ENODEV;
	for (i = 0; i < t6->nmodes; i++) {
		const struct t6_mode *m = &t6->modes[i];

		if (!m->hdisplay || !m->vdisplay)
			continue;
		len += sysfs_emit_at(buf, len, "%ux%u@%u %u\n", le16_to_cpu(m->hdisplay),
				     le16_to_cpu(m->vdisplay), m->refresh,
				     le32_to_cpu(m->clock_khz));
	}
	return len;
}
static DEVICE_ATTR_RO(device_modes);

/* pixel clock limits in kHz: all modes, and modes from T6_WIDE_W wide */
static ssize_t clock_limits_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mct_t6 *t6 = t6_from_dev(dev);

	if (!t6)
		return -ENODEV;
	return sysfs_emit(buf, "%d %d\n", T6_MAX_CLOCK_KHZ, T6_WIDE_MAX_CLOCK_KHZ);
}
static DEVICE_ATTR_RO(clock_limits);

static ssize_t pixel_format_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mct_t6 *t6 = t6_from_dev(dev);
	const char *name = "-";

	if (!t6)
		return -ENODEV;
	mutex_lock(&t6->base.lock);
	if (t6->base.width && t6->lay.planes)
		name = t6_kind_sysfs[t6->lay.kind];
	mutex_unlock(&t6->base.lock);
	return sysfs_emit(buf, "%s\n", name);
}
static DEVICE_ATTR_RO(pixel_format);

static ssize_t format_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mct_t6 *t6 = t6_from_dev(dev);
	int req;

	if (!t6)
		return -ENODEV;
	req = READ_ONCE(t6->fmt_req);
	return sysfs_emit(buf, "%s\n", req == T6_FMT_AUTO ? "auto" : t6_kind_param[req]);
}

/* takes effect with the next update; send one now */
static ssize_t format_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t len)
{
	struct mct_t6 *t6 = t6_from_dev(dev);
	int req = t6_parse_format(buf);

	if (!t6)
		return -ENODEV;
	if (req == -EINVAL)
		return req;
	WRITE_ONCE(t6->fmt_req, req);
	mutex_lock(&t6->base.lock);
	t6->base.dirty = true;
	mutex_unlock(&t6->base.lock);
	mct_kick(&t6->base);
	return len;
}
static DEVICE_ATTR_RW(format);

static ssize_t all_modes_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mct_t6 *t6 = t6_from_dev(dev);

	return t6 ? sysfs_emit(buf, "%d\n", READ_ONCE(t6->all_modes)) : -ENODEV;
}

/* the mode list changes: let userspace probe the monitor again */
static ssize_t all_modes_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t len)
{
	struct mct_t6 *t6 = t6_from_dev(dev);
	bool v;
	int ret = kstrtobool(buf, &v);

	if (!t6)
		return -ENODEV;
	if (ret)
		return ret;
	if (v != READ_ONCE(t6->all_modes)) {
		WRITE_ONCE(t6->all_modes, v);
		drm_kms_helper_hotplug_event(&t6->base.dev);
	}
	return len;
}
static DEVICE_ATTR_RW(all_modes);

static struct attribute *t6_attrs[] = {
	&dev_attr_updates.attr,
	&dev_attr_bytes.attr,
	&dev_attr_errors.attr,
	&dev_attr_recoveries.attr,
	&dev_attr_vram_mb.attr,
	&dev_attr_feature_level.attr,
	&dev_attr_usb_speed.attr,
	&dev_attr_platform.attr,
	&dev_attr_board.attr,
	&dev_attr_firmware.attr,
	&dev_attr_serial.attr,
	&dev_attr_display_flags.attr,
	&dev_attr_functions.attr,
	&dev_attr_device_modes.attr,
	&dev_attr_clock_limits.attr,
	&dev_attr_pixel_format.attr,
	&dev_attr_format.attr,
	&dev_attr_all_modes.attr,
	NULL
};

static ssize_t t6_inject_write(struct file *file, const char __user *buf, size_t len,
			       loff_t *pos)
{
	struct mct_t6 *t6 = file->private_data;
	int v, ret;

	ret = kstrtoint_from_user(buf, len, 0, &v);
	if (ret)
		return ret;
	if (v < 1 || v > 3)
		return -EINVAL;
	atomic_set(&t6->inject, v);
	mutex_lock(&t6->base.lock);
	t6->base.dirty = true;
	mutex_unlock(&t6->base.lock);
	mct_kick(&t6->base);
	return len;
}

static const struct file_operations t6_inject_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = t6_inject_write,
};

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
	 * the exporter's vmap; from a discrete GPU that may be VRAM over PCIe.
	 * An attached import would ask the exporter to pin the buffer in
	 * system memory, which some exporters' vmap then refuses.
	 */
	DRM_GEM_SHMEM_DRIVER_OPS,
	DRM_FBDEV_SHMEM_DRIVER_OPS,
	MCT_COMPAT_DRIVER_OPS
};

static const struct mct_funcs t6_funcs = {
	.get_modes = t6_get_modes,
	.mode_valid = t6_mode_valid,
	.detect = t6_detect,
	.enable = t6_enable,
	.disable = t6_disable,
	.stop = t6_stop,
	.update = t6_update,
	.gamma = t6_gamma,
	.cursor_set = t6_cursor_set,
	.cursor_hide = t6_cursor_hide,
	.stats = t6_stats,
};

static void t6_release(struct drm_device *dev, void *res)
{
	struct mct_t6 *t6 = to_t6(to_mct(dev));

	usb_free_urb(t6->ev_urb);
	drm_edid_free(t6->last_edid);
	kvfree(t6->img);
	kvfree(t6->hw[0]);
	kvfree(t6->hw[1]);
	kvfree(t6->hw[2]);
}

/*
 * What the device is, read as the vendor drivers do: the platform (0xb0
 * index 0: 0 "Lite" with a 48 MHz PLL base, 1 "Super Lite" with 40 MHz),
 * the feature level (index 4; below 3 the device takes only JPEG or I420
 * frames, which this driver doesn't send), video memory in MiB (0x88) and
 * the display function flags of the DISP descriptor (0xb3).
 */
static int t6_read_caps(struct mct_t6 *t6)
{
	struct drm_device *dev = &t6->base.dev;
	struct usb_device *udev = t6_udev(t6);
	int platform = 1, ret, i;

	mutex_lock(&t6->io_lock);
	if (t6_ctrl_in(t6, 0xb0, 0, 0, 4) == 4)
		platform = t6->ctrl[0];
	if (t6_ctrl_in(t6, 0xb0, 0, 1, 4) == 4)
		t6->boot_code = get_unaligned_le32(t6->ctrl);
	if (t6_ctrl_in(t6, 0xb0, 0, 2, 4) == 4)
		t6->image_code = get_unaligned_le32(t6->ctrl);
	ret = t6_ctrl_in(t6, 0xb0, 0, 3, 16);
	if (ret > 0) {
		memcpy(t6->project, t6->ctrl, ret);
		t6->project[ret] = '\0';
		for (i = 0; t6->project[i]; i++) {
			if (!isprint(t6->project[i])) {
				t6->project[i] = '\0';
				break;
			}
		}
	}
	if (t6_ctrl_in(t6, 0xb0, 0, 5, 8) == 8) {
		memcpy(t6->serial, t6->ctrl, 8);
		t6->has_serial = memchr_inv(t6->serial, 0, 8) && memchr_inv(t6->serial, 0xff, 8);
	}
	/* display (index 0) and audio (index 3) functions: id and UTF-16 name */
	for (i = 0; i < ARRAY_SIZE(t6->func); i++) {
		int j;

		if (t6_ctrl_in(t6, 0xb1, 0, i ? 3 : 0, 132) < 0x44)
			continue;
		t6->func[i].id = get_unaligned_le16(t6->ctrl + 2);
		for (j = 0; j < 32; j++) {
			u16 c = get_unaligned_le16(t6->ctrl + 4 + 2 * j);

			if (!c)
				break;
			t6->func[i].name[j] = c < 0x80 && isprint(c) ? c : '?';
		}
	}
	ret = t6_ctrl_in(t6, 0xb0, 0, 4, 4);
	t6->level = ret == 4 ? get_unaligned_le32(t6->ctrl) : -1;
	ret = t6_ctrl_in(t6, 0x88, 0, 0, 1);
	t6->vram_mb = ret == 1 ? t6->ctrl[0] : 0;
	ret = t6_ctrl_in(t6, 0xb3, 0, 0, 112);
	t6->disp_func = ret >= 0x58 ? get_unaligned_le32(t6->ctrl + 0x54) : 0;
	mutex_unlock(&t6->io_lock);

	t6->platform = platform;
	t6->pll_base_khz = platform == 0 ? 48000 : 40000;
	t6->usb2 = udev->speed < USB_SPEED_SUPER;
	t6->frag = t6->usb2 ? T6_FRAG_HS : T6_FRAG;
	drm_info(dev, "%s, board %s, firmware %08x/%08x, feature level %d, %d MiB, display flags %#x%s\n",
		 platform ? "Super Lite" : "Lite", t6->project[0] ? t6->project : "?",
		 t6->boot_code, t6->image_code, t6->level, t6->vram_mb, t6->disp_func,
		 t6->usb2 ? ", USB 2 link: modes up to 2048 wide" : "");

	if (t6->level >= 0 && t6->level < 3) {
		drm_err(dev, "feature level %d takes only JPEG frames, not supported\n", t6->level);
		return -ENODEV;
	}
	/* the three frame buffers end at 48 MiB */
	if (t6->vram_mb && t6->vram_mb < 48) {
		drm_err(dev, "%d MiB of video memory is too little\n", t6->vram_mb);
		return -ENODEV;
	}
	return 0;
}

/*
 * Single-output models report their HDMI port as output 0 (5600) or 1
 * (5601): use the first one with a monitor, else 0.
 */
static void t6_pick_output(struct mct_t6 *t6)
{
	int i;

	t6->output = le16_to_cpu(t6_udev(t6)->descriptor.idProduct) == 0x5601 ? 1 : 0;
	for (i = 0; i < T6_MAX_OUTPUTS; i++) {
		if (t6_output_status(t6, i) == 1) {
			t6->output = i;
			return;
		}
	}
}

static int t6_usb_probe(struct usb_interface *interface, const struct usb_device_id *id)
{
	struct mct_config cfg = {
		.funcs = &t6_funcs,
		.min_w = 64,
		.min_h = 64,
		.connector_type = DRM_MODE_CONNECTOR_HDMIA,
		.encoder_type = DRM_MODE_ENCODER_TMDS,
		.cursor_size = T6_CUR,
		.poll = true,
		.gamma = true,
		.stats_name = "mct_t6_stats",
		.chip = "t6",
		.attrs = t6_attrs,
	};
	struct mct_t6 *t6;
	struct drm_device *dev;
	int ret, i;
	static const u8 bulk_eps[] = { T6_EP_OUT, 0 };

	BUILD_BUG_ON(sizeof(struct t6_mode) != 32);
	BUILD_BUG_ON(sizeof(struct t6_session) != 32);
	BUILD_BUG_ON(sizeof(struct t6_video_header) != 48);

	if (interface->cur_altsetting->desc.bInterfaceNumber != 0)
		return -ENODEV;
	/* refuse look-alikes without the expected endpoints */
	if (!usb_check_bulk_endpoints(interface, bulk_eps))
		return -ENODEV;

	t6 = devm_drm_dev_alloc(&interface->dev, &t6_drm_driver, struct mct_t6, base.dev);
	if (IS_ERR(t6))
		return PTR_ERR(t6);
	dev = &t6->base.dev;

	/* the module parameters are the defaults this device starts with */
	kernel_param_lock(THIS_MODULE);
	t6->fmt_req = format ? t6_parse_format(format) : T6_FMT_AUTO;
	kernel_param_unlock(THIS_MODULE);
	if (t6->fmt_req == -EINVAL)
		t6->fmt_req = T6_FMT_AUTO;
	t6->all_modes = all_modes;
	INIT_DELAYED_WORK(&t6->hpd_work, t6_hpd_work);
	INIT_DELAYED_WORK(&t6->ev_retry, t6_ev_retry);
	INIT_WORK(&t6->redraw_work, t6_redraw_work);
	ret = drmm_mutex_init(dev, &t6->cur.lock) ?: drmm_mutex_init(dev, &t6->mode_lock);
	if (ret)
		return ret;
	t6->hpd_state = -1;
	t6->hpd_last = -1;
	for (i = 0; i < 256; i++) {
		t6->lut[0][i] = i;
		t6->lut[1][i] = i;
		t6->lut[2][i] = i;
	}
	ret = drmm_mutex_init(dev, &t6->io_lock);
	if (ret)
		return ret;

	t6->ctrl = drmm_kmalloc(dev, T6_CTRL_BUF, GFP_KERNEL);
	t6->cur.img = drmm_kmalloc(dev, T6_CUR_BYTES, GFP_KERNEL);
	t6->cur.shown = drmm_kmalloc(dev, T6_CUR_BYTES, GFP_KERNEL);
	if (!t6->ctrl || !t6->cur.img || !t6->cur.shown)
		return -ENOMEM;
	ret = t6_alloc_urbs(t6, interface);
	if (ret)
		return ret;

	t6_pick_output(t6);
	ret = t6_read_caps(t6);
	if (ret)
		return ret;
	ret = t6_read_modes(t6);
	if (ret) {
		drm_err(dev, "could not read the mode table: %d\n", ret);
		return ret;
	}

	t6->stale[0] = drmm_kzalloc(dev, t6->max_h, GFP_KERNEL);
	t6->stale[1] = drmm_kzalloc(dev, t6->max_h, GFP_KERNEL);
	t6->stale[2] = drmm_kzalloc(dev, t6->max_h, GFP_KERNEL);
	ret = drmm_add_action_or_reset(dev, t6_release, NULL);
	if (ret)
		return ret;
	if (!t6->stale[0] || !t6->stale[1] || !t6->stale[2])
		return -ENOMEM;

	cfg.max_w = t6->max_w;
	cfg.max_h = t6->max_h;
	ret = mct_init(&t6->base, &cfg);
	if (ret)
		return ret;
	ret = t6_events_init(t6, interface);
	if (ret)
		return ret;

	ret = mct_register(&t6->base, interface);
	if (ret)
		return ret;
	t6->hpd_events = !usb_submit_urb(t6->ev_urb, GFP_KERNEL);
	if (!t6->hpd_events)
		drm_warn(dev, "no monitor change events, polling only\n");
	if (!IS_ERR_OR_NULL(dev->debugfs_root))
		debugfs_create_file("mct_t6_inject", 0200, dev->debugfs_root, t6,
				    &t6_inject_fops);
	return 0;
}

/* no more events or event work, also none queued from a pending retry */
static void t6_stop_events(struct mct_t6 *t6)
{
	usb_poison_urb(t6->ev_urb);
	cancel_delayed_work_sync(&t6->ev_retry);
	cancel_delayed_work_sync(&t6->hpd_work);
	cancel_work_sync(&t6->redraw_work);
}

static void t6_usb_disconnect(struct usb_interface *interface)
{
	struct mct_t6 *t6 = to_t6(to_mct(usb_get_intfdata(interface)));

	t6_stop_events(t6);
	mct_unplug(&t6->base);
}

static int t6_suspend(struct usb_interface *interface, pm_message_t message)
{
	struct mct_t6 *t6 = to_t6(to_mct(usb_get_intfdata(interface)));
	int ret;

	ret = mct_suspend(&t6->base);
	/* after a failed suspend there is no resume to start them again */
	if (!ret)
		t6_stop_events(t6);
	return ret;
}

static int t6_resume(struct usb_interface *interface)
{
	struct mct_t6 *t6 = to_t6(to_mct(usb_get_intfdata(interface)));

	int ret;

	t6->hpd_state = -1;	/* the monitor may have changed meanwhile */
	atomic_set(&t6->ev_fails, 0);
	usb_unpoison_urb(t6->ev_urb);
	if (t6->hpd_events)
		usb_submit_urb(t6->ev_urb, GFP_KERNEL);
	ret = mct_resume(&t6->base);
	/* re-read the monitor in a moment, so a swapped one is noticed */
	atomic_set(&t6->hpd_same, 0);
	mod_delayed_work(system_percpu_wq, &t6->hpd_work, msecs_to_jiffies(T6_HPD_MS));
	return ret;
}

const struct mct_backend mct_t6_backend = {
	.probe = t6_usb_probe,
	.disconnect = t6_usb_disconnect,
	.suspend = t6_suspend,
	.resume = t6_resume,
};
