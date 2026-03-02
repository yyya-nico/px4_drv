/*
 * PCSC-Lite Compatible Library Implementation (libpcsc.c)
 *
 * This implementation provides a PCSC-Lite compatible interface for
 * smart card communication via px4_drv driver
 *
 * SPDX-License-Identifier: MIT
 */

#include "scard.h"
#include "../../../include/ptx_ioctl.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pthread.h>

/* Device paths for supported readers */
#define LIBPCSC_DEVICE_PATH_0   "/dev/px4video0"
#define LIBPCSC_DEVICE_PATH_1   "/dev/px4video1"
#define LIBPCSC_DEVICE_PATH_2   "/dev/px4video2"
#define LIBPCSC_DEVICE_PATH_3   "/dev/px4video3"

#define LIBPCSC_MAX_DEVICES     4
#define LIBPCSC_MAX_READERS     8
#define LIBPCSC_MAX_CARDS       8

/* Internal structures */

typedef struct {
	int device_fd;
	char device_path[256];
	bool device_open;
	bool card_present;
	pthread_mutex_t lock;
} libpcsc_reader_t;

typedef struct {
	int reader_id;
	libpcsc_reader_t *reader;
	bool connected;
	DWORD active_protocol;
	pthread_mutex_t lock;
} libpcsc_card_handle_t;

typedef struct {
	unsigned int magic;
	libpcsc_reader_t readers[LIBPCSC_MAX_READERS];
	int num_readers;
	pthread_mutex_t lock;
} libpcsc_context_t;

#define LIBPCSC_CONTEXT_MAGIC   0xDEADBEEF

/* Helper functions */

static LONG libpcsc_error_from_errno(int err)
{
	switch (err) {
	case 0:
		return SCARD_S_SUCCESS;
	case ENOMEM:
		return SCARD_E_NO_MEMORY;
	case ENODEV:
		return SCARD_E_NO_SERVICE;
	case EAGAIN:
		return SCARD_E_TIMEOUT;
	case EBUSY:
		return SCARD_E_SHARING_VIOLATION;
	default:
		return SCARD_E_COMM_ERROR;
	}
}

static LONG libpcsc_open_device(const char *device_path, int *fd)
{
	if (!device_path || !fd)
		return SCARD_E_INVALID_PARAMETER;

	*fd = open(device_path, O_RDWR);
	if (*fd < 0)
		return libpcsc_error_from_errno(errno);

	return SCARD_S_SUCCESS;
}

static void libpcsc_close_device(int fd)
{
	if (fd >= 0)
		close(fd);
}

/* Context Management Functions */

LONG SCardEstablishContext(SCARD_SCOPE scope,
			   const void *reserved1,
			   const void *reserved2,
			   SCARDCONTEXT *pContext)
{
	libpcsc_context_t *ctx;
	int i;

	(void)reserved1;
	(void)reserved2;

	if (!pContext)
		return SCARD_E_INVALID_PARAMETER;

	ctx = (libpcsc_context_t *)calloc(1, sizeof(libpcsc_context_t));
	if (!ctx)
		return SCARD_E_NO_MEMORY;

	ctx->magic = LIBPCSC_CONTEXT_MAGIC;
	ctx->num_readers = 0;
	pthread_mutex_init(&ctx->lock, NULL);

	/* Initialize reader structure */
	for (i = 0; i < LIBPCSC_MAX_READERS; i++) {
		ctx->readers[i].device_fd = -1;
		ctx->readers[i].device_open = false;
		ctx->readers[i].card_present = false;
		pthread_mutex_init(&ctx->readers[i].lock, NULL);
	}

	/* Try to open available devices */
	const char *device_paths[] = {
		LIBPCSC_DEVICE_PATH_0,
		LIBPCSC_DEVICE_PATH_1,
		LIBPCSC_DEVICE_PATH_2,
		LIBPCSC_DEVICE_PATH_3
	};

	for (i = 0; i < LIBPCSC_MAX_DEVICES && ctx->num_readers < LIBPCSC_MAX_READERS; i++) {
		int fd;
		if (libpcsc_open_device(device_paths[i], &fd) == SCARD_S_SUCCESS) {
			ctx->readers[ctx->num_readers].device_fd = fd;
			ctx->readers[ctx->num_readers].device_open = true;
			strncpy(ctx->readers[ctx->num_readers].device_path,
				device_paths[i],
				sizeof(ctx->readers[ctx->num_readers].device_path) - 1);
			ctx->num_readers++;
		}
	}

	if (ctx->num_readers == 0) {
		pthread_mutex_destroy(&ctx->lock);
		free(ctx);
		return SCARD_E_NO_READERS_AVAILABLE;
	}

	*pContext = (SCARDCONTEXT)ctx;
	return SCARD_S_SUCCESS;
}

