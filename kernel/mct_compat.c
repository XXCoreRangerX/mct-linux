// SPDX-License-Identifier: GPL-2.0-only
/*
 * Compatibility code for kernels older than 7.2, see mct_compat.h. Empty on
 * kernels that have every interface the driver uses.
 */

#include <linux/device.h>
#include <linux/dma-buf.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>

#include <drm/drm_atomic.h>
#include <drm/drm_crtc.h>
#include <drm/drm_drv.h>
#include <drm/drm_managed.h>
#include <drm/drm_prime.h>
#include <drm/drm_print.h>
#include <drm/drm_vblank.h>

#include "mct_drv.h"

#ifdef MCT_COMPAT_NO_VBLANK_HELPER
/*
 * A vblank timer at the mode's frame rate, as drm_crtc_vblank_start_timer()
 * and the vblank timer helpers do in newer kernels (drm_vblank.c,
 * drm_vblank_helper.c), with the timer kept in struct mct_compat.
 */

static struct mct_compat *mct_compat_of(struct drm_crtc *crtc)
{
	return &to_mct(crtc->dev)->compat;
}

static enum hrtimer_restart mct_compat_vblank_fn(struct hrtimer *timer)
{
	struct mct_compat *c = container_of(timer, struct mct_compat, timer);
	unsigned long flags;
	ktime_t interval;

	spin_lock_irqsave(&c->interval_lock, flags);
	interval = c->interval;
	spin_unlock_irqrestore(&c->interval_lock, flags);
	if (!interval)
		return HRTIMER_NORESTART;

	hrtimer_forward_now(&c->timer, interval);
	if (!drm_crtc_handle_vblank(c->crtc))
		return HRTIMER_NORESTART;
	return HRTIMER_RESTART;
}

int mct_compat_enable_vblank(struct drm_crtc *crtc)
{
	struct mct_compat *c = mct_compat_of(crtc);
	struct drm_vblank_crtc *vblank = drm_crtc_vblank_crtc(crtc);
	unsigned long flags;

	if (!c->crtc) {
		c->crtc = crtc;
		spin_lock_init(&c->interval_lock);
#ifdef MCT_COMPAT_NO_HRTIMER_SETUP
		hrtimer_init(&c->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
		c->timer.function = mct_compat_vblank_fn;
#else
		hrtimer_setup(&c->timer, mct_compat_vblank_fn, CLOCK_MONOTONIC,
			      HRTIMER_MODE_REL);
#endif
	} else {
		/* a cancelled timer may still be running its last expiry */
		while (hrtimer_active(&c->timer))
			hrtimer_try_to_cancel(&c->timer);
	}

	drm_calc_timestamping_constants(crtc, &crtc->mode);

	spin_lock_irqsave(&c->interval_lock, flags);
	c->interval = ns_to_ktime(vblank->framedur_ns);
	spin_unlock_irqrestore(&c->interval_lock, flags);

	hrtimer_start(&c->timer, c->interval, HRTIMER_MODE_REL);
	return 0;
}

/*
 * hrtimer_cancel() could deadlock against the vblank locks: stop the timer
 * at its next expiry instead.
 */
void mct_compat_disable_vblank(struct drm_crtc *crtc)
{
	struct mct_compat *c = mct_compat_of(crtc);
	unsigned long flags;

	spin_lock_irqsave(&c->interval_lock, flags);
	c->interval = 0;
	spin_unlock_irqrestore(&c->interval_lock, flags);

	hrtimer_try_to_cancel(&c->timer);
}

bool mct_compat_get_vblank_timestamp(struct drm_crtc *crtc, int *max_error,
				     ktime_t *vblank_time, bool in_vblank_irq)
{
	struct mct_compat *c = mct_compat_of(crtc);
	struct drm_vblank_crtc *vblank = drm_crtc_vblank_crtc(crtc);
	u64 count;
	ktime_t cur_time;

	if (!READ_ONCE(vblank->enabled)) {
		*vblank_time = ktime_get();
		return true;
	}

	/* the expiry and the count can change under us: read until they agree */
	do {
		count = drm_crtc_vblank_count_and_time(crtc, &cur_time);
		*vblank_time = READ_ONCE(c->timer.node.expires);
	} while (count != drm_crtc_vblank_count_and_time(crtc, &cur_time));

	if (!ktime_compare(*vblank_time, cur_time))
		return true;

	/* the timer was already forwarded by one frame when the vblank ran */
	*vblank_time = ktime_sub(*vblank_time, c->interval);
	return true;
}

void mct_compat_vblank_atomic_flush(struct drm_crtc *crtc, struct drm_atomic_commit *state)
{
	struct drm_device *dev = crtc->dev;
	struct drm_crtc_state *crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	struct drm_pending_vblank_event *event;

	spin_lock_irq(&dev->event_lock);
	event = crtc_state->event;
	crtc_state->event = NULL;
	if (event) {
		if (drm_crtc_vblank_get(crtc) == 0)
			drm_crtc_arm_vblank_event(crtc, event);
		else
			drm_crtc_send_vblank_event(crtc, event);
	}
	spin_unlock_irq(&dev->event_lock);
}
#endif

#ifdef MCT_COMPAT_NO_DMA_DEV
static void mct_compat_put_dma_dev(struct drm_device *dev, void *data)
{
	put_device(to_mct(dev)->compat.dma_dev);
}

void mct_compat_set_dma_dev(struct drm_device *dev, struct device *dma_dev)
{
	struct mct_compat *c = &to_mct(dev)->compat;

	if (c->dma_dev)
		return;
	c->dma_dev = get_device(dma_dev);
	if (drmm_add_action_or_reset(dev, mct_compat_put_dma_dev, NULL))
		c->dma_dev = NULL;
}

struct drm_gem_object *mct_compat_gem_prime_import(struct drm_device *dev,
						   struct dma_buf *dma_buf)
{
	struct mct_compat *c = &to_mct(dev)->compat;

	/* without a DMA device, the default import, as on newer kernels */
	if (!c->dma_dev)
		return drm_gem_prime_import(dev, dma_buf);
	return drm_gem_prime_import_dev(dev, dma_buf, c->dma_dev);
}
#endif
