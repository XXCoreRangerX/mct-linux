/*
 * t2d — Wacom DTU-1031X (MCT Trigger II, USB 0711:520d) display daemon.
 *
 * EVDI gives the compositor a real 1024x600@60 connector (panel EDID); this
 * daemon grabs damaged pixels from it and streams them to the tablet:
 *   - big damage   -> full 0x13 frame into the back buffer + 0x15 flip
 *   - small damage -> partial 0x13 rects straight into the front buffer
 * Protocol: docs/protocol-t2.md. The panel scans out rotated 180 degrees.
 *
 * Usage: t2d [-i init.bin] [-e edid.bin] [-n] [-t test] [-v]
 *   -n        skip device init (tablet already initialised)
 *   -t test   no EVDI: send a test pattern + partial rect and exit
 *   -t encode in.xrgb out.bin   offline: encode a 1024x600 XRGB frame (no USB)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libusb-1.0/libusb.h>
#include "evdi_lib.h"

#define W 1024
#define H 600
#define FB_A 0x00C000u
#define FB_B 0x1CE000u
#define EP_FB 0x02
#define EP_CMD 0x03
#define EP_IN 0x81
#define CHUNK 19968
#define MAX_RECTS 16

#ifndef T2D_DATADIR
#define T2D_DATADIR "."
#endif

static libusb_device_handle *usb;
static int verbose;
static volatile sig_atomic_t quit;

static void logf_(const char *fmt, ...)
{
	struct timespec ts;
	va_list ap;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	fprintf(stderr, "[%5ld.%03ld] ", ts.tv_sec % 100000, ts.tv_nsec / 1000000);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ---------------------------------------------------------------- USB */

static int bulk_out(unsigned char ep, const void *buf, int len, unsigned timeout)
{
	int done = 0, r;

	r = libusb_bulk_transfer(usb, ep, (unsigned char *)buf, len, &done, timeout);
	if (r || done != len) {
		logf_("bulk OUT ep%02x len %d: %s (done %d)", ep, len, libusb_error_name(r), done);
		return -1;
	}
	return 0;
}

static int bulk_in(int len)
{
	unsigned char buf[1024];
	int done = 0, r, tries;

	for (tries = 0; tries < 6; tries++) {
		r = libusb_bulk_transfer(usb, EP_IN, buf, len > 1024 ? 1024 : len, &done, 3000);
		if (!r)
			return 0;
		usleep(500000);
	}
	logf_("bulk IN failed: %s", libusb_error_name(r));
	return -1;
}

/* one command = header transfer + payload transfers (firmware is transfer-framed) */
static int send_cmd(const unsigned char *hdr, int hlen, const unsigned char *pay, int plen)
{
	int off;

	if (bulk_out(EP_FB, hdr, hlen, 5000))
		return -1;
	for (off = 0; off < plen; off += CHUNK) {
		int n = plen - off < CHUNK ? plen - off : CHUNK;

		if (bulk_out(EP_FB, pay + off, n, 30000))
			return -1;
	}
	return 0;
}

static int send_rect(uint32_t base, const unsigned char *src, int stride, int dx, int dy, int w, int h);

/* 8051 pacing rule, validated from cold (docs/init-sequence.md) */
static unsigned pace_us(unsigned ep, const unsigned char *p)
{
	if (ep == EP_FB)
		return 0;
	if (ep == EP_CMD && p[0] == 0x02)	/* single register write */
		return p[3] == 0xfe && (p[4] == 0xb0 || p[4] == 0xb1) ? 50000 : 2000;
	return 20000;
}

