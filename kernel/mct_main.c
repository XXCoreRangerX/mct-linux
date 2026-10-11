// SPDX-License-Identifier: GPL-2.0-only
/*
 * DRM driver for USB displays built on MCT (Magic Control Technology)
 * Trigger chips. One USB driver matches all supported devices and hands
 * each to the backend for its chip generation:
 *  - mct_t2.c: Trigger II, e.g. the Wacom DTU-1031X pen display
 *  - mct_t6.c: Trigger 6, e.g. the "T6 USB Station" HDMI adapter
 * The backends share the DRM core in mct_drv.c but keep their own DRM
 * driver names, modes and module parameters, and can be left out at build
 * time (MCT_T2=n / MCT_T6=n).
 */

#include <linux/module.h>
#include <linux/usb.h>

#include "mct.h"

#define MCT_BACKEND(b)	.driver_info = (kernel_ulong_t)&(b)

static const struct usb_device_id mct_id_table[] = {
#ifdef MCT_HAVE_T2
	/* Wacom DTU-1031X; it has been seen enumerating with either ID */
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5209, 0), MCT_BACKEND(mct_t2_backend) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x520d, 0), MCT_BACKEND(mct_t2_backend) },
#endif
#ifdef MCT_HAVE_T6
	/* T6 USB Station (5600) and the 5601 seen in the vendor's capture */
	{ USB_DEVICE_AND_INTERFACE_INFO(0x0711, 0x5600, 0xff, 0x00, 0x00),
	  MCT_BACKEND(mct_t6_backend) },
	{ USB_DEVICE_AND_INTERFACE_INFO(0x0711, 0x5601, 0xff, 0x00, 0x00),
	  MCT_BACKEND(mct_t6_backend) },
#endif
	{}
};
MODULE_DEVICE_TABLE(usb, mct_id_table);

static const struct mct_backend *mct_backend(struct usb_interface *intf)
{
	const struct usb_device_id *id = usb_match_id(intf, mct_id_table);

	return id ? (const struct mct_backend *)id->driver_info : NULL;
}

static int mct_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	const struct mct_backend *b = (const struct mct_backend *)id->driver_info;

	return b->probe(intf, id);
}

static void mct_disconnect(struct usb_interface *intf)
{
	const struct mct_backend *b = mct_backend(intf);

	if (b)
		b->disconnect(intf);
}

static int mct_suspend(struct usb_interface *intf, pm_message_t message)
{
	const struct mct_backend *b = mct_backend(intf);

	return b ? b->suspend(intf, message) : 0;
}

static int mct_resume(struct usb_interface *intf)
{
	const struct mct_backend *b = mct_backend(intf);

	return b ? b->resume(intf) : 0;
}

static struct usb_driver mct_usb_driver = {
	.name = "mct",
	.probe = mct_probe,
	.disconnect = mct_disconnect,
	.id_table = mct_id_table,
	/*
	 * Keep URBs alive on a software unbind (sysfs, rmmod): disconnect waits
	 * for the frame in flight, so a T2's firmware is never left halfway
	 * through a command's payload, and T6 transfers drain in order.
	 */
	.soft_unbind = 1,
	.suspend = pm_ptr(mct_suspend),
	.resume = pm_ptr(mct_resume),
	.reset_resume = pm_ptr(mct_resume),
};

module_usb_driver(mct_usb_driver);
MODULE_DESCRIPTION("DRM driver for MCT Trigger USB displays (Trigger II, Trigger 6)");
MODULE_LICENSE("GPL");
