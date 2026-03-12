// SPDX-License-Identifier: GPL-2.0-only
/*
 * IOCTL definitions for PX4 Smart Card device (px4_card_ioctl.h)
 *
 * Copyright (c) 2026
 */

#ifndef __PX4_CARD_IOCTL_H__
#define __PX4_CARD_IOCTL_H__

#ifdef __linux__
#ifndef __KERNEL__
#include <sys/ioctl.h>
#endif
#elif defined(_WIN32) || defined(_WIN64)
// Windows support can be added later
#endif

#define PX4CARD_IOC_MAGIC 'C'

/* IOCTL commands */
#define PX4CARD_RESET       _IO(PX4CARD_IOC_MAGIC, 1)
#define PX4CARD_GET_ATR     _IOR(PX4CARD_IOC_MAGIC, 2, struct px4_card_atr)
#define PX4CARD_SET_BAUDRATE _IOW(PX4CARD_IOC_MAGIC, 3, int)
#define PX4CARD_DETECT      _IOR(PX4CARD_IOC_MAGIC, 4, int)
#define PX4CARD_READ_READY  _IOR(PX4CARD_IOC_MAGIC, 5, int)
#define PX4CARD_READ        _IOR(PX4CARD_IOC_MAGIC, 6, struct px4_card_data)
#define PX4CARD_WRITE       _IOW(PX4CARD_IOC_MAGIC, 7, struct px4_card_data)

/* ATR (Answer To Reset) structure - max 33 bytes per ISO/IEC 7816-3 */
struct px4_card_atr {
	unsigned char data[33];
	unsigned int length;
};

/* Data structure for card read/write operations */
struct px4_card_data {
	unsigned char buffer[256];
	unsigned char length;
};

/* Baudrate values */
#define PX4CARD_BAUDRATE_9600   9600
#define PX4CARD_BAUDRATE_19200  19200

#endif /* __PX4_CARD_IOCTL_H__ */
