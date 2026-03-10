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
#include <limits.h>
#include <sys/ioctl.h>

/* PC/SC IFD Handler API headers */
#include <PCSC/ifdhandler.h>
#include <PCSC/debuglog.h>

/* PX4 specific headers */
#include "px4_card_ioctl.h"

#define MAX_READERS 16
#define MAX_ATR_SIZE 33
#define DEFAULT_T1_IFSC 32
#define RX_ZERO_LENGTH_RETRY_MAX 3
/* MAX_BUFFER_SIZE is defined in pcsclite.h as 264 */

/* Reader context */
struct reader_context {
	int fd;
	char device_name[256];
	unsigned char atr[MAX_ATR_SIZE];
	unsigned int atr_len;
	int protocol;
	unsigned int t1_ifsc;
};

static struct reader_context readers[MAX_READERS];

/* pcscd optional callback for TAG_IFD_STOP_POLLING_THREAD */
static RESPONSECODE px4_ifd_stop_polling(DWORD Lun)
{
	(void)Lun;
	return IFD_SUCCESS;
}

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

/* Extract IFSC from ATR TA3 when T=1 parameters are present. */
static unsigned int px4_ifd_parse_t1_ifsc(const unsigned char *atr,
					  unsigned int atr_len)
{
	unsigned int idx;
	unsigned int iface_idx;
	unsigned int y;
	unsigned int protocol_for_set;

	if (!atr || atr_len < 2)
		return DEFAULT_T1_IFSC;

	idx = 1;
	iface_idx = 1;
	y = (atr[idx] >> 4) & 0x0F;
	protocol_for_set = 0; /* T=0 by default for first interface set */

	while (1) {
		if (y & 0x1) { /* TAi */
			idx++;
			if (idx >= atr_len)
				break;

			if (iface_idx == 3 && protocol_for_set == 1 && atr[idx] != 0)
				return atr[idx];
		}

		if (y & 0x2) { /* TBi */
			idx++;
			if (idx >= atr_len)
				break;
		}

		if (y & 0x4) { /* TCi */
			idx++;
			if (idx >= atr_len)
				break;
		}

		if (y & 0x8) { /* TDi */
			idx++;
			if (idx >= atr_len)
				break;

			y = (atr[idx] >> 4) & 0x0F;
			protocol_for_set = atr[idx] & 0x0F;
			iface_idx++;
			continue;
		}

		break;
	}

	return DEFAULT_T1_IFSC;
}

/* Read response with retries to avoid treating transient 0-byte reads as success. */
static RESPONSECODE px4_ifd_read_response(struct reader_context *ctx,
					  PUCHAR RxBuffer,
					  PDWORD RxLength)
{
	struct px4_card_data rx_data;
	DWORD rx_capacity;
	DWORD total_len = 0;
	DWORD t1_frame_len = 0;
	int zero_length_retries = 0;
	int ret;

	if (!ctx || !RxBuffer || !RxLength)
		return IFD_COMMUNICATION_ERROR;

	rx_capacity = *RxLength;
	*RxLength = 0;

	if (rx_capacity == 0)
		return IFD_ERROR_INSUFFICIENT_BUFFER;

	while (1) {
		memset(&rx_data, 0, sizeof(rx_data));
		ret = ioctl(ctx->fd, PX4CARD_READ, &rx_data);
		if (ret < 0) {
			if (errno == EAGAIN) {
				if (total_len > 0)
					break;
				return IFD_RESPONSE_TIMEOUT;
			}

			Log2(PCSC_LOG_ERROR, "PX4CARD_READ failed: %s", strerror(errno));
			return IFD_COMMUNICATION_ERROR;
		}

		if (rx_data.length == 0) {
			if (total_len > 0)
				break;

			zero_length_retries++;
			if (zero_length_retries >= RX_ZERO_LENGTH_RETRY_MAX)
				return IFD_RESPONSE_TIMEOUT;

			usleep(5000);
			continue;
		}

		zero_length_retries = 0;

		if (total_len + rx_data.length > rx_capacity) {
			*RxLength = total_len + rx_data.length;
			return IFD_ERROR_INSUFFICIENT_BUFFER;
		}

		memcpy(RxBuffer + total_len, rx_data.buffer, rx_data.length);
		total_len += rx_data.length;

		if (ctx->protocol != SCARD_PROTOCOL_T1)
			break;

		if (t1_frame_len == 0 && total_len >= 3) {
			t1_frame_len = (DWORD)RxBuffer[2] + 4; /* NAD + PCB + LEN + EDC(1) */
			if (t1_frame_len > rx_capacity) {
				*RxLength = t1_frame_len;
				return IFD_ERROR_INSUFFICIENT_BUFFER;
			}
		}

		if (t1_frame_len > 0 && total_len >= t1_frame_len)
			break;
	}

	if (ctx->protocol == SCARD_PROTOCOL_T1 && t1_frame_len > 0 && total_len < t1_frame_len)
		return IFD_RESPONSE_TIMEOUT;

	if (total_len == 0)
		return IFD_RESPONSE_TIMEOUT;

	*RxLength = total_len;
	return IFD_SUCCESS;
}

/*
 * IFDHCreateChannel
 * Opens a communication channel to the device
 */
