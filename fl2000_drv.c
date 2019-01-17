/* SPDX-License-Identifier: GPL-2.0 */
/*
 * fl2000_module.c
 *
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018, Artem Mygaiev
 */

#include "fl2000.h"

#define USB_DRIVER_NAME			"fl2000_usb"

#define USB_CLASS_AV 			0x10
#define USB_SUBCLASS_AV_CONTROL		0x01
#define USB_SUBCLASS_AV_VIDEO		0x02
#define USB_SUBCLASS_AV_AUDIO		0x03

#define USB_VENDOR_ID_FRESCO_LOGIC	0x1D5C
#define USB_PRODUCT_ID_FL2000		0x2000

#define FL2000_USBIF_AVCONTROL		0
#define FL2000_USBIF_STREAMING		1
#define FL2000_USBIF_INTERRUPT		2

/* I2C adapter interface creation */
int fl2000_i2c_create(struct usb_device *usb_dev);

/* Register map creation */
int fl2000_regmap_create(struct usb_device *usb_dev);

/* DRM device creation */
int fl2000_drm_create(struct usb_device *usb_dev);

/* Private data on USB interrupt interface */
int fl2000_intr_create(struct usb_interface *interface);
void fl2000_intr_destroy(struct usb_interface *interface);

static struct usb_device_id fl2000_id_table[] = {
	{ USB_DEVICE_INTERFACE_CLASS(USB_VENDOR_ID_FRESCO_LOGIC, \
		USB_PRODUCT_ID_FL2000, USB_CLASS_AV) },
	{},
};
MODULE_DEVICE_TABLE(usb, fl2000_id_table);

struct usb_dev_data {
	struct regmap_field *app_reset;
	struct regmap_field *wakeup_clear_en;
};

static void fl2000_usb_dev_data_release(struct device *dev, void *res)
{
}

static int fl2000_init(struct usb_device *usb_dev)
{
	int ret;
	struct usb_dev_data *usb_dev_data;
	struct regmap *regmap;

	/* Create local data structure */
	usb_dev_data = devres_alloc(fl2000_usb_dev_data_release,
			sizeof(*usb_dev_data), GFP_KERNEL);
	if (!usb_dev_data) {
		dev_err(&usb_dev->dev, "USB data allocation failed");
		return -ENOMEM;
	}
	devres_add(&usb_dev->dev, usb_dev_data);

	/* Create registers map */
	ret = fl2000_regmap_create(usb_dev);
	if (ret != 0) {
		dev_err(&usb_dev->dev, "Cannot create registers map (%d)", ret);
		return ret;
	}

	/* Regmap must exist on initialization */
	regmap = dev_get_regmap(&usb_dev->dev, NULL);
	if (!regmap) {
		dev_err(&usb_dev->dev, "Regmap capture failed");
		return -ENOMEM;
	}

	regmap_write_bits(regmap, FL2000_USB_LPM, (3<<19), (3<<19));
	regmap_write_bits(regmap, FL2000_USB_LPM, (3<<20), (3<<20));

	/* Create control fields */
	usb_dev_data->app_reset = devm_regmap_field_alloc(&usb_dev->dev,
			regmap, FL2000_RST_CTRL_REG_app_reset);
	if (IS_ERR(usb_dev_data->app_reset))
		return PTR_ERR(usb_dev_data->app_reset);

	usb_dev_data->wakeup_clear_en = devm_regmap_field_alloc(&usb_dev->dev,
			regmap, FL2000_VGA_CTRL_REG_3_wakeup_clear_en);
	if (IS_ERR(usb_dev_data->wakeup_clear_en))
		return PTR_ERR(usb_dev_data->wakeup_clear_en);

	ret = regmap_field_write(usb_dev_data->app_reset, true);
	if (ret != 0) return -EIO;

	ret = regmap_field_write(usb_dev_data->wakeup_clear_en, false);
	if (ret != 0) return -EIO;

	regmap_write_bits(regmap, FL2000_VGA_CTRL_REG_ACLK, 0x1F, 0x1C);

	/* Create I2C adapter */
	ret = fl2000_i2c_create(usb_dev);
	if (ret != 0) {
		dev_err(&usb_dev->dev, "Cannot create I2C adapter (%d)", ret);
		return ret;
	}

	/* Create DRM device */
	ret = fl2000_drm_create(usb_dev);
	if (ret != 0) {
		dev_err(&usb_dev->dev, "Cannot create DRM interface (%d)", ret);
		return ret;
	}

	return 0;
}

static int fl2000_probe(struct usb_interface *interface,
		const struct usb_device_id *usb_dev_id)
{
	int ret;
	u8 iface_num = interface->cur_altsetting->desc.bInterfaceNumber;
	struct usb_device *usb_dev = interface_to_usbdev(interface);

	switch (iface_num) {
	case FL2000_USBIF_AVCONTROL:
		/* This is rather useless, AVControl is not properly implemented
		 * on FL2000 chip - that is why all the "magic" needed */

		ret = fl2000_init(usb_dev);

		break;

	case FL2000_USBIF_STREAMING:
		dev_info(&usb_dev->dev, "Probing Streaming interface (%u)",
				iface_num);

		/* TODO: create streaming structures */
		ret = 0;

		break;

	case FL2000_USBIF_INTERRUPT:
		dev_info(&usb_dev->dev, "Probing Interrupt interface (%u)",
				iface_num);

		/* Initialize interrupt endpoint processing */
		ret = fl2000_intr_create(interface);

		break;

	default:
		/* Device does not have any other interfaces */
		dev_warn(&interface->dev, "What interface %d?",
				interface->cur_altsetting->desc.iInterface);
		ret = -ENODEV;

		break;
	}
	return ret;
}

static void fl2000_disconnect(struct usb_interface *interface)
{
	u8 iface_num = interface->cur_altsetting->desc.bInterfaceNumber;

	struct usb_device *usb_dev = interface_to_usbdev(interface);
	dev_info(&usb_dev->dev, "Disconnecting interface: %u", iface_num);

	switch (iface_num) {
	case FL2000_USBIF_AVCONTROL:
		break;

	case FL2000_USBIF_STREAMING:
		break;

	case FL2000_USBIF_INTERRUPT:
		fl2000_intr_destroy(interface);
		break;

	default:
		/* Device does not have any other interfaces */
		break;
	}
}

static int fl2000_suspend(struct usb_interface *interface,
			   pm_message_t message)
{
	struct usb_device *usb_dev = interface_to_usbdev(interface);
	dev_dbg(&usb_dev->dev, "resume");

	/* TODO: suspend */

	return 0;
}

static int fl2000_resume(struct usb_interface *interface)
{
	struct usb_device *usb_dev = interface_to_usbdev(interface);
	dev_dbg(&usb_dev->dev, "suspend");

	/* TODO: resume */

	return 0;
}

static struct usb_driver fl2000_driver = {
	.name 		= USB_DRIVER_NAME,
	.probe 		= fl2000_probe,
	.disconnect 	= fl2000_disconnect,
	.suspend	= fl2000_suspend,
	.resume		= fl2000_resume,
	.id_table 	= fl2000_id_table,
};

module_usb_driver(fl2000_driver); /* @suppress("Unused static function")
			@suppress("Unused variable declaration in file scope")
			@suppress("Unused function declaration") */

MODULE_AUTHOR("Artem Mygaiev");
MODULE_DESCRIPTION("FL2000 USB display driver");
MODULE_LICENSE("GPL v2");
