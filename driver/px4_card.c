// SPDX-License-Identifier: GPL-2.0-only
/*
 * PX4 Smart Card character device driver (px4_card.c)
 *
 * Copyright (c) 2026
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "print_format.h"
#include "px4_card.h"
#include "it930x.h"

#define PX4CARD_CHRDEV_NUM	16
#define PX4CARD_DEVNAME		"px4card"

static dev_t px4card_dev_first;
static struct class *px4card_class;
static DEFINE_MUTEX(px4card_lock);
static DECLARE_BITMAP(px4card_available, PX4CARD_CHRDEV_NUM);

/* Helper: wait for UART data ready */
static int px4card_wait_data_ready(struct px4_card_context *card_ctx,
				   bool *ready, long timeout_ms)
{
	struct it930x_bridge *it930x = card_ctx->it930x;
	int ret;
	long timeout = msecs_to_jiffies(timeout_ms);
	long remaining;

	remaining = wait_event_interruptible_timeout(
		card_ctx->read_wq,
		({
			ret = it930x_bcas_check_ready(it930x, ready);
			ret == 0 && *ready;
		}),
		timeout
	);

	if (remaining == 0)
		return -ETIMEDOUT;
	else if (remaining < 0)
		return remaining;

	return ret;
}

/* Helper: receive ATR after card reset */
static int px4card_receive_atr(struct px4_card_context *card_ctx,
			       struct px4_card_atr *atr)
{
	struct it930x_bridge *it930x = card_ctx->it930x;
	int ret;
	bool ready;
	u8 len;
	unsigned long start_time;

	atr->length = 0;
	start_time = jiffies;

	/* Wait up to 1 second for ATR (ISO/IEC 7816-3: max 400-40000 clock cycles) */
	while (atr->length < sizeof(atr->data)) {
		/* Check if data is ready */
		ret = it930x_bcas_check_ready(it930x, &ready);
		if (ret)
			return ret;

		if (!ready) {
			if (time_after(jiffies, start_time + HZ)) {
				/* Timeout */
				break;
			}
			msleep(10);
			continue;
		}

		/* Read available data */
		len = sizeof(atr->data) - atr->length;
		ret = it930x_bcas_get_data(it930x, &atr->data[atr->length], &len);
		if (ret)
			return ret;

		atr->length += len;

		/* Basic ATR validation: need at least TS and T0 */
		if (atr->length >= 2) {
			/* Check if we have received the complete ATR
			 * This is a simplistic check - full parsing should be done in userland
			 */
			if (atr->length >= 3)
				break;
		}
	}

	if (atr->length < 2)
		return -ENODATA;

	return 0;
}

/* File operations: open */
static int px4card_fops_open(struct inode *inode, struct file *file)
{
	struct px4_card_context *card_ctx;
	int ret = 0;

	card_ctx = container_of(inode->i_cdev, struct px4_card_context, cdev);

	if (atomic_cmpxchg(&card_ctx->open, 0, 1)) {
		dev_dbg(card_ctx->dev, "px4card_fops_open: device busy\n");
		return -EBUSY;
	}

	kref_get(card_ctx->owner_kref);

	mutex_lock(&card_ctx->lock);

	/* Check if card is present */
	ret = it930x_bcas_detect_card(card_ctx->it930x, &card_ctx->card_present);
	if (ret) {
		dev_err(card_ctx->dev, "px4card_fops_open: failed to detect card. (ret: %d)\n", ret);
		goto exit;
	}

	file->private_data = card_ctx;
	dev_dbg(card_ctx->dev, "px4card_fops_open: device opened\n");

exit:
	if (ret) {
		atomic_set(&card_ctx->open, 0);
		kref_put(card_ctx->owner_kref, card_ctx->owner_kref_release);
	}
	mutex_unlock(&card_ctx->lock);
	return ret;
}