RESPONSECODE IFDHCreateChannel(DWORD Lun, DWORD Channel)
{
	(void)Lun;
	(void)Channel;
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
	ctx->fd = -1;
	ctx->t1_ifsc = DEFAULT_T1_IFSC;

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
	ctx->atr_len = 0;
	ctx->protocol = 0;
	ctx->t1_ifsc = DEFAULT_T1_IFSC;

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
	void *stop_polling_cb = (void *)px4_ifd_stop_polling;

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

	case TAG_IFD_POLLING_THREAD_KILLABLE:
		if (*Length < 1)
			return IFD_ERROR_INSUFFICIENT_BUFFER;
		*Value = 1; /* pcscd can stop polling thread with pthread_cancel() */
		*Length = 1;
		break;

	case TAG_IFD_STOP_POLLING_THREAD:
		if (*Length < sizeof(stop_polling_cb)) {
			*Length = sizeof(stop_polling_cb);
			return IFD_ERROR_INSUFFICIENT_BUFFER;
		}
		memcpy(Value, &stop_polling_cb, sizeof(stop_polling_cb));
		*Length = sizeof(stop_polling_cb);
		break;

	default:
		Log2(PCSC_LOG_DEBUG, "Unknown tag: 0x%X", Tag);
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
	(void)Lun;
	(void)Tag;
	(void)Length;
	(void)Value;
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
	(void)Flags;
	(void)PTS1;
	(void)PTS2;
	(void)PTS3;

	Log1(PCSC_LOG_INFO, "IFDHSetProtocolParameters");

	if (!ctx || ctx->fd < 0)
		return IFD_COMMUNICATION_ERROR;

	/* Store protocol for later use */
	ctx->protocol = Protocol;

	/* PX4 B-CAS cards typically use T=1 protocol */
	switch(Protocol) {
	case SCARD_PROTOCOL_T0:
		ctx->t1_ifsc = DEFAULT_T1_IFSC;
		Log1(PCSC_LOG_INFO, "Protocol set to T=0");
		break;
	case SCARD_PROTOCOL_T1:
		ctx->t1_ifsc = px4_ifd_parse_t1_ifsc(ctx->atr, ctx->atr_len);
		Log2(PCSC_LOG_INFO, "Protocol set to T=1, IFSC=%u", ctx->t1_ifsc);
		break;
	default:
		Log2(PCSC_LOG_ERROR, "Unsupported protocol: 0x%X", Protocol);
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
		if (Action == IFD_POWER_UP)
			Log1(PCSC_LOG_INFO, "action: PowerUp");
		else
			Log1(PCSC_LOG_INFO, "action: Reset");
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
		ctx->t1_ifsc = px4_ifd_parse_t1_ifsc(ctx->atr, ctx->atr_len);

		/* Return ATR to caller */
		if (*AtrLength < atr_data.length) {
			*AtrLength = atr_data.length;
			return IFD_ERROR_INSUFFICIENT_BUFFER;
		}

		memcpy(Atr, atr_data.data, atr_data.length);
		*AtrLength = atr_data.length;

		Log1(PCSC_LOG_INFO, "ATR received");
		LogXxd(PCSC_LOG_INFO, "ATR:", Atr, *AtrLength);
		Log2(PCSC_LOG_INFO, "Derived IFSC from ATR: %u", ctx->t1_ifsc);
		break;

	case IFD_POWER_DOWN:
		/* PX4 doesn't support explicit power down */
		Log1(PCSC_LOG_INFO, "'action: PowerDown' not supported (ignored)");
		break;

	default:
		Log2(PCSC_LOG_ERROR, "Unknown power action: 0x%X", Action);
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
	DWORD rx_capacity;
	int ret;

	Log1(PCSC_LOG_INFO, "IFDHTransmitToICC");
	LogXxd(PCSC_LOG_INFO, "TX:", TxBuffer, TxLength);
	if (SendPci.Protocol == SCARD_PROTOCOL_T0 || SendPci.Protocol == SCARD_PROTOCOL_T1)
		ctx->protocol = SendPci.Protocol;

	if (!ctx || ctx->fd < 0) {
		if (RxLength)
			*RxLength = 0;
		return IFD_COMMUNICATION_ERROR;
	}

	if (!TxBuffer || !RxBuffer || !RxLength)
		return IFD_COMMUNICATION_ERROR;

	rx_capacity = *RxLength;

	if (TxLength == 0 || TxLength > UCHAR_MAX || TxLength > sizeof(tx_data.buffer))
		return IFD_COMMUNICATION_ERROR;

	if (ctx->protocol == SCARD_PROTOCOL_T1 && TxLength > (DWORD)(ctx->t1_ifsc + 4))
		Log2(PCSC_LOG_DEBUG, "T=1 TX may require chaining, length=%u", TxLength);

	/* Send APDU to the driver via ioctl */
	memset(&tx_data, 0, sizeof(tx_data));
	memcpy(tx_data.buffer, TxBuffer, TxLength);
	tx_data.length = (unsigned char)TxLength;

	ret = ioctl(ctx->fd, PX4CARD_WRITE, &tx_data);
	if (ret < 0) {
		Log2(PCSC_LOG_ERROR, "PX4CARD_WRITE failed: %s", strerror(errno));
		return IFD_COMMUNICATION_ERROR;
	}

	*RxLength = rx_capacity;
	ret = px4_ifd_read_response(ctx, RxBuffer, RxLength);
	if (ret != IFD_SUCCESS)
		return ret;

	Log1(PCSC_LOG_INFO, "RX completed");
	LogXxd(PCSC_LOG_INFO, "RX:", RxBuffer, *RxLength);

	if (RecvPci) {
		RecvPci->Protocol = ctx->protocol;
		RecvPci->Length = sizeof(*RecvPci);
	}

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
	(void)Lun;
	(void)dwControlCode;
	(void)TxBuffer;
	(void)TxLength;
	(void)RxBuffer;
	(void)RxLength;
	if (pdwBytesReturned)
		*pdwBytesReturned = 0;
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

	// Log1(PCSC_LOG_DEBUG, "Card presence check");

	return detected ? IFD_ICC_PRESENT : IFD_ICC_NOT_PRESENT;
}
