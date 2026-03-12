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
#include <sys/time.h>

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

/* T=1 protocol constants (ISO/IEC 7816-3) */
#define T1_NAD_IFD_ICC       0x00   /* NAD: IFD=0, ICC=0 */
#define T1_PCB_I_BLOCK       0x00   /* I-block (bit7=0) */
#define T1_PCB_I_SEQ         0x40   /* I-block sequence number (bit6) */
#define T1_PCB_I_CHAIN       0x20   /* I-block more-data chain flag (bit5) */
#define T1_PCB_R_BLOCK       0x80   /* R-block (bits7:6=10) */
#define T1_PCB_R_SEQ         0x10   /* R-block next-expected seq (bit4) */
#define T1_PCB_R_NO_ERROR    0x00   /* R-block: no error */
#define T1_PCB_S_RESYNCH_REQ 0xC0   /* S-block RESYNCH request */
#define T1_PCB_S_RESYNCH_RSP 0xE0   /* S-block RESYNCH response */
#define T1_PCB_S_IFS_REQ     0xC1   /* S-block IFS request */
#define T1_PCB_S_IFS_RSP     0xE1   /* S-block IFS response */
#define T1_IFS_IFSD          254    /* IFD max INF size to advertise to card */
#define T1_GUARD_INTERVAL_MS 50L    /* Min ms between TX and previous RX */
#define T1_RX_TIMEOUT_MS     200   /* Max ms to wait for card ready */
#define T1_RX_POLL_MS        10     /* Polling interval ms for RX ready */

