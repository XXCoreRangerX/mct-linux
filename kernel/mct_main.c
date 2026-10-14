// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Filip Piękoś <mail@filippiekos.com>
 *
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
	/* Wacom DTU-1031X, which enumerates with either ID */
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5209, 0), MCT_BACKEND(mct_t2_backend) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x520d, 0), MCT_BACKEND(mct_t2_backend) },
	/*
	 * Other DTU-1031X units (Linux Hardware Database; untested). Plain T2
	 * adapters use these IDs too: mct_t2 checks for the Wacom panel first.
	 */
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x5201, 0), MCT_BACKEND(mct_t2_backend) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x0711, 0x520f, 0), MCT_BACKEND(mct_t2_backend) },
#endif
#ifdef MCT_HAVE_T6
	/* T6 USB Station and similar single-output T6 adapters */
	{ USB_DEVICE_AND_INTERFACE_INFO(0x0711, 0x5600, 0xff, 0x00, 0x00),
	  MCT_BACKEND(mct_t6_backend) },
	{ USB_DEVICE_AND_INTERFACE_INFO(0x0711, 0x5601, 0xff, 0x00, 0x00),
	  MCT_BACKEND(mct_t6_backend) },
	/*
	 * Untested T6 devices, driven only with mct.untested_ids=1: the
	 * firmware image sets the PID (it changes with reflashing), so these
	 * are IDs seen in MCT's firmware images, the Linux Hardware Database
	 * and other drivers (docs/history/roadmap-done.md): j5create,
	 * StarTech, Mobile Pixels (560b), Insignia/Platinum (19ff:560b-560e),
	 * HP Engage (03f0:0182), the Wacom DTU-1031AX (0711:5609, and its own
	 * Wacom ID on the vendor interface).
	 */
	{ USB_DEVICE_AND_INTERFACE_INFO(0x0711, 0x5602, 0xff, 0x00, 0x00),
	  MCT_BACKEND(mct_t6_backend) },
	{ USB_DEVICE_AND_INTERFACE_INFO(0x0711, 0x5603, 0xff, 0x00, 0x00),
	  MCT_BACKEND(mct_t6_backend) },
	{ USB_DEVICE_AND_INTERFACE_INFO(0x0711, 0x5604, 0xff, 0x00, 0x00),
	  MCT_BACKEND(mct_t6_backend) },
	{ USB_DEVICE_AND_INTERFACE_INFO(0x0711, 0x5605, 0xff, 0x00, 0x00),
	  MCT_BACKEND(mct_t6_backend) },
	{ USB_DEVICE_AND_INTERFACE_INFO(0x0711, 0x5606, 0xff, 0x00, 0x00),
	  MCT_BACKEND(mct_t6_backend) },
	{ USB_DEVICE_AND_INTERFACE_INFO(0x0711, 0x5607, 0xff, 0x00, 0x00),
	  MCT_BACKEND(mct_t6_backend) },
	{ USB_DEVICE_AND_INTERFACE_INFO(0x0711, 0x5608, 0xff, 0x00, 0x00),
	  MCT_BACKEND(mct_t6_backend) },
	{ USB_DEVICE_AND_INTERFACE_INFO(0x0711, 0x5609, 0xff, 0x00, 0x00),
	  MCT_BACKEND(mct_t6_backend) },
	{ USB_DEVICE_AND_INTERFACE_INFO(0x0711, 0x560a, 0xff, 0x00, 0x00),
	  MCT_BACKEND(mct_t6_backend) },
	{ USB_DEVICE_AND_INTERFACE_INFO(0x0711, 0x560b, 0xff, 0x00, 0x00),
	  MCT_BACKEND(mct_t6_backend) },
	{ USB_DEVICE_AND_INTERFACE_INFO(0x0711, 0x561a, 0xff, 0x00, 0x00),
	  MCT_BACKEND(mct_t6_backend) },
	{ USB_DEVICE_AND_INTERFACE_INFO(0x19ff, 0x560b, 0xff, 0x00, 0x00),
	  MCT_BACKEND(mct_t6_backend) },
	{ USB_DEVICE_AND_INTERFACE_INFO(0x19ff, 0x560c, 0xff, 0x00, 0x00),
	  MCT_BACKEND(mct_t6_backend) },
	{ USB_DEVICE_AND_INTERFACE_INFO(0x19ff, 0x560d, 0xff, 0x00, 0x00),
	  MCT_BACKEND(mct_t6_backend) },
	{ USB_DEVICE_AND_INTERFACE_INFO(0x19ff, 0x560e, 0xff, 0x00, 0x00),
	  MCT_BACKEND(mct_t6_backend) },
	{ USB_DEVICE_AND_INTERFACE_INFO(0x03f0, 0x0182, 0xff, 0x00, 0x00),
	  MCT_BACKEND(mct_t6_backend) },
	{ USB_DEVICE_AND_INTERFACE_INFO(0x056a, 0x039f, 0xff, 0x00, 0x00),
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

	if (!b)
		return -ENODEV;
	return b->probe(intf, id);
}

static void mct_disconnect(struct usb_interface *intf)
{
	const struct mct_backend *b = mct_backend(intf);

	if (b)
		b->disconnect(intf);
}

static int mct_usb_suspend(struct usb_interface *intf, pm_message_t message)
{
	const struct mct_backend *b = mct_backend(intf);

	return b ? b->suspend(intf, message) : 0;
}

static int mct_usb_resume(struct usb_interface *intf)
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
	.suspend = pm_ptr(mct_usb_suspend),
	.resume = pm_ptr(mct_usb_resume),
	.reset_resume = pm_ptr(mct_usb_resume),
	/* each ID needs its backend; IDs added at runtime would have none */
	.no_dynamic_id = 1,
};

module_usb_driver(mct_usb_driver);
MODULE_AUTHOR("Filip Piękoś <mail@filippiekos.com>");
MODULE_DESCRIPTION("DRM driver for MCT Trigger USB displays (Trigger II, Trigger 6)");
MODULE_LICENSE("GPL");
