// SPDX-License-Identifier: GPL-2.0-only
/*
 * PX4 Smart Card device definitions (px4_card.h)
 *
 * Copyright (c) 2026
 */

#ifndef __PX4_CARD_H__
#define __PX4_CARD_H__

#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/kref.h>

#include "it930x.h"
#include "px4_card_ioctl.h"

/* Card device context */
struct px4_card_context {
	struct mutex lock;
	atomic_t open;
	unsigned int id;
	char name[32];
	struct device *dev;
	struct cdev cdev;
	struct device *device;
	struct it930x_bridge *it930x;
	wait_queue_head_t read_wq;
	bool card_present;
	struct kref *owner_kref;
	void (*owner_kref_release)(struct kref *);
};

/* Device management */
int px4_card_init_dev_node(const char *devname);
void px4_card_term_dev_node(void);

/* Card device registration/unregistration */
int px4_card_register(struct px4_card_context *card_ctx,
		      struct device *dev,
		      const char *devname,
		      struct it930x_bridge *it930x,
		      struct kref *owner_kref,
		      void (*owner_kref_release)(struct kref *));

void px4_card_unregister(struct px4_card_context *card_ctx);

#endif /* __PX4_CARD_H__ */