/* Reader context */
struct reader_context {
	int fd;
	char device_name[256];
	unsigned char atr[MAX_ATR_SIZE];
	unsigned int atr_len;
	int protocol;
	unsigned int t1_ifsc;
	int t1_edc_crc;            /* 0=LRC (default), 1=CRC */
	int t1_seq;                /* IFD TX I-block sequence number (0 or 1) */
	struct timeval last_rx_time; /* Timestamp of last successful RX */
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

/* Parse T=1 parameters from ATR: IFSC (from TA3 for T=1 interface)
 * and EDC type (from TCi for T=1 interface, bit0=1 means CRC). */
static void px4_ifd_parse_t1_atr(struct reader_context *ctx)
{
	const unsigned char *atr = ctx->atr;
	unsigned int atr_len = ctx->atr_len;
	unsigned int idx;
	unsigned int iface_idx;
	unsigned int y;
	unsigned int protocol_for_set;

	/* Reset to defaults */
	ctx->t1_ifsc = DEFAULT_T1_IFSC;
	ctx->t1_edc_crc = 0;

	if (!atr || atr_len < 2)
		return;

	idx = 1;
	iface_idx = 1;
	y = (atr[idx] >> 4) & 0x0F;
	protocol_for_set = 0; /* T=0 by default for first interface set */

	while (1) {
		if (y & 0x1) { /* TAi */
			idx++;
			if (idx >= atr_len)
				break;

			/* TA3 for T=1 interface: IFSC */
			if (iface_idx == 3 && protocol_for_set == 1 && atr[idx] != 0)
				ctx->t1_ifsc = atr[idx];
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

			/* TCi for T=1 interface: bit0=1 means CRC EDC */
			if (protocol_for_set == 1 && (atr[idx] & 0x01))
				ctx->t1_edc_crc = 1;
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

		if (ctx->protocol != 1)
			break;

		if (t1_frame_len == 0 && total_len >= 3) {
			t1_frame_len = (DWORD)RxBuffer[2] + 3 + (DWORD)(ctx->t1_edc_crc ? 2 : 1); /* NAD+PCB+LEN+INF+EDC */
			if (t1_frame_len > rx_capacity) {
				*RxLength = t1_frame_len;
				return IFD_ERROR_INSUFFICIENT_BUFFER;
			}
		}

		if (t1_frame_len > 0 && total_len >= t1_frame_len)
			break;
	}

	if (ctx->protocol == 1 && t1_frame_len > 0 && total_len < t1_frame_len)
		return IFD_RESPONSE_TIMEOUT;

	if (total_len == 0)
		return IFD_RESPONSE_TIMEOUT;

	*RxLength = total_len;
	return IFD_SUCCESS;
}

/* --- T=1 low-level helpers --- */

/* LRC: XOR of all bytes */
static unsigned char px4_t1_lrc(const unsigned char *data, unsigned int len)
{
	unsigned char lrc = 0;
	unsigned int i;
	for (i = 0; i < len; i++)
		lrc ^= data[i];
	return lrc;
}

/* CRC-CCITT: polynomial 0x1021, init 0xFFFF (ISO/IEC 7816-3) */
static unsigned short px4_t1_crc(const unsigned char *data, unsigned int len)
{
	unsigned short crc = 0xFFFF;
	unsigned int i, j;
	for (i = 0; i < len; i++) {
		crc ^= (unsigned short)data[i] << 8;
		for (j = 0; j < 8; j++) {
			if (crc & 0x8000)
				crc = (unsigned short)((crc << 1) ^ 0x1021u);
			else
				crc = (unsigned short)(crc << 1);
		}
	}
	return crc;
}

/*
 * Build a T=1 frame: [NAD][PCB][LEN][INF...][EDC...]
 * Returns total frame length, or 0 if frame_max is too small.
 */
static unsigned int px4_t1_make_frame(unsigned char *frame,
				      unsigned int frame_max,
				      unsigned char pcb,
				      const unsigned char *inf,
				      unsigned char inf_len,
				      int use_crc)
{
	unsigned int edc_len = use_crc ? 2u : 1u;
	unsigned int total_needed = 3u + inf_len + edc_len;
	unsigned int hdr_inf;
	unsigned short crc;

	if (frame_max < total_needed)
		return 0;

	frame[0] = T1_NAD_IFD_ICC;
	frame[1] = pcb;
	frame[2] = inf_len;
	if (inf_len > 0)
		memcpy(frame + 3, inf, inf_len);

	hdr_inf = 3u + inf_len;
	if (use_crc) {
		crc = px4_t1_crc(frame, hdr_inf);
		frame[hdr_inf]     = (unsigned char)(crc >> 8);
		frame[hdr_inf + 1] = (unsigned char)(crc & 0xFF);
	} else {
		frame[hdr_inf] = px4_t1_lrc(frame, hdr_inf);
	}
	return total_needed;
}

/* Wait at least T1_GUARD_INTERVAL_MS ms since last successful RX. */
static void px4_ifd_guard_interval(struct reader_context *ctx)
{
	struct timeval now;
	long elapsed_ms;

	gettimeofday(&now, NULL);
	elapsed_ms = (now.tv_sec  - ctx->last_rx_time.tv_sec)  * 1000L
		   + (now.tv_usec - ctx->last_rx_time.tv_usec) / 1000L;

	if (elapsed_ms < T1_GUARD_INTERVAL_MS)
		usleep((useconds_t)((T1_GUARD_INTERVAL_MS - elapsed_ms) * 1000L));
}

/* Send a raw frame to the card device. */
static RESPONSECODE px4_ifd_send_frame(struct reader_context *ctx,
				       const unsigned char *frame,
				       unsigned int len)
{
	struct px4_card_data tx;

	if (len == 0 || len > sizeof(tx.buffer))
		return IFD_COMMUNICATION_ERROR;

	memset(&tx, 0, sizeof(tx));
	memcpy(tx.buffer, frame, len);
	tx.length = (unsigned char)len;

	if (ioctl(ctx->fd, PX4CARD_WRITE, &tx) < 0) {
		Log2(PCSC_LOG_ERROR, "PX4CARD_WRITE failed: %s", strerror(errno));
		return IFD_COMMUNICATION_ERROR;
	}
	return IFD_SUCCESS;
}

/* Poll PX4CARD_READ_READY until data is ready or timeout_ms elapses. */
static RESPONSECODE px4_ifd_wait_rx_ready(struct reader_context *ctx,
					  unsigned int timeout_ms)
{
	int ready;
	unsigned int elapsed = 0;

	while (elapsed < timeout_ms) {
		ready = 0;
		if (ioctl(ctx->fd, PX4CARD_READ_READY, &ready) < 0) {
			Log2(PCSC_LOG_ERROR, "PX4CARD_READ_READY failed: %s",
			     strerror(errno));
			return IFD_COMMUNICATION_ERROR;
		}
		if (ready)
			return IFD_SUCCESS;
		usleep(T1_RX_POLL_MS * 1000u);
		elapsed += T1_RX_POLL_MS;
	}
	return IFD_RESPONSE_TIMEOUT;
}

/*
 * Receive a complete T=1 frame (NAD+PCB+LEN+INF+EDC).
 * Updates ctx->last_rx_time on success.
 */
static RESPONSECODE px4_ifd_recv_t1_frame(struct reader_context *ctx,
					  unsigned char *rx_buf,
					  DWORD *rx_len)
{
	struct px4_card_data rx_data;
	DWORD rx_capacity = *rx_len;
	DWORD total = 0;
	DWORD expected = 0;
	int zero_retries = 0;
	int edc_len = ctx->t1_edc_crc ? 2 : 1;
	RESPONSECODE rc;
	int ret;

	*rx_len = 0;
	rc = px4_ifd_wait_rx_ready(ctx, T1_RX_TIMEOUT_MS);
	if (rc != IFD_SUCCESS)
		return rc;

	while (1) {
		memset(&rx_data, 0, sizeof(rx_data));
		ret = ioctl(ctx->fd, PX4CARD_READ, &rx_data);
		if (ret < 0) {
			if (errno == EAGAIN) {
				if (total > 0)
					break;
				return IFD_RESPONSE_TIMEOUT;
			}
			Log2(PCSC_LOG_ERROR, "PX4CARD_READ failed: %s",
			     strerror(errno));
			return IFD_COMMUNICATION_ERROR;
		}

		if (rx_data.length == 0) {
			if (total > 0)
				break;
			zero_retries++;
			if (zero_retries >= RX_ZERO_LENGTH_RETRY_MAX)
				return IFD_RESPONSE_TIMEOUT;
			usleep(5000);
			continue;
		}
		zero_retries = 0;

		if (total + rx_data.length > rx_capacity)
			return IFD_ERROR_INSUFFICIENT_BUFFER;

		memcpy(rx_buf + total, rx_data.buffer, rx_data.length);
		total += rx_data.length;

		/* Compute expected frame length once LEN byte is available */
		if (expected == 0 && total >= 3)
			expected = (DWORD)rx_buf[2] + 3 + (DWORD)edc_len;

		if (expected > 0 && total >= expected)
			break;
	}

	gettimeofday(&ctx->last_rx_time, NULL);
	*rx_len = total;
	return IFD_SUCCESS;
}

/*
 * T=1 initialization after card reset:
 *   1. RESYNCH S-block request/response
 *   2. IFS S-block request (IFSD=254) / response
 * Retries up to 3 times on failure.
 */
static RESPONSECODE px4_ifd_t1_init(struct reader_context *ctx)
{
	unsigned char frame[8];
	unsigned char rx_buf[8];
	unsigned char ifsd = T1_IFS_IFSD;
	unsigned int frame_len;
	DWORD rx_len;
	RESPONSECODE rc;
	int retry;

	ctx->t1_seq = 0;

	for (retry = 0; retry < 3; retry++) {
		/* --- Send RESYNCH S-block --- */
		frame_len = px4_t1_make_frame(frame, sizeof(frame),
					      T1_PCB_S_RESYNCH_REQ,
					      NULL, 0, ctx->t1_edc_crc);
		if (frame_len == 0)
			return IFD_COMMUNICATION_ERROR;

		rc = px4_ifd_send_frame(ctx, frame, frame_len);
		if (rc != IFD_SUCCESS)
			continue;

		rx_len = sizeof(rx_buf);
		rc = px4_ifd_recv_t1_frame(ctx, rx_buf, &rx_len);
		if (rc != IFD_SUCCESS) {
			Log1(PCSC_LOG_ERROR, "T=1 init: no RESYNCH response");
			continue;
		}
		if (rx_len < 3 || rx_buf[1] != T1_PCB_S_RESYNCH_RSP) {
			Log2(PCSC_LOG_ERROR, "T=1 init: RESYNCH PCB mismatch (got 0x%02X)",
			     (unsigned int)rx_buf[1]);
			continue;
		}
		Log1(PCSC_LOG_INFO, "T=1 RESYNCH OK");

		/* --- Send IFS S-block (IFSD = 254) --- */
		frame_len = px4_t1_make_frame(frame, sizeof(frame),
					      T1_PCB_S_IFS_REQ,
					      &ifsd, 1, ctx->t1_edc_crc);
		if (frame_len == 0)
			return IFD_COMMUNICATION_ERROR;

		px4_ifd_guard_interval(ctx);
		rc = px4_ifd_send_frame(ctx, frame, frame_len);
		if (rc != IFD_SUCCESS)
			continue;

		rx_len = sizeof(rx_buf);
		rc = px4_ifd_recv_t1_frame(ctx, rx_buf, &rx_len);
		if (rc != IFD_SUCCESS) {
			Log1(PCSC_LOG_ERROR, "T=1 init: no IFS response");
			continue;
		}
		if (rx_len < 4 || rx_buf[1] != T1_PCB_S_IFS_RSP) {
			Log2(PCSC_LOG_ERROR, "T=1 init: IFS PCB mismatch (got 0x%02X)",
			     (unsigned int)rx_buf[1]);
			continue;
		}
		Log2(PCSC_LOG_INFO, "T=1 IFS OK (IFSD=%u)", (unsigned int)rx_buf[3]);
		return IFD_SUCCESS;
	}

	Log1(PCSC_LOG_ERROR, "T=1 init: all retries exhausted");
	return IFD_COMMUNICATION_ERROR;
}

/*
 * T=1 Transmit: wrap APDU in I-block(s), send, receive response I-block(s).
 * Handles chaining in both directions.
 */
static RESPONSECODE px4_ifd_t1_transmit(struct reader_context *ctx,
					 const unsigned char *apdu,
					 DWORD apdu_len,
					 unsigned char *resp,
					 PDWORD resp_len)
{
	/* NAD(1)+PCB(1)+LEN(1)+INF(254)+EDC(2) = 259 */
	unsigned char frame[259];
	unsigned char rx_frame[259];
	DWORD rx_frame_len;
	DWORD offset = 0;
	unsigned int inf_len;
	unsigned char pcb;
	int chain;
	DWORD total_resp = 0;
	DWORD resp_capacity = *resp_len;
	unsigned int frame_len;
	RESPONSECODE rc;

	*resp_len = 0;

	/* --- Send phase: split APDU into I-blocks of <= t1_ifsc bytes --- */
	do {
		inf_len = (unsigned int)(apdu_len - offset);
		chain = 0;
		if (inf_len > ctx->t1_ifsc) {
			inf_len = ctx->t1_ifsc;
			chain = 1;
		}

		pcb = T1_PCB_I_BLOCK;
		if (ctx->t1_seq)
			pcb |= T1_PCB_I_SEQ;
		if (chain)
			pcb |= T1_PCB_I_CHAIN;

		frame_len = px4_t1_make_frame(frame, sizeof(frame), pcb,
					      apdu + offset,
					      (unsigned char)inf_len,
					      ctx->t1_edc_crc);
		if (frame_len == 0)
			return IFD_COMMUNICATION_ERROR;

		px4_ifd_guard_interval(ctx);
		LogXxd(PCSC_LOG_DEBUG, "T=1 TX I-block:", frame, frame_len);
		rc = px4_ifd_send_frame(ctx, frame, frame_len);
		if (rc != IFD_SUCCESS)
			return rc;

		offset += (DWORD)inf_len;
		ctx->t1_seq ^= 1;

		/* For chained TX: wait for R-block ACK from card */
		if (chain) {
			rx_frame_len = sizeof(rx_frame);
			rc = px4_ifd_recv_t1_frame(ctx, rx_frame, &rx_frame_len);
			if (rc != IFD_SUCCESS)
				return rc;
			if (rx_frame_len < 3 ||
			    (rx_frame[1] & 0xC0) != T1_PCB_R_BLOCK) {
				Log1(PCSC_LOG_ERROR,
				     "T=1 TX chain: expected R-block ACK");
				return IFD_COMMUNICATION_ERROR;
			}
			LogXxd(PCSC_LOG_DEBUG, "T=1 RX R-block (chain ACK):",
			       rx_frame, rx_frame_len);
		}
	} while (offset < apdu_len);

	/* --- Receive phase: collect response I-blocks --- */
	do {
		rx_frame_len = sizeof(rx_frame);
		rc = px4_ifd_recv_t1_frame(ctx, rx_frame, &rx_frame_len);
		if (rc != IFD_SUCCESS)
			return rc;

		LogXxd(PCSC_LOG_DEBUG, "T=1 RX I-block:", rx_frame, rx_frame_len);

		/* Must be an I-block (bit7=0) */
		if (rx_frame_len < 3 || (rx_frame[1] & 0x80) != 0) {
			Log2(PCSC_LOG_ERROR,
			     "T=1 RX: expected I-block, got PCB=0x%02X",
			     (unsigned int)rx_frame[1]);
			return IFD_COMMUNICATION_ERROR;
		}

		inf_len = (unsigned int)rx_frame[2];
		if (total_resp + (DWORD)inf_len > resp_capacity) {
			*resp_len = total_resp + (DWORD)inf_len;
			return IFD_ERROR_INSUFFICIENT_BUFFER;
		}
		if (inf_len > 0)
			memcpy(resp + total_resp, rx_frame + 3, inf_len);
		total_resp += (DWORD)inf_len;

		/* Chain flag: send R-block requesting next block */
		if (rx_frame[1] & T1_PCB_I_CHAIN) {
			/* R-block seq = next expected card I-block seq */
			int card_seq = (rx_frame[1] & T1_PCB_I_SEQ) ? 1 : 0;
			unsigned char r_pcb = (unsigned char)(T1_PCB_R_BLOCK |
						T1_PCB_R_NO_ERROR);
			if (!card_seq)   /* next expected = !current */
				r_pcb |= T1_PCB_R_SEQ;
			frame_len = px4_t1_make_frame(frame, sizeof(frame),
						      r_pcb, NULL, 0,
						      ctx->t1_edc_crc);
			px4_ifd_guard_interval(ctx);
			rc = px4_ifd_send_frame(ctx, frame, frame_len);
			if (rc != IFD_SUCCESS)
				return rc;
		} else {
			break; /* Last block */
		}
	} while (1);

	*resp_len = total_resp;
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
	ctx->t1_edc_crc = 0;
	ctx->t1_seq = 0;
	memset(&ctx->last_rx_time, 0, sizeof(ctx->last_rx_time));

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
		px4_ifd_parse_t1_atr(ctx);
		Log2(PCSC_LOG_INFO, "Protocol set to T=1, IFSC=%u", ctx->t1_ifsc);
		Log2(PCSC_LOG_INFO, "EDC type: %s", ctx->t1_edc_crc ? "CRC" : "LRC");
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

		/* Store ATR and parse T=1 parameters */
		memcpy(ctx->atr, atr_data.data, atr_data.length);
		ctx->atr_len = atr_data.length;
		ctx->protocol = 1; /* B-CAS is always T=1 */
		px4_ifd_parse_t1_atr(ctx);

		/* Return ATR to caller */
		if (*AtrLength < atr_data.length) {
			*AtrLength = atr_data.length;
			return IFD_ERROR_INSUFFICIENT_BUFFER;
		}

		memcpy(Atr, atr_data.data, atr_data.length);
		*AtrLength = atr_data.length;

		Log1(PCSC_LOG_INFO, "ATR received");
		LogXxd(PCSC_LOG_INFO, "ATR:", Atr, *AtrLength);
		Log2(PCSC_LOG_INFO, "IFSC from ATR: %u", ctx->t1_ifsc);
		Log2(PCSC_LOG_INFO, "EDC type: %s", ctx->t1_edc_crc ? "CRC" : "LRC");

		/* Switch UART baudrate to 19200 (B-CAS standard, typically used after ATR) */
		const int baurate_19200 = PX4CARD_BAUDRATE_19200;
		ret = ioctl(ctx->fd, PX4CARD_SET_BAUDRATE, &baurate_19200);
		if (ret < 0) {
			Log1(PCSC_LOG_ERROR, "PX4CARD_SET_BAUDRATE failed");
			return IFD_COMMUNICATION_ERROR;
		}
		Log1(PCSC_LOG_INFO, "UART baudrate set to 19200");

		/* T=1 initialization: RESYNCH then IFS exchange */
		ret = (int)px4_ifd_t1_init(ctx);
		if (ret != IFD_SUCCESS) {
			Log1(PCSC_LOG_ERROR, "T=1 init (RESYNCH+IFS) failed");
			return IFD_COMMUNICATION_ERROR;
		}
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
	int ready = 0;
	unsigned int elapsed_ms = 0;
	const unsigned int timeout_ms = 5000;
	const unsigned int poll_interval_ms = 10;
	DWORD rx_capacity;
	int ret;

	Log1(PCSC_LOG_INFO, "IFDHTransmitToICC");

	if (!ctx || ctx->fd < 0) {
		if (RxLength)
			*RxLength = 0;
		return IFD_COMMUNICATION_ERROR;
	}

	if (!TxBuffer || !RxBuffer || !RxLength)
		return IFD_COMMUNICATION_ERROR;

	if (SendPci.Protocol == 0 ||
	    SendPci.Protocol == 1)
		ctx->protocol = SendPci.Protocol;

	if (TxLength == 0)
		return IFD_COMMUNICATION_ERROR;

	rx_capacity = *RxLength;
	LogXxd(PCSC_LOG_INFO, "TX:", TxBuffer, TxLength);

	/* T=1: full ISO 7816-3 framing with guard interval and I-block chaining */
	if (ctx->protocol == 1) {
		if (TxLength > 254)
			return IFD_COMMUNICATION_ERROR;
		ret = (int)px4_ifd_t1_transmit(ctx, TxBuffer, TxLength,
					      RxBuffer, RxLength);
		if (ret != IFD_SUCCESS)
			return (RESPONSECODE)ret;
		Log1(PCSC_LOG_INFO, "RX completed");
		LogXxd(PCSC_LOG_INFO, "RX:", RxBuffer, *RxLength);
		if (RecvPci) {
			RecvPci->Protocol = ctx->protocol;
			RecvPci->Length = sizeof(*RecvPci);
		}
		return IFD_SUCCESS;
	}

	/* T=0 / raw: send APDU as-is */
	if (TxLength > sizeof(tx_data.buffer))
		return IFD_COMMUNICATION_ERROR;

	memset(&tx_data, 0, sizeof(tx_data));
	memcpy(tx_data.buffer, TxBuffer, TxLength);
	tx_data.length = (unsigned char)TxLength;

	ret = ioctl(ctx->fd, PX4CARD_WRITE, &tx_data);
	if (ret < 0) {
		Log2(PCSC_LOG_ERROR, "PX4CARD_WRITE failed: %s", strerror(errno));
		return IFD_COMMUNICATION_ERROR;
	}

	while (elapsed_ms < timeout_ms) {
		ready = 0;
		ret = ioctl(ctx->fd, PX4CARD_READ_READY, &ready);
		if (ret < 0) {
			Log2(PCSC_LOG_ERROR, "PX4CARD_READ_READY failed: %s",
			     strerror(errno));
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

	*RxLength = rx_capacity;
	ret = (int)px4_ifd_read_response(ctx, RxBuffer, RxLength);
	if (ret != IFD_SUCCESS)
		return (RESPONSECODE)ret;

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
