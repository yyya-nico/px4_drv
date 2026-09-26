// SPDX-License-Identifier: GPL-2.0-only
/*
 * PTX driver definitions for XIT XIT-SQR100 device (xit_sqr100_device.h)
 *
 * The XIT-SQR100 is an ISDB-T/S receiver (USB VID 0x06B8, PID 0x106B)
 * built on the ITE IT9303FN USB bridge, the Sony CXD2856ER demodulator
 * and the Sony CXD6866AER tuner.
 *
 * NOTE: The CXD6866AER register map, the IT9303FN GPIO power/LNB pin
 * assignments, and the input port / I2C bus / I2C address for this board
 * have NOT been verified against a datasheet or real hardware.  The values
 * used in xit_sqr100_device.c are placeholders derived from the closest
 * analog in this repository (pxmlt_device.c for the demod pairing and
 * LNB/streaming management, s1ur_device.c for the single-tuner structure)
 * and MUST be verified on the target board before this driver is
 * considered functional.  See the TODO markers in xit_sqr100_device.c.
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
	bool lnb_power;
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
	unsigned int lnb_power_count;
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
