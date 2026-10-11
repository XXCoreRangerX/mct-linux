/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * One USB driver for MCT Trigger display adapters; each chip generation is
 * a backend with its own DRM driver.
 */
#ifndef MCT_H
#define MCT_H

#include <linux/usb.h>

struct mct_backend {
	int (*probe)(struct usb_interface *intf, const struct usb_device_id *id);
	void (*disconnect)(struct usb_interface *intf);
	int (*suspend)(struct usb_interface *intf, pm_message_t message);
	int (*resume)(struct usb_interface *intf);
};

extern const struct mct_backend mct_t2_backend;	/* Trigger II, mct_t2.c */
extern const struct mct_backend mct_t6_backend;	/* Trigger 6, mct_t6.c */

#endif
