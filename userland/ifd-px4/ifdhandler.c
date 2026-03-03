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
#include <sys/select.h>

/* PC/SC IFD Handler API headers */
#include <PCSC/ifdhandler.h>
#include <PCSC/debuglog.h>

/* PX4 specific headers */
#include "px4_card_ioctl.h"

#define MAX_READERS 16
#define MAX_ATR_SIZE 33
#define MAX_BUFFER_SIZE 256

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
 * IFDHCreateChannelByName
 * Opens a communication channel to the device
 */
RESPONSECODE IFDHCreateChannelByName(DWORD Lun, LPSTR DeviceName)
{
	struct reader_context *ctx;
	int idx = get_reader_index(Lun);

	Log2(PCSC_LOG_INFO, "IFDHCreateChannelByName: Lun=0x%08X, Device=%s", Lun, DeviceName);

	if (idx < 0 || idx >= MAX_READERS) {
		Log1(PCSC_LOG_ERROR, "Invalid LUN");
		return IFD_COMMUNICATION_ERROR;
	}

	ctx = &readers[idx];
	memset(ctx, 0, sizeof(*ctx));

	/* Open device */
	ctx->fd = open(DeviceName, O_RDWR | O_NOCTTY);
	if (ctx->fd < 0) {
		Log3(PCSC_LOG_ERROR, "Failed to open %s: %s", DeviceName, strerror(errno));
		return IFD_COMMUNICATION_ERROR;
	}

	strncpy(ctx->device_name, DeviceName, sizeof(ctx->device_name) - 1);
	Log2(PCSC_LOG_INFO, "Opened %s (fd=%d)", DeviceName, ctx->fd);

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

	Log2(PCSC_LOG_INFO, "IFDHGetCapabilities: Tag=0x%08X", Tag);

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
		Log2(PCSC_LOG_ERROR, "Unknown tag: 0x%08X", Tag);
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
	Log2(PCSC_LOG_INFO, "IFDHSetCapabilities: Tag=0x%08X (not supported)", Tag);
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

	Log2(PCSC_LOG_INFO, "IFDHSetProtocolParameters: Protocol=%d", Protocol);

	if (!ctx || ctx->fd < 0)
		return IFD_COMMUNICATION_ERROR;

	/* Store protocol for later use */
	ctx->protocol = Protocol;

	/* PX4 B-CAS cards typically use T=1 protocol */
	if (Protocol != SCARD_PROTOCOL_T0 && Protocol != SCARD_PROTOCOL_T1) {
		Log2(PCSC_LOG_ERROR, "Unsupported protocol: %d", Protocol);
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

	Log2(PCSC_LOG_INFO, "IFDHPowerICC: Action=%d", Action);

	if (!ctx || ctx->fd < 0)
		return IFD_COMMUNICATION_ERROR;

	switch (Action) {
	case IFD_POWER_UP:
	case IFD_RESET:
		/* Reset card */
		ret = ioctl(ctx->fd, PX4CARD_RESET);
		if (ret < 0) {
			Log2(PCSC_LOG_ERROR, "PX4CARD_RESET failed: %s", strerror(errno));
			return IFD_COMMUNICATION_ERROR;
		}

		/* Get ATR */
		ret = ioctl(ctx->fd, PX4CARD_GET_ATR, &atr_data);
		if (ret < 0) {
			Log2(PCSC_LOG_ERROR, "PX4CARD_GET_ATR failed: %s", strerror(errno));
			return IFD_COMMUNICATION_ERROR;
		}

		if (atr_data.length == 0 || atr_data.length > MAX_ATR_SIZE) {
			Log2(PCSC_LOG_ERROR, "Invalid ATR length: %d", atr_data.length);
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

		Log2(PCSC_LOG_INFO, "ATR received: %d bytes", atr_data.length);
		LogXxd(PCSC_LOG_INFO, "ATR:", Atr, *AtrLength);
		break;

	case IFD_POWER_DOWN:
		/* PX4 doesn't support explicit power down */
		Log1(PCSC_LOG_INFO, "Power down not supported (ignored)");
		break;

	default:
		Log2(PCSC_LOG_ERROR, "Unknown power action: %d", Action);
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
	ssize_t sent, received;
	fd_set readfds;
	struct timeval timeout;
	int ret;

	Log3(PCSC_LOG_INFO, "IFDHTransmitToICC: Protocol=%d, TxLen=%d", 
	     SendPci.Protocol, TxLength);
	LogXxd(PCSC_LOG_INFO, "TX:", TxBuffer, TxLength);

	if (!ctx || ctx->fd < 0)
		return IFD_COMMUNICATION_ERROR;

	if (TxLength > MAX_BUFFER_SIZE || *RxLength > MAX_BUFFER_SIZE)
		return IFD_COMMUNICATION_ERROR;

	/* Send data */
	sent = write(ctx->fd, TxBuffer, TxLength);
	if (sent < 0) {
		Log2(PCSC_LOG_ERROR, "Write failed: %s", strerror(errno));
		return IFD_COMMUNICATION_ERROR;
	}

	if ((size_t)sent != TxLength) {
		Log3(PCSC_LOG_ERROR, "Partial write: %d/%d", sent, TxLength);
		return IFD_COMMUNICATION_ERROR;
	}

	/* Wait for response with timeout (5 seconds) */
	FD_ZERO(&readfds);
	FD_SET(ctx->fd, &readfds);
	timeout.tv_sec = 5;
	timeout.tv_usec = 0;

	ret = select(ctx->fd + 1, &readfds, NULL, NULL, &timeout);
	if (ret < 0) {
		Log2(PCSC_LOG_ERROR, "Select failed: %s", strerror(errno));
		return IFD_COMMUNICATION_ERROR;
	}

	if (ret == 0) {
		Log1(PCSC_LOG_ERROR, "Read timeout");
		return IFD_RESPONSE_TIMEOUT;
	}

	/* Receive response */
	received = read(ctx->fd, RxBuffer, *RxLength);
	if (received < 0) {
		Log2(PCSC_LOG_ERROR, "Read failed: %s", strerror(errno));
		return IFD_COMMUNICATION_ERROR;
	}

	*RxLength = received;

	Log2(PCSC_LOG_INFO, "RX: %d bytes", received);
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
	Log2(PCSC_LOG_INFO, "IFDHControl: ControlCode=0x%08X (not supported)", dwControlCode);
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
		Log2(PCSC_LOG_ERROR, "PX4CARD_DETECT failed: %s", strerror(errno));
		return IFD_COMMUNICATION_ERROR;
	}

	Log2(PCSC_LOG_DEBUG, "Card presence: %s", detected ? "present" : "absent");

	return detected ? IFD_ICC_PRESENT : IFD_ICC_NOT_PRESENT;
}
