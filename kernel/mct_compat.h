/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Compatibility with kernels older than the one the driver is written for.
 *
 * The driver uses the DRM API of Linux 7.2. Kbuild checks the kernel's
 * headers for each newer interface and sets an MCT_COMPAT_* flag where one
 * is missing; this file maps the newer calls onto what such a kernel has.
 * On 7.2 and later no flag is set and nothing here changes the code.
 * Built and tested on hardware with 7.2; builds checked against 6.12 and
 * 6.18.
 */
#ifndef MCT_COMPAT_H
#define MCT_COMPAT_H

#include <linux/hrtimer.h>
#include <linux/spinlock.h>

#include <drm/drm_fbdev_shmem.h>

/* drm_client_setup(): drm/clients/ since 6.15, drm/ before; none before 6.13 */
#if defined(MCT_COMPAT_NO_FBDEV_SHMEM_OPS)
#define DRM_FBDEV_SHMEM_DRIVER_OPS .fbdev_probe = NULL
#define drm_client_setup(dev, format) drm_fbdev_shmem_setup(dev, 0)
#elif defined(MCT_COMPAT_CLIENT_SETUP_OLD_PATH)
#include <drm/drm_client_setup.h>
#else
#include <drm/clients/drm_client_setup.h>
#endif

/* the vblank timer helpers; without them, a copy of the same timer below */
#ifndef MCT_COMPAT_NO_VBLANK_HELPER
#include <drm/drm_vblank_helper.h>
#endif

/*
 * struct drm_atomic_state was renamed struct drm_atomic_commit. drm_atomic.h
 * comes first, so its function drm_atomic_commit() keeps its name.
 */
#ifdef MCT_COMPAT_ATOMIC_STATE
#include <drm/drm_atomic.h>
#define drm_atomic_commit drm_atomic_state
#endif

/* the connector's mode_valid takes a const mode since 6.15 */
#ifdef MCT_COMPAT_MODE_VALID_NONCONST
#define MCT_CONN_MODE_CONST
#else
#define MCT_CONN_MODE_CONST const
#endif

/* the plane's atomic_async_check has a flip argument since 6.13 */
#ifdef MCT_COMPAT_ASYNC_CHECK_NO_FLIP
#define MCT_ASYNC_CHECK_ARGS struct drm_plane *plane, struct drm_atomic_commit *state
#define MCT_ASYNC_FLIP false
#else
#define MCT_ASYNC_CHECK_ARGS struct drm_plane *plane, struct drm_atomic_commit *state, bool flip
#define MCT_ASYNC_FLIP flip
#endif

struct drm_crtc;
struct drm_device;
struct device;
struct dma_buf;

/* state the compatibility code keeps per device, in struct mct_device */
struct mct_compat {
#ifdef MCT_COMPAT_NO_VBLANK_HELPER
	struct drm_crtc *crtc;		/* set when the timer is first started */
	struct hrtimer timer;
	spinlock_t interval_lock;
	ktime_t interval;		/* 0: stop at the next expiry */
#endif
#ifdef MCT_COMPAT_NO_DMA_DEV
	struct device *dma_dev;		/* for buffer imports, held */
#endif
};

#ifdef MCT_COMPAT_NO_VBLANK_HELPER
int mct_compat_enable_vblank(struct drm_crtc *crtc);
void mct_compat_disable_vblank(struct drm_crtc *crtc);
bool mct_compat_get_vblank_timestamp(struct drm_crtc *crtc, int *max_error,
				     ktime_t *vblank_time, bool in_vblank_irq);
void mct_compat_vblank_atomic_flush(struct drm_crtc *crtc, struct drm_atomic_commit *state);

#define DRM_CRTC_VBLANK_TIMER_FUNCS \
	.enable_vblank = mct_compat_enable_vblank, \
	.disable_vblank = mct_compat_disable_vblank, \
	.get_vblank_timestamp = mct_compat_get_vblank_timestamp
#define drm_crtc_vblank_atomic_flush mct_compat_vblank_atomic_flush
#endif

/*
 * Without drm_device.dma_dev, buffer imports go through the driver's
 * gem_prime_import with the USB controller as the DMA device, as udl and
 * gud did before 6.16. MCT_COMPAT_DRIVER_OPS goes into struct drm_driver.
 */
#ifdef MCT_COMPAT_NO_DMA_DEV
void mct_compat_set_dma_dev(struct drm_device *dev, struct device *dma_dev);
struct drm_gem_object *mct_compat_gem_prime_import(struct drm_device *dev,
						   struct dma_buf *dma_buf);
#define drm_dev_set_dma_dev mct_compat_set_dma_dev
#define MCT_COMPAT_DRIVER_OPS .gem_prime_import = mct_compat_gem_prime_import,
#else
#define MCT_COMPAT_DRIVER_OPS
#endif

#endif