static int run_init(const char *path)
{
	static unsigned char zeros[65536];
	FILE *f = fopen(path, "rb");
	unsigned char hdr[6], *data = NULL;
	int n = 0;
	double t0 = now();

	if (!f) {
		logf_("init blob %s: %s", path, strerror(errno));
		return -1;
	}
	while (fread(hdr, 1, 6, f) == 6) {
		unsigned ep = hdr[0], flags = hdr[1];
		uint32_t len = hdr[2] | hdr[3] << 8 | hdr[4] << 16 | (uint32_t)hdr[5] << 24;
		uint32_t rdlen = 0;
		const unsigned char *p;

		if (flags & 1) {
			if (len > sizeof(zeros))
				goto bad;
			p = zeros;
		} else {
			data = realloc(data, len);
			if (fread(data, 1, len, f) != len)
				goto bad;
			p = data;
		}
		if (flags & 2 && fread(&rdlen, 4, 1, f) != 1)
			goto bad;
		/*
		 * 0x14 display enable scans out buffer A: put a black frame there first
		 * (the trimmed init no longer clears video memory).
		 */
		if (ep == EP_CMD && p[0] == 0x14) {
			unsigned char *blk = calloc((size_t)W * H, 4);

			if (!blk || send_rect(FB_A, blk, W * 4, 0, 0, W, H) < 0) {
				free(blk);
				goto bad;
			}
			free(blk);
		}
		if (bulk_out(ep, p, len, 30000))
			goto bad;
		if (flags & 2 && bulk_in(rdlen))
			goto bad;
		usleep(pace_us(ep, p));
		n++;
	}
	fclose(f);
	free(data);
	logf_("init: %d transfers in %.1fs", n, now() - t0);
	return 0;
bad:
	logf_("init failed at record %d", n);
	fclose(f);
	free(data);
	return -1;
}

/* ---------------------------------------------------------------- RLE */

/* Encode one channel line of n pixels (values gathered already). Returns bytes written. */
static int rle_line(const unsigned char *v, int n, unsigned char *out)
{
	unsigned char *o = out;
	int x = 0;

	while (x < n) {
		unsigned char c = v[x] == 0x30 ? 0x31 : v[x];
		int r = 1;

		while (x + r < n && r < 0xFB && (v[x + r] == 0x30 ? 0x31 : v[x + r]) == c)
			r++;
		if (r >= 3) {
			if (r - 1 == 0x30) {
				*o++ = 0x30; *o++ = 0x2F; *o++ = c; *o++ = c;
			} else {
				*o++ = 0x30; *o++ = (unsigned char)(r - 1); *o++ = c;
			}
		} else {
			for (int i = 0; i < r; i++)
				*o++ = c;
		}
		x += r;
	}
	return (int)(o - out);
}

/*
 * Encode device rect (dx,dy,w,h) of the visible XRGB8888 image `src` (stride
 * bytes), applying the 180-degree rotation: device (X,Y) = visible (W-1-X, H-1-Y).
 * Device rows >= H (alignment overshoot) are filled with black.
 * Pixels stream row-major, cut into 1024-px units, each unit as R, G, B lines.
 */
static int encode_rect(const unsigned char *src, int stride, int dx, int dy, int w, int h,
		       unsigned char *out)
{
	static unsigned char ch[3][1024];
	int k = 0, len = 0;

	for (int y = dy; y < dy + h; y++) {
		for (int x = dx; x < dx + w; x++) {
			if (y < H) {
				const unsigned char *px = src + (size_t)(H - 1 - y) * stride + (size_t)(W - 1 - x) * 4;

				ch[0][k] = px[2]; ch[1][k] = px[1]; ch[2][k] = px[0];
			} else {
				ch[0][k] = ch[1][k] = ch[2][k] = 0;
			}
			if (++k == 1024) {
				for (int c = 0; c < 3; c++)
					len += rle_line(ch[c], 1024, out + len);
				k = 0;
			}
		}
	}
	return len;
}

