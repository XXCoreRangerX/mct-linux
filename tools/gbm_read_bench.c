// SPDX-License-Identifier: GPL-2.0-only
// CPU read speed of GBM buffers on the primary GPU, allocated like niri does
// for a display-only device (RENDERING | SCANOUT, LINEAR), plus variants.
#include <fcntl.h>
#include <gbm.h>
#include <immintrin.h>
#include <linux/dma-buf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define W 1920
#define H 1080

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static void stream_copy(void *dst, const void *src, size_t n)
{
	const __m128i *s = src; __m128i *d = dst;
	for (size_t i = 0; i < n / 64; i++, s += 4, d += 4) {
		__m128i a = _mm_stream_load_si128((__m128i *)s), b = _mm_stream_load_si128((__m128i *)s + 1);
		__m128i c = _mm_stream_load_si128((__m128i *)s + 2), e = _mm_stream_load_si128((__m128i *)s + 3);
		_mm_store_si128(d, a); _mm_store_si128(d + 1, b); _mm_store_si128(d + 2, c); _mm_store_si128(d + 3, e);
	}
}

static void run(struct gbm_device *gbm, const char *name, uint32_t flags)
{
	uint64_t mod = 0; /* DRM_FORMAT_MOD_LINEAR */
	struct gbm_bo *bo = gbm_bo_create_with_modifiers2(gbm, W, H, GBM_FORMAT_XRGB8888, &mod, 1, flags);
	if (!bo) { printf("%-28s allocation failed\n", name); return; }
	int fd = gbm_bo_get_fd(bo);
	uint32_t stride = gbm_bo_get_stride(bo);
	size_t len = (size_t)stride * H;
	void *map = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) { printf("%-28s mmap failed\n", name); goto out; }
	void *dst = aligned_alloc(64, len);
	struct dma_buf_sync sync = { DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW };
	ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
	memset(map, 0x40, len);                     /* touch it so it gets placed */
	sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW; ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
	double best_m = 1e9, best_s = 1e9;
	for (int i = 0; i < 5; i++) {
		sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ; ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
		double t0 = now(); memcpy(dst, map, len); double t1 = now();
		stream_copy(dst, map, len); double t2 = now();
		sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ; ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
		if (t1 - t0 < best_m) best_m = t1 - t0;
		if (t2 - t1 < best_s) best_s = t2 - t1;
	}
	printf("%-28s stride %u: memcpy %6.2f ms (%5.2f GB/s), streaming %6.2f ms (%5.2f GB/s)\n",
	       name, stride, best_m * 1e3, len / best_m / 1e9, best_s * 1e3, len / best_s / 1e9);
	free(dst);
	munmap(map, len);
out:
	close(fd);
	gbm_bo_destroy(bo);
}

int main(int argc, char **argv)
{
	const char *node = argc > 1 ? argv[1] : "/dev/dri/renderD128";
	int fd = open(node, O_RDWR);
	if (fd < 0) { perror(node); return 1; }
	struct gbm_device *gbm = gbm_create_device(fd);
	printf("%s (%s)\n", node, gbm_device_get_backend_name(gbm));
	run(gbm, "rendering|scanout (niri)", GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
	run(gbm, "rendering only", GBM_BO_USE_RENDERING);
	run(gbm, "rendering|linear", GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
	run(gbm, "scanout|linear", GBM_BO_USE_SCANOUT | GBM_BO_USE_LINEAR);
	gbm_device_destroy(gbm);
	close(fd);
	return 0;
}