/* File operations: release */
static int px4card_fops_release(struct inode *inode, struct file *file)
{
	struct px4_card_context *card_ctx = file->private_data;

	if (!card_ctx)
		return -EINVAL;

	mutex_lock(&card_ctx->lock);
	atomic_set(&card_ctx->open, 0);
	dev_dbg(card_ctx->dev, "px4card_fops_release: device closed\n");
	mutex_unlock(&card_ctx->lock);

	kref_put(card_ctx->owner_kref, card_ctx->owner_kref_release);

	return 0;
}

/* File operations: read */
static ssize_t px4card_fops_read(struct file *file, char __user *buf,
				 size_t count, loff_t *ppos)
{
	struct px4_card_context *card_ctx = file->private_data;
	struct it930x_bridge *it930x;
	u8 kbuf[256];
	u8 len;
	int ret;
	bool ready;

	if (!card_ctx)
		return -EINVAL;

	it930x = card_ctx->it930x;

	if (count > sizeof(kbuf))
		count = sizeof(kbuf);

	mutex_lock(&card_ctx->lock);

	/* Wait for data with timeout */
	ret = px4card_wait_data_ready(card_ctx, &ready, 1000);
	if (ret) {
		if (ret == -ETIMEDOUT)
			ret = -EAGAIN;
		goto exit;
	}

	/* Read data from UART */
	len = count;
	ret = it930x_bcas_get_data(it930x, kbuf, &len);
	if (ret) {
		dev_err(card_ctx->dev, "px4card_fops_read: failed to get data. (ret: %d)\n", ret);
		goto exit;
	}

	/* Copy to user space */
	if (copy_to_user(buf, kbuf, len)) {
		ret = -EFAULT;
		goto exit;
	}

	ret = len;

exit:
	mutex_unlock(&card_ctx->lock);
	return ret;
}

/* File operations: write */
static ssize_t px4card_fops_write(struct file *file, const char __user *buf,
				  size_t count, loff_t *ppos)
{
	struct px4_card_context *card_ctx = file->private_data;
	struct it930x_bridge *it930x;
	u8 kbuf[256];
	int ret;

	if (!card_ctx)
		return -EINVAL;

	it930x = card_ctx->it930x;

	if (count > sizeof(kbuf))
		return -EINVAL;

	/* Copy from user space */
	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;

	mutex_lock(&card_ctx->lock);

	/* Send data to UART */
	ret = it930x_bcas_send_data(it930x, kbuf, count);
	if (ret) {
		dev_err(card_ctx->dev, "px4card_fops_write: failed to send data. (ret: %d)\n", ret);
		goto exit;
	}

	ret = count;

exit:
	mutex_unlock(&card_ctx->lock);
	return ret;
}