LONG SCardReleaseContext(SCARDCONTEXT hContext)
{
	libpcsc_context_t *ctx = (libpcsc_context_t *)hContext;
	int i;

	if (!ctx || ctx->magic != LIBPCSC_CONTEXT_MAGIC)
		return SCARD_E_INVALID_HANDLE;

	pthread_mutex_lock(&ctx->lock);

	for (i = 0; i < LIBPCSC_MAX_READERS; i++) {
		if (ctx->readers[i].device_open) {
			libpcsc_close_device(ctx->readers[i].device_fd);
			ctx->readers[i].device_open = false;
		}
		pthread_mutex_destroy(&ctx->readers[i].lock);
	}

	pthread_mutex_unlock(&ctx->lock);
	pthread_mutex_destroy(&ctx->lock);

	free(ctx);
	return SCARD_S_SUCCESS;
}

/* Reader Enumeration Functions */

LONG SCardListReaders(SCARDCONTEXT hContext,
		      const char *mszGroups,
		      char *mszReaders,
		      DWORD *pcchReaders)
{
	libpcsc_context_t *ctx = (libpcsc_context_t *)hContext;
	int i, buf_len = 0;
	DWORD required_len = 0;
	char reader_name[256];

	(void)mszGroups;

	if (!ctx || ctx->magic != LIBPCSC_CONTEXT_MAGIC)
		return SCARD_E_INVALID_HANDLE;

	if (!pcchReaders)
		return SCARD_E_INVALID_PARAMETER;

	pthread_mutex_lock(&ctx->lock);

	/* Calculate required buffer size */
	for (i = 0; i < ctx->num_readers; i++) {
		snprintf(reader_name, sizeof(reader_name),
			"PX4-BCAS:%d", i);
		required_len += strlen(reader_name) + 1;
	}
	required_len += 1;  /* Final null terminator */

	if (*pcchReaders == 0 || !mszReaders) {
		*pcchReaders = required_len;
		pthread_mutex_unlock(&ctx->lock);
		return SCARD_S_SUCCESS;
	}

	if (*pcchReaders < required_len) {
		*pcchReaders = required_len;
		pthread_mutex_unlock(&ctx->lock);
		return SCARD_E_INVALID_VALUE;
	}

	/* Fill buffer with reader names */
	buf_len = 0;
	for (i = 0; i < ctx->num_readers; i++) {
		snprintf(reader_name, sizeof(reader_name),
			"PX4-BCAS:%d", i);
		int name_len = strlen(reader_name) + 1;
		memcpy(&mszReaders[buf_len], reader_name, name_len);
		buf_len += name_len;
	}
	mszReaders[buf_len++] = '\0';

	*pcchReaders = buf_len;

	pthread_mutex_unlock(&ctx->lock);
	return SCARD_S_SUCCESS;
}

LONG SCardGetStatusChange(SCARDCONTEXT hContext,
			  DWORD dwTimeout,
			  struct scard_readerstate *rgReaderStates,
			  DWORD cReaders)
{
	libpcsc_context_t *ctx = (libpcsc_context_t *)hContext;
	DWORD i;

	if (!ctx || ctx->magic != LIBPCSC_CONTEXT_MAGIC)
		return SCARD_E_INVALID_HANDLE;

	if (!rgReaderStates || cReaders == 0)
		return SCARD_E_INVALID_PARAMETER;

	/* For now, simply update state based on card detection */
	pthread_mutex_lock(&ctx->lock);

	for (i = 0; i < cReaders; i++) {
		struct scard_readerstate *state = &rgReaderStates[i];
		int reader_id = 0;

		/* Parse reader ID from reader name (PX4-BCAS:N) */
		if (state->reader && sscanf(state->reader, "PX4-BCAS:%d", &reader_id) == 1) {
			if (reader_id >= 0 && reader_id < ctx->num_readers) {
				struct ptx_bcas_detect_card detect = {0};
				libpcsc_reader_t *reader = &ctx->readers[reader_id];

				if (reader->device_open) {
					int ret = ioctl(reader->device_fd,
							PTX_BCAS_DETECT_CARD,
							&detect);
					if (ret == 0) {
						reader->card_present = (detect.detected != 0);
						state->event_state = reader->card_present ?
							SCARD_PRESENT : SCARD_ABSENT;
					}
				}
			}
		}
	}

	pthread_mutex_unlock(&ctx->lock);

	/* Sleep for timeout duration */
	if (dwTimeout > 0)
		usleep(dwTimeout * 1000);

	return SCARD_S_SUCCESS;
}

