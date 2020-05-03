/* SPDX-License-Identifier: GPL-2.0 */
/*
 * fl2000_streaming.c
 *
 * Original driver uses default altsetting (#0) of streaming interface, which
 * allows bursts of bulk transfers of 15x1024 bytes on output. But the HW
 * actually works incorrectly here: it uses same endpoint #1 across interfaces
 * 1 and 2, which is not allowed by USB specification: endpoint addresses can be
 * shared only between alternate settings, not interfaces. In order to
 * workaround this we use isochronous transfers instead of bulk. There is a
 * possibility that we still can use bulk transfers with interface 0, but this
 * is yet to be checked.
 *
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2019, Artem Mygaiev
 */

#include "fl2000.h"

int fl2000_framebuffer_get(struct usb_device *usb_dev, void *dest,
		size_t dest_size);

/* Streaming is implemented with a single URB for each frame. USB is configured
 * to send NULL URB automatically after each data URB */
struct fl2000_stream {
	struct urb *urb, *zero_len_urb;
};

static void fl2000_stream_release(struct device *dev, void *res)
{
	/* Noop */
}

static void fl2000_stream_completion(struct urb *urb)
{
	int ret;
	struct usb_device *usb_dev = urb->dev;
	struct fl2000_stream *stream = devres_find(&usb_dev->dev,
			fl2000_stream_release, NULL, NULL);

	/* TODO: handle USB errors in URB status */

	ret = usb_submit_urb(stream->zero_len_urb, GFP_KERNEL);
	if (ret) {
		/* TODO: handle USB errors in ret */
		dev_err(&usb_dev->dev, "Zero length URB error %d", ret);
	}
}

static void fl2000_stream_zero_len_completion(struct urb *urb)
{
	int ret;
	struct usb_device *usb_dev = urb->dev;
	struct fl2000_stream *stream = devres_find(&usb_dev->dev,
			fl2000_stream_release, NULL, NULL);

	/* TODO: handle USB errors in URB status */

	fl2000_framebuffer_get(usb_dev, urb->transfer_buffer,
			urb->transfer_buffer_length);

	ret = usb_submit_urb(stream->urb, GFP_KERNEL);
	if (ret) {
		/* TODO: handle USB errors in ret */
		dev_err(&usb_dev->dev, "Data URB error %d", ret);
	}
}

int fl2000_stream_mode_set(struct usb_device *usb_dev, ssize_t size)
{
	u8 *buf;
	dma_addr_t buf_addr;
	struct fl2000_stream *stream = devres_find(&usb_dev->dev,
			fl2000_stream_release, NULL, NULL);
	struct urb *urb;

	if (!stream)
		return -ENODEV;

	urb = stream->urb;

	/* Destroy existing data buffer */
	if (urb->transfer_buffer)
		usb_free_coherent(usb_dev, urb->transfer_buffer_length,
				urb->transfer_buffer, urb->transfer_dma);

	buf = usb_alloc_coherent(usb_dev, size, GFP_KERNEL, &buf_addr);
	if (!buf) {
		dev_err(&usb_dev->dev, "Allocate stream FB buffer failed");
		return -ENOMEM;
	}

	urb->transfer_buffer = buf;
	urb->transfer_dma = buf_addr;

	return 0;
}

int fl2000_stream_enable(struct usb_device *usb_dev)
{
	int ret = 0;
	struct fl2000_stream *stream = devres_find(&usb_dev->dev,
			fl2000_stream_release, NULL, NULL);

	if (!stream)
		return -ENODEV;

	ret = usb_submit_urb(stream->urb, GFP_KERNEL);
	if (ret) {
		/* TODO: handle USB errors in ret */
		dev_err(&usb_dev->dev, "Data URB error %d", ret);
	}

	return ret;
}

void fl2000_stream_disable(struct usb_device *usb_dev)
{
	struct fl2000_stream *stream = devres_find(&usb_dev->dev,
			fl2000_stream_release, NULL, NULL);

	if (!stream)
		return;

	usb_kill_urb(stream->urb);
	usb_kill_urb(stream->zero_len_urb);
}

/**
 * fl2000_stream_create() - streaming processing context creation
 * @interface:	streaming transfers interface
 *
 * This function is called only on Streaming interface probe
 *
 * It shall not initiate any USB transfers. URB is not allocated here because
 * we do not know the stream requirements yet.
 *
 * Return: Operation result
 */
int fl2000_stream_create(struct usb_interface *interface)
{
	int ret = 0;
	struct fl2000_stream *stream;
	struct usb_device *usb_dev = interface_to_usbdev(interface);

	ret = usb_set_interface(usb_dev, 0, 1);
	if (ret) {
		dev_err(&usb_dev->dev, "Cannot set streaming interface " \
				"altstting for bulk transfers");
		return ret;
	}

	stream = devres_alloc(&fl2000_stream_release, sizeof(*stream),
			GFP_KERNEL);
	if (!stream) {
		dev_err(&usb_dev->dev, "Cannot allocate stream");
		return -ENOMEM;
	}
	devres_add(&usb_dev->dev, stream);

	stream->urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!stream->urb) {
		dev_err(&usb_dev->dev, "Allocate data URB failed");
		return -ENOMEM;
	}

	usb_fill_bulk_urb(stream->urb, usb_dev,
			usb_sndbulkpipe(usb_dev, 1),
			NULL, 0,
			fl2000_stream_completion, NULL);

	stream->zero_len_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!stream->zero_len_urb) {
		dev_err(&usb_dev->dev, "Allocate zero length URB failed");
		return -ENOMEM;
	}

	usb_fill_bulk_urb(stream->zero_len_urb, usb_dev,
			usb_sndbulkpipe(usb_dev, 1),
			NULL, 0,
			fl2000_stream_zero_len_completion, NULL);

	dev_info(&usb_dev->dev, "Streaming interface up");

	return ret;
}

void fl2000_stream_destroy(struct usb_interface *interface)
{
	struct usb_device *usb_dev = interface_to_usbdev(interface);
	struct fl2000_stream *stream = devres_find(&usb_dev->dev,
			fl2000_stream_release, NULL, NULL);
	struct urb *urb;

	if (stream) {
		urb = stream->urb;

		/* Destroy existing data buffer */
		if (urb && urb->transfer_buffer)
			usb_free_coherent(usb_dev, urb->transfer_buffer_length,
					urb->transfer_buffer, urb->transfer_dma);

		usb_free_urb(stream->urb);
		usb_free_urb(stream->zero_len_urb);
	}

	devres_release(&usb_dev->dev, fl2000_stream_release, NULL, NULL);
}