/* File operations: ioctl */
static long px4card_fops_ioctl(struct file *file, unsigned int cmd,
			       unsigned long arg)
{
	struct px4_card_context *card_ctx = file->private_data;
	struct it930x_bridge *it930x;
	int ret = 0;
	void __user *argp = (void __user *)arg;

	if (!card_ctx)
		return -EINVAL;

	it930x = card_ctx->it930x;

	mutex_lock(&card_ctx->lock);

	switch (cmd) {
	case PX4CARD_RESET:
	{
		dev_dbg(card_ctx->dev, "px4card_fops_ioctl: PX4CARD_RESET\n");

		/* Reset card */
		ret = it930x_bcas_reset_card(it930x);
		if (ret) {
			dev_err(card_ctx->dev, "ioctl: failed to reset card. (ret: %d)\n", ret);
			break;
		}

		/* Wait a bit for card to initialize */
		msleep(50);

		break;
	}

	case PX4CARD_GET_ATR:
	{
		struct px4_card_atr atr;

		dev_dbg(card_ctx->dev, "px4card_fops_ioctl: PX4CARD_GET_ATR\n");

		/* Receive ATR */
		ret = px4card_receive_atr(card_ctx, &atr);
		if (ret) {
			dev_err(card_ctx->dev, "ioctl: failed to receive ATR. (ret: %d)\n", ret);
			break;
		}

		/* Copy to user space */
		if (copy_to_user(argp, &atr, sizeof(atr))) {
			ret = -EFAULT;
			break;
		}

		break;
	}

	case PX4CARD_SET_BAUDRATE:
	{
		int baudrate;
		enum it930x_uart_baudrate it930x_baudrate;

		if (copy_from_user(&baudrate, argp, sizeof(baudrate))) {
			ret = -EFAULT;
			break;
		}

		dev_dbg(card_ctx->dev, "px4card_fops_ioctl: PX4CARD_SET_BAUDRATE %d\n", baudrate);

		/* Convert to IT930x baudrate */
		switch (baudrate) {
		case PX4CARD_BAUDRATE_9600:
			it930x_baudrate = IT930X_UART_BAUDRATE_9600;
			break;
		case PX4CARD_BAUDRATE_19200:
			it930x_baudrate = IT930X_UART_BAUDRATE_19200;
			break;
		default:
			ret = -EINVAL;
			break;
		}

		if (ret)
			break;

		/* Set baudrate */
		ret = it930x_bcas_set_baudrate(it930x, it930x_baudrate);
		if (ret) {
			dev_err(card_ctx->dev, "ioctl: failed to set baudrate. (ret: %d)\n", ret);
			break;
		}

		break;
	}

	case PX4CARD_DETECT:
	{
		int detected = 0;
		bool card_detected;

		dev_dbg(card_ctx->dev, "px4card_fops_ioctl: PX4CARD_DETECT\n");

		/* Detect card */
		ret = it930x_bcas_detect_card(it930x, &card_detected);
		if (ret) {
			dev_err(card_ctx->dev, "ioctl: failed to detect card. (ret: %d)\n", ret);
			break;
		}

		detected = card_detected ? 1 : 0;
		card_ctx->card_present = card_detected;

		/* Copy to user space */
		if (copy_to_user(argp, &detected, sizeof(detected))) {
			ret = -EFAULT;
			break;
		}

		break;
	}

	default:
		ret = -ENOTTY;
		break;
	}

	mutex_unlock(&card_ctx->lock);
	return ret;
}

/* File operations: poll */
static unsigned int px4card_fops_poll(struct file *file,
				      struct poll_table_struct *wait)
{
	struct px4_card_context *card_ctx = file->private_data;
	struct it930x_bridge *it930x;
	unsigned int mask = 0;
	bool ready = false;
	int ret;

	if (!card_ctx)
		return POLLERR;

	it930x = card_ctx->it930x;

	poll_wait(file, &card_ctx->read_wq, wait);

	mutex_lock(&card_ctx->lock);

	/* Check if data is available */
	ret = it930x_bcas_check_ready(it930x, &ready);
	if (!ret && ready)
		mask |= POLLIN | POLLRDNORM;

	/* Always writable (for now) */
	mask |= POLLOUT | POLLWRNORM;

	mutex_unlock(&card_ctx->lock);

	return mask;
}

static const struct file_operations px4card_fops = {
	.owner = THIS_MODULE,
	.open = px4card_fops_open,
	.release = px4card_fops_release,
	.read = px4card_fops_read,
	.write = px4card_fops_write,
	.unlocked_ioctl = px4card_fops_ioctl,
	.poll = px4card_fops_poll,
};

