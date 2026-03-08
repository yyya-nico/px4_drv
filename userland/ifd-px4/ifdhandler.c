// SPDX-License-Identifier: GPL-2.0-only
/*
 * PX4 PC/SC IFD Handler implementation (ifdhandler.c)
 *
 * Copyright (c) 2026
 *
 * This implements the IFD Handler API v3.0 for pcscd to communicate
 * with PX4 smart card devices (/dev/px4card*).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>

/* PC/SC IFD Handler API headers */
#include <PCSC/ifdhandler.h>
#include <PCSC/debuglog.h>

/* PX4 specific headers */
#include "px4_card_ioctl.h"

#define MAX_READERS 16
#define MAX_ATR_SIZE 33
/* MAX_BUFFER_SIZE is defined in pcsclite.h as 264 */

/* Reader context */
struct reader_context {
	int fd;
	char device_name[256];
	unsigned char atr[MAX_ATR_SIZE];
	unsigned int atr_len;
	int protocol;
};

static struct reader_context readers[MAX_READERS];

/* Helper functions */
static int get_reader_index(DWORD Lun)
{
	return (int)(Lun & 0xFFFF);
}

static struct reader_context *get_reader(DWORD Lun)
{
	int idx = get_reader_index(Lun);
	if (idx < 0 || idx >= MAX_READERS)
		return NULL;
	return &readers[idx];
}

/*
 * IFDHCreateChannel
 * Opens a communication channel to the device
 */
RESPONSECODE IFDHCreateChannel(DWORD Lun, DWORD Channel)
{
    Log1(PCSC_LOG_ERROR, "IFDHCreateChannel: Use IFDHCreateChannelByName instead");
    return IFD_COMMUNICATION_ERROR;
}

/*
 * IFDHCreateChannelByName
 * Opens a communication channel to the device
 */
RESPONSECODE IFDHCreateChannelByName(DWORD Lun, LPSTR DeviceName)
{
	struct reader_context *ctx;
	int idx = get_reader_index(Lun);

	Log1(PCSC_LOG_INFO, "IFDHCreateChannelByName");

	if (idx < 0 || idx >= MAX_READERS) {
		Log1(PCSC_LOG_ERROR, "Invalid LUN");
		return IFD_COMMUNICATION_ERROR;
	}

	ctx = &readers[idx];
	memset(ctx, 0, sizeof(*ctx));

	/* Open device */
	ctx->fd = open(DeviceName, O_RDWR | O_NOCTTY);
	if (ctx->fd < 0) {
		Log1(PCSC_LOG_ERROR, "Failed to open device");
		return IFD_COMMUNICATION_ERROR;
	}

	strncpy(ctx->device_name, DeviceName, sizeof(ctx->device_name) - 1);
	Log1(PCSC_LOG_INFO, "Device opened");

	return IFD_SUCCESS;
}

/*
 * IFDHCloseChannel
 * Closes the communication channel
 */
RESPONSECODE IFDHCloseChannel(DWORD Lun)
{
	struct reader_context *ctx = get_reader(Lun);

	Log1(PCSC_LOG_INFO, "IFDHCloseChannel");

	if (!ctx || ctx->fd < 0)
		return IFD_COMMUNICATION_ERROR;

	close(ctx->fd);
	ctx->fd = -1;

	return IFD_SUCCESS;
}

/*
 * IFDHGetCapabilities
 * Returns capabilities of the reader
 */
RESPONSECODE IFDHGetCapabilities(DWORD Lun, DWORD Tag, 
				 PDWORD Length, PUCHAR Value)
{
	struct reader_context *ctx = get_reader(Lun);

	Log1(PCSC_LOG_INFO, "IFDHGetCapabilities");

	if (!ctx || ctx->fd < 0)
		return IFD_COMMUNICATION_ERROR;

	switch (Tag) {
	case TAG_IFD_ATR:
		if (*Length < ctx->atr_len) {
			*Length = ctx->atr_len;
			return IFD_ERROR_INSUFFICIENT_BUFFER;
		}
		memcpy(Value, ctx->atr, ctx->atr_len);
		*Length = ctx->atr_len;
		break;

	case TAG_IFD_SIMULTANEOUS_ACCESS:
		if (*Length < 1)
			return IFD_ERROR_INSUFFICIENT_BUFFER;
		*Value = 1; /* One slot only */
		*Length = 1;
		break;

	case TAG_IFD_SLOTS_NUMBER:
		if (*Length < 1)
			return IFD_ERROR_INSUFFICIENT_BUFFER;
		*Value = 1; /* One slot */
		*Length = 1;
		break;

	case TAG_IFD_SLOT_THREAD_SAFE:
		if (*Length < 1)
			return IFD_ERROR_INSUFFICIENT_BUFFER;
		*Value = 0; /* Not thread safe */
		*Length = 1;
		break;

	default:
		Log1(PCSC_LOG_ERROR, "Unknown tag");
		return IFD_ERROR_TAG;
	}

	return IFD_SUCCESS;
}

