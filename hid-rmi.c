/*
 *  Copyright (c) 2013 Andrew Duggan <aduggan@synaptics.com>
 *  Copyright (c) 2013 Synaptics Incorporated
 *  Copyright (c) 2014 Benjamin Tissoires <benjamin.tissoires@gmail.com>
 *  Copyright (c) 2014 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include <linux/kernel.h>
#include <linux/hid.h>
#include <linux/module.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include "hid-ids.h"

#define RMI_WRITE_REPORT_ID		0x9 /* Output Report */
#define RMI_READ_ADDR_REPORT_ID		0xa /* Output Report */
#define RMI_READ_DATA_REPORT_ID		0xb /* Input Report */
#define RMI_ATTN_REPORT_ID		0xc /* Input Report */
#define RMI_SET_RMI_MODE_REPORT_ID	0xf /* Feature Report */

/* flags */
#define RMI_READ_REQUEST_PENDING	BIT(0)
#define RMI_READ_DATA_PENDING		BIT(1)
#define RMI_STARTED			BIT(2)

enum rmi_mode_type {
	RMI_MODE_OFF 			= 0,
	RMI_MODE_ATTN_REPORTS		= 1,
	RMI_MODE_NO_PACKED_ATTN_REPORTS	= 2,
};

/**
 * struct rmi_data - stores information for hid communication
 *
 * @page_mutex: Locks current page to avoid changing pages in unexpected ways.
 * @page: Keeps track of the current virtual page
 *
 * @wait: Used for waiting for read data
 *
 * @writeReport: output buffer when writing RMI registers
 * @readReport: input buffer when reading RMI registers
 *
 * @input_report_size: size of an input report (advertised by HID)
 * @output_report_size: size of an output report (advertised by HID)
 *
 * @flags: flags for the current device (started, reading, etc...)
 */
struct rmi_data {
	struct mutex page_mutex;
	int page;

	wait_queue_head_t wait;

	u8 *writeReport;
	u8 *readReport;

	int input_report_size;
	int output_report_size;

	unsigned long flags;
};

#define RMI_PAGE(addr) (((addr) >> 8) & 0xff)

static int rmi_write_report(struct hid_device *hdev, u8 *report, int len);

/**
 * rmi_set_page - Set RMI page
 * @hdev: The pointer to the hid_device struct
 * @page: The new page address.
 *
 * RMI devices have 16-bit addressing, but some of the physical
 * implementations (like SMBus) only have 8-bit addressing. So RMI implements
 * a page address at 0xff of every page so we can reliable page addresses
 * every 256 registers.
 *
 * The page_mutex lock must be held when this function is entered.
 *
 * Returns zero on success, non-zero on failure.
 */
static int rmi_set_page(struct hid_device *hdev, u8 page)
{
	struct rmi_data *data = hid_get_drvdata(hdev);
	int retval;

	data->writeReport[0] = RMI_WRITE_REPORT_ID;
	data->writeReport[1] = 1;
	data->writeReport[2] = 0xFF;
	data->writeReport[4] = page;

	retval = rmi_write_report(hdev, data->writeReport,
			data->output_report_size);
	if (retval != data->output_report_size) {
		dev_err(&hdev->dev,
			"%s: set page failed: %d.", __func__, retval);
		return retval;
	}

	data->page = page;
	return 0;
}

static int rmi_set_mode(struct hid_device *hdev, u8 mode)
{
	int ret;
	u8 txbuf[2] = {RMI_SET_RMI_MODE_REPORT_ID, mode};

	ret = hid_hw_raw_request(hdev, RMI_SET_RMI_MODE_REPORT_ID, txbuf,
			sizeof(txbuf), HID_FEATURE_REPORT, HID_REQ_SET_REPORT);
	if (ret < 0) {
		dev_err(&hdev->dev, "unable to set rmi mode to %d (%d)\n", mode,
			ret);
		return ret;
	}

	return 0;
}

static int rmi_write_report(struct hid_device *hdev, u8 * report, int len)
{
	int ret;

	ret = hid_hw_output_report(hdev, (void *)report, len);
	if (ret < 0) {
		dev_err(&hdev->dev, "failed to write hid report (%d)\n", ret);
		return ret;
	}

	return ret;
}

static int rmi_read_block(struct hid_device *hdev, u16 addr, void *buf,
		const int len)
{
	struct rmi_data *data = hid_get_drvdata(hdev);
	int ret;
	int bytes_read;
	int bytes_needed;
	int retries;
	int read_input_count;

	mutex_lock(&data->page_mutex);

	if (RMI_PAGE(addr) != data->page) {
		ret = rmi_set_page(hdev, RMI_PAGE(addr));
		if (ret < 0)
			goto exit;
	}

	for (retries = 5; retries > 0; retries--) {
		data->writeReport[0] = RMI_READ_ADDR_REPORT_ID;
		data->writeReport[1] = 0; /* old 1 byte read count */
		data->writeReport[2] = addr & 0xFF;
		data->writeReport[3] = (addr >> 8) & 0xFF;
		data->writeReport[4] = len  & 0xFF;
		data->writeReport[5] = (len >> 8) & 0xFF;

		set_bit(RMI_READ_REQUEST_PENDING, &data->flags);

		ret = rmi_write_report(hdev, data->writeReport,
						data->output_report_size);
		if (ret != data->output_report_size) {
			clear_bit(RMI_READ_REQUEST_PENDING, &data->flags);
			dev_err(&hdev->dev,
				"failed to write request output report (%d)\n",
				ret);
			goto exit;
		}

		bytes_read = 0;
		bytes_needed = len;
		while (bytes_read < len) {
			if (!wait_event_timeout(data->wait, 
				test_bit(RMI_READ_DATA_PENDING, &data->flags),
					msecs_to_jiffies(1000))) {
				hid_warn(hdev, "%s: timeout elapsed\n",
					 __func__);
				ret = -EAGAIN;
				break;
			}

			read_input_count = data->readReport[1];
			memcpy(buf + bytes_read, &data->readReport[2],
				read_input_count < bytes_needed ?
					read_input_count : bytes_needed);

			bytes_read += read_input_count;
			bytes_needed -= read_input_count;
			clear_bit(RMI_READ_DATA_PENDING, &data->flags);
		}

		if (ret >= 0) {
			ret = bytes_read;
			break;
		}
	}

exit:
	clear_bit(RMI_READ_REQUEST_PENDING, &data->flags);
	mutex_unlock(&data->page_mutex);
	return ret;
}

