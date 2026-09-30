// SPDX-License-Identifier: GPL-2.0-only
/*
 * PTX driver definitions for XIT XIT-SQR100 device (xit_sqr100_device.h)
 *
 * The XIT-SQR100 is an ISDB-T/S receiver (USB VID 0x06B8, PID 0x106B)
 * built on the ITE IT9303FN USB bridge, the Sony CXD2856ER demodulator
 * and the Sony CXD6866AER tuner.
 *
 * The manufacturer specifies that the XIT-SQR100 does not supply power to
 * BS/CS antennas, so this device does not expose LNB voltage control.  The
 * IT9303FN board power GPIOs and input port / I2C bus / address are still
 * provisional values derived from related devices and need confirmation.
 */

#ifndef __XITSQR100_DEVICE_H__
#define __XITSQR100_DEVICE_H__

#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/kref.h>
#include <linux/mutex.h>
#include <linux/completion.h>
#include <linux/device.h>

#include "ptx_chrdev.h"
#include "it930x.h"
#include "cxd2856er.h"
#include "cxd6866.h"

#define XITSQR100_CHRDEV_NUM	1

struct xit_sqr100_device;

struct xit_sqr100_chrdev {
	struct ptx_chrdev *chrdev;
	struct xit_sqr100_device *parent;
	struct mutex *tuner_lock;
	struct cxd2856er_demod cxd2856er;
	struct cxd6866_tuner cxd6866;
};

struct xit_sqr100_device {
	struct mutex lock;
	struct kref kref;
	atomic_t available;
	struct device *dev;
	struct completion *quit_completion;
	unsigned int open_count;
	unsigned int streaming_count;
	struct mutex tuner_lock;
	struct ptx_chrdev_group *chrdev_group;
	struct xit_sqr100_chrdev chrdevs;
	struct it930x_bridge it930x;
	void *stream_ctx;
};

int xit_sqr100_device_init(struct xit_sqr100_device *xit, struct device *dev,
			   struct ptx_chrdev_context *chrdev_ctx,
			   struct completion *quit_completion);
void xit_sqr100_device_term(struct xit_sqr100_device *xit);

#endif
