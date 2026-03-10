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

/* Card context group - manages device class and region for a group (e.g., px4, pxmlt5) */
struct px4_card_context_group {
	struct kref kref;
	struct mutex lock;
	char devname[64];
	struct class *class;
	dev_t dev_base;
	unsigned int max_num;
	unsigned int minor_num;
	u8 *minor_table;
	unsigned int last_id;
};

/* Individual card device context */
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
	struct px4_card_context_group *parent;
	struct kref *owner_kref;
	void (*owner_kref_release)(struct kref *);
};

/* Context group management */
int px4_card_context_create(const char *name, const char *devname,
			    unsigned int max_num,
			    struct px4_card_context_group **card_ctx_group);
void px4_card_context_destroy(struct px4_card_context_group *card_ctx_group);

/* Card device registration/unregistration */
int px4_card_register(struct px4_card_context *card_ctx,
		      struct device *dev,
		      struct px4_card_context_group *ctx_group,
		      struct it930x_bridge *it930x,
		      struct kref *owner_kref,
		      void (*owner_kref_release)(struct kref *));

void px4_card_unregister(struct px4_card_context *card_ctx);

#endif /* __PX4_CARD_H__ */