/* Connection Management Functions */

LONG SCardConnect(SCARDCONTEXT hContext,
		  const char *szReader,
		  SCARD_SHARE_MODE dwShareMode,
		  DWORD dwPreferredProtocols,
		  SCARDHANDLE *phCard,
		  DWORD *pdwActiveProtocol)
{
	libpcsc_context_t *ctx = (libpcsc_context_t *)hContext;
	libpcsc_card_handle_t *card_handle;
	int reader_id = 0;

	if (!ctx || ctx->magic != LIBPCSC_CONTEXT_MAGIC)
		return SCARD_E_INVALID_HANDLE;

	if (!szReader || !phCard)
		return SCARD_E_INVALID_PARAMETER;

	/* Parse reader ID from reader name (PX4-BCAS:N) */
	if (sscanf(szReader, "PX4-BCAS:%d", &reader_id) != 1)
		return SCARD_E_INVALID_VALUE;

	if (reader_id < 0 || reader_id >= ctx->num_readers)
		return SCARD_E_READER_UNAVAILABLE;

	pthread_mutex_lock(&ctx->lock);

	libpcsc_reader_t *reader = &ctx->readers[reader_id];
	if (!reader->device_open) {
		pthread_mutex_unlock(&ctx->lock);
		return SCARD_E_READER_UNAVAILABLE;
	}

	/* Allocate card handle */
	card_handle = (libpcsc_card_handle_t *)calloc(1, sizeof(libpcsc_card_handle_t));
	if (!card_handle) {
		pthread_mutex_unlock(&ctx->lock);
		return SCARD_E_NO_MEMORY;
	}

	card_handle->reader_id = reader_id;
	card_handle->reader = reader;
	card_handle->connected = true;
	card_handle->active_protocol = SCARD_PROTOCOL_T0;
	pthread_mutex_init(&card_handle->lock, NULL);

	/* Reset card */
	int ret = ioctl(reader->device_fd, PTX_BCAS_RESET_CARD, NULL);
	if (ret != 0) {
		free(card_handle);
		pthread_mutex_unlock(&ctx->lock);
		return libpcsc_error_from_errno(errno);
	}

	if (pdwActiveProtocol)
		*pdwActiveProtocol = card_handle->active_protocol;

	*phCard = (SCARDHANDLE)card_handle;

	pthread_mutex_unlock(&ctx->lock);
	return SCARD_S_SUCCESS;
}

LONG SCardDisconnect(SCARDHANDLE hCard,
		     SCARD_DISPOSITION dwDisposition)
{
	libpcsc_card_handle_t *card_handle = (libpcsc_card_handle_t *)hCard;

	if (!card_handle)
		return SCARD_E_INVALID_HANDLE;

	pthread_mutex_lock(&card_handle->lock);

	if (!card_handle->connected) {
		pthread_mutex_unlock(&card_handle->lock);
		return SCARD_E_INVALID_HANDLE;
	}

	card_handle->connected = false;

	pthread_mutex_unlock(&card_handle->lock);
	pthread_mutex_destroy(&card_handle->lock);

	free(card_handle);
	return SCARD_S_SUCCESS;
}

/* Card Communication Functions */