/* Initialize device node infrastructure */
int px4_card_init_dev_node(void)
{
	int ret;

	ret = alloc_chrdev_region(&px4card_dev_first, 0, PX4CARD_CHRDEV_NUM, PX4CARD_DEVNAME);
	if (ret) {
		pr_err("px4_card: alloc_chrdev_region() failed. (ret: %d)\n", ret);
		return ret;
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
	px4card_class = class_create(PX4CARD_DEVNAME);
#else
	px4card_class = class_create(THIS_MODULE, PX4CARD_DEVNAME);
#endif
	if (IS_ERR(px4card_class)) {
		pr_err("px4_card: class_create() failed.\n");
		ret = PTR_ERR(px4card_class);
		goto fail_class;
	}

	bitmap_fill(px4card_available, PX4CARD_CHRDEV_NUM);

	pr_info("px4_card: device node infrastructure initialized\n");
	return 0;

fail_class:
	unregister_chrdev_region(px4card_dev_first, PX4CARD_CHRDEV_NUM);
	return ret;
}

/* Terminate device node infrastructure */
void px4_card_term_dev_node(void)
{
	class_destroy(px4card_class);
	unregister_chrdev_region(px4card_dev_first, PX4CARD_CHRDEV_NUM);
	pr_info("px4_card: device node infrastructure terminated\n");
}

/* Register a card device */
int px4_card_register(struct px4_card_context *card_ctx,
		      struct device *dev,
		      const char *devname,
		      struct it930x_bridge *it930x,
		      struct kref *owner_kref,
		      void (*owner_kref_release)(struct kref *))
{
	unsigned int id;
	dev_t devt;
	int ret;
	const char *name_prefix;

	if (!card_ctx || !dev || !it930x || !owner_kref || !owner_kref_release)
		return -EINVAL;

	name_prefix = devname ? devname : PX4CARD_DEVNAME;

	/* Find available device ID */
	mutex_lock(&px4card_lock);
	id = find_first_bit(px4card_available, PX4CARD_CHRDEV_NUM);
	if (id >= PX4CARD_CHRDEV_NUM) {
		mutex_unlock(&px4card_lock);
		dev_err(dev, "px4_card_register: no available device ID\n");
		return -ENOSPC;
	}
	clear_bit(id, px4card_available);
	mutex_unlock(&px4card_lock);

	/* Initialize context */
	memset(card_ctx, 0, sizeof(*card_ctx));
	mutex_init(&card_ctx->lock);
	atomic_set(&card_ctx->open, 0);
	card_ctx->id = id;
	snprintf(card_ctx->name, sizeof(card_ctx->name), "%s%u", name_prefix, id);
	card_ctx->dev = dev;
	card_ctx->it930x = it930x;
	init_waitqueue_head(&card_ctx->read_wq);
	card_ctx->card_present = false;
	card_ctx->owner_kref = owner_kref;
	card_ctx->owner_kref_release = owner_kref_release;

	/* Initialize cdev */
	devt = MKDEV(MAJOR(px4card_dev_first), MINOR(px4card_dev_first) + id);
	cdev_init(&card_ctx->cdev, &px4card_fops);
	card_ctx->cdev.owner = THIS_MODULE;

	ret = cdev_add(&card_ctx->cdev, devt, 1);
	if (ret) {
		dev_err(dev, "px4_card_register: cdev_add() failed. (ret: %d)\n", ret);
		goto fail_cdev;
	}

	/* Create device node */
	card_ctx->device = device_create(px4card_class, dev, devt, NULL, card_ctx->name);
	if (IS_ERR(card_ctx->device)) {
		dev_err(dev, "px4_card_register: device_create() failed.\n");
		ret = PTR_ERR(card_ctx->device);
		goto fail_device;
	}

	dev_info(dev, "px4_card: registered as /dev/%s\n", card_ctx->name);
	return 0;

fail_device:
	cdev_del(&card_ctx->cdev);
fail_cdev:
	mutex_lock(&px4card_lock);
	set_bit(id, px4card_available);
	mutex_unlock(&px4card_lock);
	return ret;
}

/* Unregister a card device */
void px4_card_unregister(struct px4_card_context *card_ctx)
{
	if (!card_ctx)
		return;

	dev_info(card_ctx->dev, "px4_card: unregistering /dev/%s\n", card_ctx->name);

	device_destroy(px4card_class, card_ctx->cdev.dev);
	cdev_del(&card_ctx->cdev);

	mutex_lock(&px4card_lock);
	set_bit(card_ctx->id, px4card_available);
	mutex_unlock(&px4card_lock);
}