static void put24(unsigned char *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; }
static void put16(unsigned char *p, uint32_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(unsigned char *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

static unsigned char *encbuf;

static int send_rect(uint32_t base, const unsigned char *src, int stride, int dx, int dy, int w, int h)
{
	unsigned char hdr[36];
	uint32_t a = (base + (uint32_t)dy * W * 3 + (uint32_t)dx * 3) << 8;
	int len = encode_rect(src, stride, dx, dy, w, h, encbuf);

	hdr[0] = 0x13;
	put32(hdr + 1, a);
	put32(hdr + 5, a);
	hdr[9] = 0;
	put16(hdr + 10, w); put16(hdr + 12, h); put16(hdr + 14, w); put16(hdr + 16, h);
	memcpy(hdr + 18, "\x00\x40\x00\x40\x00\x00\x00\x39\x00", 9);
	put24(hdr + 27, (uint32_t)w * h * 3);
	put24(hdr + 30, len);
	memcpy(hdr + 33, "\x30\x05\x28", 3);
	return send_cmd(hdr, 36, encbuf, len) ? -1 : len;
}

static int send_flip(uint32_t base)
{
	uint32_t a = base >> 2;
	unsigned char c[23] = { 0x15, 0x07, 0xfb, 0x65, base == FB_A ? 0x10 : 0x20,
				0xfc, 0xb4, 0x11, 0xfc, 0xb1, a & 0xff, 0xfc, 0xb2, (a >> 8) & 0xff,
				0xfc, 0xb3, (a >> 16) & 0xff, 0xfc, 0xb4, 0x15, 0xfc, 0xb4, 0x11 };

	return bulk_out(EP_FB, c, sizeof(c), 5000);
}

/* ---------------------------------------------------------------- display state */

static uint32_t front = FB_A;

static int show_full(const unsigned char *src, int stride)
{
	uint32_t back = front == FB_A ? FB_B : FB_A;

	if (send_rect(back, src, stride, 0, 0, W, H) < 0 || send_flip(back))
		return -1;
	front = back;
	return 0;
}

static int gcd(int a, int b) { while (b) { int t = a % b; a = b; b = t; } return a; }

/* visible-space damage rect -> aligned device rect, sent into front buffer */
static int show_partial(const unsigned char *src, int stride, const struct evdi_rect *r)
{
	int x1 = r->x1 < 0 ? 0 : r->x1, y1 = r->y1 < 0 ? 0 : r->y1;
	int x2 = r->x2 > W ? W : r->x2, y2 = r->y2 > H ? H : r->y2;
	int dx1, dx2, dy1, dy2, w, h, m;

	if (x2 <= x1 || y2 <= y1)
		return 0;
	/* rotate 180 */
	dx1 = W - x2; dx2 = W - x1; dy1 = H - y2; dy2 = H - y1;
	/* x: 64-px units */
	dx1 &= ~63; dx2 = (dx2 + 63) & ~63;
	w = dx2 - dx1;
	/* h: w*h must be a multiple of 1024 */
	m = 16 / gcd(w / 64, 16);
	h = ((dy2 - dy1) + m - 1) / m * m;
	if (dy1 + h > H)
		dy1 = H - h >= 0 ? (H - h) : 0; /* shift up; overshoot only if h > H */
	return send_rect(front, src, stride, dx1, dy1, w, h) < 0 ? -1 : 0;
}

/* ---------------------------------------------------------------- test pattern */

static void test_pattern(unsigned char *img, int stride)
{
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++) {
			unsigned char *p = img + y * stride + x * 4;
			int r = 0, g = 0, b = 0;

			if (y < 200 && x < 200) r = 255;
			else if (y < 200 && x >= W - 200) g = 255;
			else if (y >= H - 200 && x < 200) b = 255;
			else if (y >= H - 200 && x >= W - 200) r = g = b = 255;
			else if (y >= 250 && y < 300) r = g = b = x * 255 / (W - 1);
			else if ((x / 32 + y / 32) % 2) r = g = b = 40;
			p[0] = b; p[1] = g; p[2] = r; p[3] = 0;
		}
}

/* ---------------------------------------------------------------- EVDI */

struct ctx {
	evdi_handle h;
	unsigned char *buf;
	int stride, registered, update_ready, width, height;
	struct evdi_rect rects[MAX_RECTS];
};

static void on_mode(struct evdi_mode mode, void *ud)
{
	struct ctx *c = ud;

	logf_("mode %dx%d@%d bpp %d fmt %08x", mode.width, mode.height, mode.refresh_rate,
	      mode.bits_per_pixel, mode.pixel_format);
	if (c->registered) {
		evdi_unregister_buffer(c->h, 0);
		c->registered = 0;
	}
	c->width = mode.width;
	c->height = mode.height;
	c->stride = mode.width * 4;
	free(c->buf);
	c->buf = calloc((size_t)c->stride, mode.height);
	struct evdi_buffer b = { .id = 0, .buffer = c->buf, .width = mode.width,
				 .height = mode.height, .stride = c->stride,
				 .rects = c->rects, .rect_count = MAX_RECTS };
	evdi_register_buffer(c->h, b);
	c->registered = 1;
}

static void on_update(int buf, void *ud) { (void)buf; ((struct ctx *)ud)->update_ready = 1; }
/* DPMS: fcf6 = pixel-clock PLL enable; 0 makes the panel switch its backlight off */
static void on_dpms(int mode, void *ud)
{
	unsigned char cmd[6] = { 0x02, 0x00, 0x01, 0xfc, 0xf6, mode == 0 ? 1 : 0 };

	(void)ud;
	logf_("dpms %d -> panel %s", mode, mode == 0 ? "on" : "off");
	bulk_out(EP_CMD, cmd, sizeof(cmd), 5000);
}
static void on_crtc(int st, void *ud) { (void)ud; logf_("crtc state %d", st); }

static void evdi_log(void *ud, const char *fmt, ...)
{
	va_list ap;

	(void)ud;
	if (!verbose)
		return;
	va_start(ap, fmt);
	fprintf(stderr, "evdi: ");
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

static void on_signal(int s) { (void)s; quit = 1; }

static int evdi_loop(const char *edid_path)
{
	unsigned char edid[512];
	int edid_len, dev = -1, frames = 0;
	long bytes = 0;
	double tstat = now();
	FILE *f = fopen(edid_path, "rb");
	struct ctx c = { 0 };
	struct evdi_event_context ev = { .dpms_handler = on_dpms, .mode_changed_handler = on_mode,
					 .update_ready_handler = on_update, .crtc_state_handler = on_crtc,
					 .user_data = &c };
	struct evdi_logging lg = { .function = evdi_log };

	if (!f) {
		logf_("edid %s: %s", edid_path, strerror(errno));
		return -1;
	}
	edid_len = fread(edid, 1, sizeof(edid), f);
	fclose(f);
	evdi_set_logging(lg);

	for (int i = 0; i < 32; i++)
		if (evdi_check_device(i) == AVAILABLE) {
			dev = i;
			break;
		}
	if (dev < 0) {
		logf_("no EVDI device (load module: insmod evdi.ko initial_device_count=1)");
		return -1;
	}
	c.h = evdi_open(dev);
	if (c.h == EVDI_INVALID_HANDLE) {
		logf_("evdi_open(%d) failed", dev);
		return -1;
	}
	logf_("EVDI card%d opened, connecting with %d-byte EDID", dev, edid_len);
	evdi_connect2(c.h, edid, edid_len, W * H, W * H * 60);

	struct pollfd pfd = { .fd = evdi_get_event_ready(c.h), .events = POLLIN };

	while (!quit) {
		if (c.registered && !c.update_ready && evdi_request_update(c.h, 0))
			c.update_ready = 1;
		if (!c.update_ready) {
			if (poll(&pfd, 1, 1000) > 0)
				evdi_handle_events(c.h, &ev);
			continue;
		}
		c.update_ready = 0;
		if (!c.registered)
			continue;

		int n = MAX_RECTS;
		long area = 0;

		evdi_grab_pixels(c.h, c.rects, &n);
		if (c.width != W || c.height != H) {
			if (verbose)
				logf_("ignoring update for mode %dx%d", c.width, c.height);
			continue;
		}
		for (int i = 0; i < n; i++)
			area += (long)(c.rects[i].x2 - c.rects[i].x1) * (c.rects[i].y2 - c.rects[i].y1);
		if (n == 0)
			continue;
		if (area * 3 > (long)W * H || n >= MAX_RECTS || frames == 0) {
			if (show_full(c.buf, c.stride))
				break;
		} else {
			for (int i = 0; i < n; i++)
				if (show_partial(c.buf, c.stride, &c.rects[i]))
					goto out;
		}
		frames++;
		bytes += area;
		if (now() - tstat >= 5) {
			logf_("%.1f updates/s, %.0f kpx/s damage", frames / (now() - tstat),
			      bytes / 1000.0 / (now() - tstat));
			frames = 1;
			bytes = 0;
			tstat = now();
		}
	}
out:
	evdi_disconnect(c.h);
	evdi_close(c.h);
	free(c.buf);
	return 0;
}

/* ---------------------------------------------------------------- main */

int main(int argc, char **argv)
{
	const char *init = T2D_DATADIR "/init.bin", *edid = T2D_DATADIR "/panel_edid.bin", *test = NULL;
	int opt, noinit = 0, ret = 1;

	while ((opt = getopt(argc, argv, "i:e:nt:v")) != -1) {
		switch (opt) {
		case 'i': init = optarg; break;
		case 'e': edid = optarg; break;
		case 'n': noinit = 1; break;
		case 't': test = optarg; break;
		case 'v': verbose = 1; break;
		default:
			fprintf(stderr, "usage: %s [-i init.bin] [-e edid.bin] [-n] [-t pattern] [-v]\n", argv[0]);
			return 2;
		}
	}
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	encbuf = malloc((size_t)W * (H + 16) * 3 + 4096);
	if (optind + 2 == argc && test && !strcmp(test, "encode")) {
		/* offline: t2d -t encode in.xrgb out.bin -> full-frame RLE payload (rotated) */
		unsigned char *img = malloc((size_t)W * H * 4);
		FILE *fi = fopen(argv[optind], "rb"), *fo = fopen(argv[optind + 1], "wb");
		int len;

		if (!fi || !fo || fread(img, 1, (size_t)W * H * 4, fi) != (size_t)W * H * 4)
			return 1;
		len = encode_rect(img, W * 4, 0, 0, W, H, encbuf);
		fwrite(encbuf, 1, len, fo);
		fclose(fi);
		fclose(fo);
		return 0;
	}
	if (libusb_init(NULL))
		return 1;
	usb = libusb_open_device_with_vid_pid(NULL, 0x0711, 0x520d);
	if (!usb) {
		logf_("tablet 0711:520d not found / no permission");
		goto done;
	}
	if (libusb_claim_interface(usb, 0)) {
		logf_("claim interface 0 failed");
		goto done;
	}
	if (!noinit && run_init(init))
		goto rel;

	if (test) {
		unsigned char *img = malloc((size_t)W * H * 4);
		double t0 = now();

		test_pattern(img, W * 4);
		if (show_full(img, W * 4) == 0)
			logf_("test pattern: full frame + flip in %.3fs", now() - t0);
		/* then a partial: bright magenta box at visible (412,150)-(612,230) */
		for (int y = 150; y < 230; y++)
			for (int x = 412; x < 612; x++)
				memcpy(img + y * W * 4 + x * 4, "\xff\x00\xff\x00", 4);
		sleep(2);
		struct evdi_rect r = { 412, 150, 612, 230 };
		t0 = now();
		if (show_partial(img, W * 4, &r) == 0)
			logf_("test partial rect in %.3fs", now() - t0);
		free(img);
		ret = 0;
		goto rel;
	}
	ret = evdi_loop(edid) ? 1 : 0;
rel:
	libusb_release_interface(usb, 0);
done:
	if (usb)
		libusb_close(usb);
	libusb_exit(NULL);
	return ret;
}