LONG SCardTransmit(SCARDHANDLE hCard,
		   const void *pioSendPci,
		   const BYTE *pbSendBuffer,
		   DWORD cbSendLength,
		   void *pioRecvPci,
		   BYTE *pbRecvBuffer,
		   DWORD *pcbRecvLength)
{
	libpcsc_card_handle_t *card_handle = (libpcsc_card_handle_t *)hCard;
	struct ptx_bcas_data send_data, recv_data;
	int ret;

	(void)pioSendPci;
	(void)pioRecvPci;

	if (!card_handle || !card_handle->connected)
		return SCARD_E_INVALID_HANDLE;

	if (!pbSendBuffer || cbSendLength == 0 || !pbRecvBuffer || !pcbRecvLength)
		return SCARD_E_INVALID_PARAMETER;

	if (cbSendLength > 255)
		return SCARD_E_INVALID_VALUE;

	pthread_mutex_lock(&card_handle->lock);

	/* Send command */
	send_data.buf = (BYTE *)pbSendBuffer;
	send_data.len = cbSendLength;

	ret = ioctl(card_handle->reader->device_fd,
		    PTX_BCAS_SEND_DATA,
		    &send_data);
	if (ret != 0) {
		pthread_mutex_unlock(&card_handle->lock);
		return libpcsc_error_from_errno(errno);
	}

	/* Receive response */
	recv_data.buf = pbRecvBuffer;
	recv_data.len = *pcbRecvLength;

	ret = ioctl(card_handle->reader->device_fd,
		    PTX_BCAS_RCV_DATA,
		    &recv_data);
	if (ret != 0) {
		pthread_mutex_unlock(&card_handle->lock);
		return libpcsc_error_from_errno(errno);
	}

	*pcbRecvLength = recv_data.len;

	pthread_mutex_unlock(&card_handle->lock);
	return SCARD_S_SUCCESS;
}

LONG SCardControl(SCARDHANDLE hCard,
		  DWORD dwControlCode,
		  const BYTE *pbSendBuffer,
		  DWORD cbSendLength,
		  BYTE *pbRecvBuffer,
		  DWORD cbRecvLength,
		  DWORD *lpBytesReturned)
{
	libpcsc_card_handle_t *card_handle = (libpcsc_card_handle_t *)hCard;

	(void)dwControlCode;
	(void)pbSendBuffer;
	(void)cbSendLength;
	(void)pbRecvBuffer;
	(void)cbRecvLength;
	(void)lpBytesReturned;

	if (!card_handle || !card_handle->connected)
		return SCARD_E_INVALID_HANDLE;

	return SCARD_E_NOT_READY;
}

LONG SCardGetAttrib(SCARDHANDLE hCard,
		    DWORD dwAttrId,
		    BYTE *pbAttr,
		    DWORD *pcbAttrLen)
{
	libpcsc_card_handle_t *card_handle = (libpcsc_card_handle_t *)hCard;

	(void)dwAttrId;
	(void)pbAttr;
	(void)pcbAttrLen;

	if (!card_handle || !card_handle->connected)
		return SCARD_E_INVALID_HANDLE;

	return SCARD_E_NOT_READY;
}

LONG SCardSetAttrib(SCARDHANDLE hCard,
		    DWORD dwAttrId,
		    const BYTE *pbAttr,
		    DWORD cbAttrLen)
{
	libpcsc_card_handle_t *card_handle = (libpcsc_card_handle_t *)hCard;

	(void)dwAttrId;
	(void)pbAttr;
	(void)cbAttrLen;

	if (!card_handle || !card_handle->connected)
		return SCARD_E_INVALID_HANDLE;

	return SCARD_E_NOT_READY;
}

/* Error Message Functions */

const char *SCardGetErrorMessage(LONG lError)
{
	static const struct {
		LONG code;
		const char *message;
	} error_messages[] = {
		{ SCARD_S_SUCCESS, "Operation completed successfully" },
		{ SCARD_E_INVALID_HANDLE, "Invalid handle" },
		{ SCARD_E_INVALID_PARAMETER, "Invalid parameter" },
		{ SCARD_E_INVALID_VALUE, "Invalid value" },
		{ SCARD_E_NO_MEMORY, "Not enough memory" },
		{ SCARD_E_NO_SERVICE, "Service not available" },
		{ SCARD_E_NO_READERS_AVAILABLE, "No readers available" },
		{ SCARD_E_READER_UNAVAILABLE, "Reader unavailable" },
		{ SCARD_E_TIMEOUT, "Operation timeout" },
		{ SCARD_E_SHARING_VIOLATION, "Sharing violation" },
		{ SCARD_E_NO_CARD, "No card in reader" },
		{ SCARD_E_PROTO_MISMATCH, "Protocol mismatch" },
		{ SCARD_E_NOT_READY, "Device not ready" },
		{ SCARD_E_COMM_ERROR, "Communication error" },
		{ 0, NULL }
	};

	for (int i = 0; error_messages[i].message; i++) {
		if (error_messages[i].code == lError)
			return error_messages[i].message;
	}

	return "Unknown error";
}