/*
 * IFDHSetCapabilities
 * Sets reader capabilities (not supported yet)
 */
RESPONSECODE IFDHSetCapabilities(DWORD Lun, DWORD Tag,
				 DWORD Length, PUCHAR Value)
{
	Log1(PCSC_LOG_INFO, "IFDHSetCapabilities (not supported)");
	return IFD_NOT_SUPPORTED;
}

/*
 * IFDHSetProtocolParameters
 * Sets protocol parameters (T=0 or T=1)
 */
RESPONSECODE IFDHSetProtocolParameters(DWORD Lun, DWORD Protocol,
				       UCHAR Flags, UCHAR PTS1,
				       UCHAR PTS2, UCHAR PTS3)
{
	struct reader_context *ctx = get_reader(Lun);

	Log1(PCSC_LOG_INFO, "IFDHSetProtocolParameters");

	if (!ctx || ctx->fd < 0)
		return IFD_COMMUNICATION_ERROR;

	/* Store protocol for later use */
	ctx->protocol = Protocol;

	/* PX4 B-CAS cards typically use T=1 protocol */
	if (Protocol != SCARD_PROTOCOL_T0 && Protocol != SCARD_PROTOCOL_T1) {
		Log1(PCSC_LOG_ERROR, "Unsupported protocol");
		return IFD_PROTOCOL_NOT_SUPPORTED;
	}

	return IFD_SUCCESS;
}

/*
 * IFDHPowerICC
 * Powers on/off the smart card
 */
RESPONSECODE IFDHPowerICC(DWORD Lun, DWORD Action,
			  PUCHAR Atr, PDWORD AtrLength)
{
	struct reader_context *ctx = get_reader(Lun);
	struct px4_card_atr atr_data;
	int ret;

	Log1(PCSC_LOG_INFO, "IFDHPowerICC");

	if (!ctx || ctx->fd < 0)
		return IFD_COMMUNICATION_ERROR;

	switch (Action) {
	case IFD_POWER_UP:
	case IFD_RESET:
		/* Reset card */
		ret = ioctl(ctx->fd, PX4CARD_RESET);
		if (ret < 0) {
			Log1(PCSC_LOG_ERROR, "PX4CARD_RESET failed");
			return IFD_COMMUNICATION_ERROR;
		}

		/* Get ATR */
		ret = ioctl(ctx->fd, PX4CARD_GET_ATR, &atr_data);
		if (ret < 0) {
			Log1(PCSC_LOG_ERROR, "PX4CARD_GET_ATR failed");
			return IFD_COMMUNICATION_ERROR;
		}

		if (atr_data.length == 0 || atr_data.length > MAX_ATR_SIZE) {
			Log1(PCSC_LOG_ERROR, "Invalid ATR length");
			return IFD_COMMUNICATION_ERROR;
		}

		/* Store ATR */
		memcpy(ctx->atr, atr_data.data, atr_data.length);
		ctx->atr_len = atr_data.length;

		/* Return ATR to caller */
		if (*AtrLength < atr_data.length) {
			*AtrLength = atr_data.length;
			return IFD_ERROR_INSUFFICIENT_BUFFER;
		}

		memcpy(Atr, atr_data.data, atr_data.length);
		*AtrLength = atr_data.length;

		Log1(PCSC_LOG_INFO, "ATR received");
		LogXxd(PCSC_LOG_INFO, "ATR:", Atr, *AtrLength);
		break;

	case IFD_POWER_DOWN:
		/* PX4 doesn't support explicit power down */
		Log1(PCSC_LOG_INFO, "Power down not supported (ignored)");
		break;

	default:
			Log1(PCSC_LOG_ERROR, "Unknown power action");
		return IFD_NOT_SUPPORTED;
	}

	return IFD_SUCCESS;
}

/*
 * IFDHTransmitToICC
 * Transmits APDU to the card and receives response
 */