static int rmi_read_data_event(struct hid_device *hdev, u8 *data, int size)
{
	struct rmi_data *hdata = hid_get_drvdata(hdev);

	if (!test_bit(RMI_READ_REQUEST_PENDING, &hdata->flags)) {
		hid_err(hdev, "no read request pending\n");
		return 0;
	}

	memcpy(hdata->readReport, data, size < hdata->input_report_size ?
			size : hdata->input_report_size);
	set_bit(RMI_READ_DATA_PENDING, &hdata->flags);
	wake_up(&hdata->wait);

	return 1;
}

static int rmi_raw_event(struct hid_device *hdev,
		struct hid_report *report, u8 *data, int size)
{
	switch (data[0]) {
	case RMI_READ_DATA_REPORT_ID:
		return rmi_read_data_event(hdev, data, size);
	}

	return 0;
}

static int rmi_post_reset(struct hid_device *hdev)
{
	return rmi_set_mode(hdev, RMI_MODE_ATTN_REPORTS);
}

static int rmi_post_resume(struct hid_device *hdev)
{
	return rmi_set_mode(hdev, RMI_MODE_ATTN_REPORTS);
}

static int rmi_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct rmi_data *data = NULL;
	int ret;
	size_t alloc_size;

	data = devm_kzalloc(&hdev->dev, sizeof(struct rmi_data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	hid_set_drvdata(hdev, data);

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "parse failed\n");
		return ret;
	}

	data->input_report_size = (hdev->report_enum[HID_INPUT_REPORT]
		.report_id_hash[RMI_ATTN_REPORT_ID]->size >> 3)
		+ 1 /* report id */;
	data->output_report_size = (hdev->report_enum[HID_OUTPUT_REPORT]
		.report_id_hash[RMI_WRITE_REPORT_ID]->size >> 3)
		+ 1 /* report id */;

	alloc_size = data->output_report_size + data->input_report_size;

	data->writeReport = devm_kzalloc(&hdev->dev, alloc_size, GFP_KERNEL);
	if (!data->writeReport) {
		ret = -ENOMEM;
		return ret;
	}

	data->readReport = data->writeReport + data->output_report_size;

	init_waitqueue_head(&data->wait);

	mutex_init(&data->page_mutex);

	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (ret) {
		hid_err(hdev, "hw start failed\n");
		return ret;
	}

	hid_dbg(hdev, "Opening low level driver\n");
	if (!hid_hw_open(hdev)) {
		ret = -EIO;
		goto err;
	}

	/* Allow incoming hid reports */
	hid_device_io_start(hdev);

	ret = rmi_set_mode(hdev, RMI_MODE_ATTN_REPORTS);
	if (ret < 0) {
		hid_err(hdev, "failed to set rmi mode\n");
		goto err;
	}

	ret = rmi_set_page(hdev, 0);
	if (ret < 0) {
		hid_err(hdev, "failed to set page select to 0.\n");
		goto err;
	}

	return 0;
err:
	hid_hw_close(hdev);
	return ret;
}

static void rmi_remove(struct hid_device *hdev)
{
	struct rmi_data *hdata = hid_get_drvdata(hdev);

	clear_bit(RMI_STARTED, &hdata->flags);

	/* FIXME: do not keep the device opened */
	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static const struct hid_device_id rmi_id[] = {
	{ HID_I2C_DEVICE(USB_VENDOR_ID_SYNAPTICS, HID_ANY_ID) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_SYNAPTICS, HID_ANY_ID) },
	{ }
};
MODULE_DEVICE_TABLE(hid, rmi_id);

static struct hid_driver rmi_driver = {
	.name = "rmi_hid",
	.driver = {
		.owner	= THIS_MODULE,
		.name	= "rmi_hid",
	},
	.id_table		= rmi_id,
	.probe			= rmi_probe,
	.remove			= rmi_remove,
	.raw_event		= rmi_raw_event,
#ifdef CONFIG_PM
	.resume			= rmi_post_resume,
	.reset_resume		= rmi_post_reset,
#endif
};

module_hid_driver(rmi_driver);

MODULE_AUTHOR("Andrew Duggan <aduggan@synaptics.com>");
MODULE_DESCRIPTION("RMI HID driver");
MODULE_LICENSE("GPL");