RESPONSECODE IFDHTransmitToICC(DWORD Lun, SCARD_IO_HEADER SendPci,
			       PUCHAR TxBuffer, DWORD TxLength,
			       PUCHAR RxBuffer, PDWORD RxLength,
			       PSCARD_IO_HEADER RecvPci)
{
	struct reader_context *ctx = get_reader(Lun);
	struct px4_card_data tx_data;
	struct px4_card_data rx_data;
	int ready = 0;
	unsigned int elapsed_ms = 0;
	const unsigned int timeout_ms = 5000;
	const unsigned int poll_interval_ms = 10;
	int ret;

	Log1(PCSC_LOG_INFO, "IFDHTransmitToICC");
	LogXxd(PCSC_LOG_INFO, "TX:", TxBuffer, TxLength);
	(void)SendPci;

	if (!ctx || ctx->fd < 0)
		return IFD_COMMUNICATION_ERROR;

	if (!TxBuffer || !RxBuffer || !RxLength)
		return IFD_COMMUNICATION_ERROR;

	if (TxLength == 0 || TxLength > sizeof(tx_data.buffer) ||
	    *RxLength > sizeof(rx_data.buffer))
		return IFD_COMMUNICATION_ERROR;

	/* Send APDU to the driver via ioctl */
	memset(&tx_data, 0, sizeof(tx_data));
	memcpy(tx_data.buffer, TxBuffer, TxLength);
	tx_data.length = (unsigned char)TxLength;

	ret = ioctl(ctx->fd, PX4CARD_WRITE, &tx_data);
	if (ret < 0) {
		Log2(PCSC_LOG_ERROR, "PX4CARD_WRITE failed: %s", strerror(errno));
		return IFD_COMMUNICATION_ERROR;
	}

	/* Poll readiness and keep prior 5-second timeout behavior */
	while (elapsed_ms < timeout_ms) {
		ready = 0;
		ret = ioctl(ctx->fd, PX4CARD_READ_READY, &ready);
		if (ret < 0) {
			Log2(PCSC_LOG_ERROR, "PX4CARD_READ_READY failed: %s", strerror(errno));
			return IFD_COMMUNICATION_ERROR;
		}

		if (ready)
			break;

		usleep(poll_interval_ms * 1000);
		elapsed_ms += poll_interval_ms;
	}

	if (!ready) {
		Log1(PCSC_LOG_ERROR, "Read timeout");
		return IFD_RESPONSE_TIMEOUT;
	}

	/* Read APDU response */
	memset(&rx_data, 0, sizeof(rx_data));
	ret = ioctl(ctx->fd, PX4CARD_READ, &rx_data);
	if (ret < 0) {
		if (errno == EAGAIN)
			return IFD_RESPONSE_TIMEOUT;

		Log2(PCSC_LOG_ERROR, "PX4CARD_READ failed: %s", strerror(errno));
		return IFD_COMMUNICATION_ERROR;
	}

	if (*RxLength < rx_data.length) {
		*RxLength = rx_data.length;
		return IFD_ERROR_INSUFFICIENT_BUFFER;
	}

	memcpy(RxBuffer, rx_data.buffer, rx_data.length);
	*RxLength = rx_data.length;

	Log1(PCSC_LOG_INFO, "RX completed");
	LogXxd(PCSC_LOG_INFO, "RX:", RxBuffer, *RxLength);

	if (RecvPci)
		RecvPci->Protocol = ctx->protocol;

	return IFD_SUCCESS;
}

/*
 * IFDHControl
 * Device-specific control operations
 */
RESPONSECODE IFDHControl(DWORD Lun, DWORD dwControlCode,
			 PUCHAR TxBuffer, DWORD TxLength,
			 PUCHAR RxBuffer, DWORD RxLength,
			 LPDWORD pdwBytesReturned)
{
	Log1(PCSC_LOG_INFO, "IFDHControl (not supported)");
	return IFD_ERROR_NOT_SUPPORTED;
}

/*
 * IFDHICCPresence
 * Checks if a card is present
 */
RESPONSECODE IFDHICCPresence(DWORD Lun)
{
	struct reader_context *ctx = get_reader(Lun);
	int detected = 0;
	int ret;

	if (!ctx || ctx->fd < 0)
		return IFD_COMMUNICATION_ERROR;

	ret = ioctl(ctx->fd, PX4CARD_DETECT, &detected);
	if (ret < 0) {
		Log1(PCSC_LOG_ERROR, "PX4CARD_DETECT failed");
		return IFD_COMMUNICATION_ERROR;
	}

	Log1(PCSC_LOG_DEBUG, "Card presence check");

	return detected ? IFD_ICC_PRESENT : IFD_ICC_NOT_PRESENT;
}
