/*
 * PCSC-Lite Compatible Smart Card API (scard.h)
 *
 * This is a PCSC-Lite compatible interface for smart card communication
 * via px4_drv (PX-S1UR, PX-MLT, etc.)
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef __LIBPCSC_SCARD_H__
#define __LIBPCSC_SCARD_H__

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PCSC Error codes */
typedef int32_t LONG;
typedef uint8_t BYTE;
typedef uint32_t DWORD;

#define SCARD_S_SUCCESS             0x00000000  /* Operation completed successfully */
#define SCARD_E_INVALID_HANDLE      0x80100003  /* Invalid handle */
#define SCARD_E_INVALID_PARAMETER   0x80100004  /* Invalid parameter */
#define SCARD_E_INVALID_VALUE       0x80100011  /* Invalid value */
#define SCARD_E_NO_MEMORY           0x80100006  /* Not enough memory */
#define SCARD_E_NO_SERVICE          0x8010001D  /* Service not available */
#define SCARD_E_NO_READERS_AVAILABLE 0x8010002E /* No readers available */
#define SCARD_E_READER_UNAVAILABLE  0x80100017  /* Reader unavailable */
#define SCARD_E_TIMEOUT             0x8010000A  /* Timeout */
#define SCARD_E_SHARING_VIOLATION   0x8010000B  /* Sharing violation */
#define SCARD_E_NO_CARD             0x8010000C  /* No card in reader */
#define SCARD_E_PROTO_MISMATCH      0x8010000F  /* Protocol mismatch */
#define SCARD_E_NOT_READY           0x80100010  /* Device not ready */
#define SCARD_E_SYSTEM_CANCELLED    0x80100001  /* System cancelled */
#define SCARD_E_COMM_ERROR          0x8010001F  /* Communication error */

/* PCSC Return codes */
#define SCARD_W_REMOVED_CARD        0x80100069  /* Card was removed */
#define SCARD_W_RESET_CARD          0x80100068  /* Card was reset */

/* Card state definitions */
#define SCARD_UNKNOWN               0x00000001  /* Unknown state */
#define SCARD_ABSENT                0x00000002  /* Card is absent */
#define SCARD_PRESENT               0x00000004  /* Card is present */
#define SCARD_SWALLOWED             0x00000008  /* Card was swallowed */
#define SCARD_POWERED               0x00000010  /* Card is powered */
#define SCARD_NEGOTIABLE            0x00000020  /* Card is negotiable */
#define SCARD_SPECIFIC              0x00000040  /* Card is specific */

/* Protocol definitions */
#define SCARD_PROTOCOL_UNDEFINED    0x00000000  /* Undefined protocol */
#define SCARD_PROTOCOL_T0           0x00000001  /* T=0 protocol */
#define SCARD_PROTOCOL_T1           0x00000002  /* T=1 protocol */
#define SCARD_PROTOCOL_RAW          0x00000004  /* Raw protocol */
#define SCARD_PROTOCOL_T15          0x00000008  /* T=15 protocol */

/* Scope definitions */
typedef enum {
	SCARD_SCOPE_USER = 0,           /* User scope */
	SCARD_SCOPE_TERMINAL = 1,       /* Terminal scope */
	SCARD_SCOPE_SYSTEM = 2          /* System scope */
} SCARD_SCOPE;

/* Share modes */
typedef enum {
	SCARD_SHARE_EXCLUSIVE = 1,      /* Exclusive access */
	SCARD_SHARE_SHARED = 2,         /* Shared access */
	SCARD_SHARE_DIRECT = 3          /* Direct access */
} SCARD_SHARE_MODE;

/* Dispose actions */
typedef enum {
	SCARD_LEAVE_CARD = 0,           /* Leave card as is */
	SCARD_RESET_CARD = 1,           /* Reset card */
	SCARD_UNPOWER_CARD = 2,         /* Unpower card */
	SCARD_EJECT_CARD = 3            /* Eject card */
} SCARD_DISPOSITION;

/* Generic structure (Handle) */
typedef void *SCARDCONTEXT;
typedef void *SCARDHANDLE;

/* APDU Command/Response structure */
struct scard_apdu {
	BYTE   cla;                     /* Class byte */
	BYTE   ins;                     /* Instruction byte */
	BYTE   p1;                      /* Parameter 1 */
	BYTE   p2;                      /* Parameter 2 */
	BYTE   lc;                      /* Length of command data (0-255) */
	BYTE   *cmd_data;               /* Command data buffer */
	BYTE   le;                      /* Expected response length */
	BYTE   *resp_data;              /* Response data buffer */
	BYTE   resp_len;                /* Response data length */
	BYTE   sw1;                     /* Status word 1 */
	BYTE   sw2;                     /* Status word 2 */
};

/* ATR (Answer To Reset) structure */
struct scard_atr {
	BYTE   atr[33];                 /* ATR bytes */
	DWORD  atr_len;                 /* ATR length */
};

/* Reader info structure */
struct scard_readerstate {
	const char *reader;             /* Reader name */
	void *user_data;                /* User data */
	DWORD current_state;            /* Current state */
	DWORD event_state;              /* Event state */
	DWORD atr_len;                  /* ATR length */
	BYTE  atr[33];                  /* ATR bytes */
};

/* Context/Handle management functions */

/**
 * SCardEstablishContext - Establish a context
 *
 * @scope: SCARD_SCOPE_USER, SCARD_SCOPE_TERMINAL, or SCARD_SCOPE_SYSTEM
 * @reserved1: Reserved (NULL)
 * @reserved2: Reserved (NULL)
 * @pContext: Pointer to context handle
 *
 * Return: SCARD_S_SUCCESS on success, error code otherwise
 */
LONG SCardEstablishContext(SCARD_SCOPE scope,
			   const void *reserved1,
			   const void *reserved2,
			   SCARDCONTEXT *pContext);

/**
 * SCardReleaseContext - Release context
 *
 * @hContext: Context handle
 *
 * Return: SCARD_S_SUCCESS on success, error code otherwise
 */
LONG SCardReleaseContext(SCARDCONTEXT hContext);

/* Reader enumeration and state functions */

/**
 * SCardListReaders - List available readers
 *
 * @hContext: Context handle
 * @mszGroups: List of reader groups (NULL for all)
 * @mszReaders: Buffer for reader names
 * @pcchReaders: Buffer size / returned size
 *
 * Return: SCARD_S_SUCCESS on success, error code otherwise
 */
LONG SCardListReaders(SCARDCONTEXT hContext,
		      const char *mszGroups,
		      char *mszReaders,
		      DWORD *pcchReaders);

/**
 * SCardGetStatusChange - Get status change (block until change occurs)
 *
 * @hContext: Context handle
 * @dwTimeout: Timeout in milliseconds
 * @rgReaderStates: Array of reader states
 * @cReaders: Number of readers
 *
 * Return: SCARD_S_SUCCESS on success, error code otherwise
 */
LONG SCardGetStatusChange(SCARDCONTEXT hContext,
			  DWORD dwTimeout,
			  struct scard_readerstate *rgReaderStates,
			  DWORD cReaders);

/* Connection management functions */

/**
 * SCardConnect - Connect to a card
 *
 * @hContext: Context handle
 * @szReader: Reader name
 * @dwShareMode: SCARD_SHARE_EXCLUSIVE or SCARD_SHARE_SHARED
 * @dwPreferredProtocols: Preferred protocols (SCARD_PROTOCOL_T0 | SCARD_PROTOCOL_T1)
 * @phCard: Pointer to card handle
 * @pdwActiveProtocol: Pointer to active protocol
 *
 * Return: SCARD_S_SUCCESS on success, error code otherwise
 */
LONG SCardConnect(SCARDCONTEXT hContext,
		  const char *szReader,
		  SCARD_SHARE_MODE dwShareMode,
		  DWORD dwPreferredProtocols,
		  SCARDHANDLE *phCard,
		  DWORD *pdwActiveProtocol);

/**
 * SCardDisconnect - Disconnect from a card
 *
 * @hCard: Card handle
 * @dwDisposition: SCARD_LEAVE_CARD, SCARD_RESET_CARD, SCARD_UNPOWER_CARD, or SCARD_EJECT_CARD
 *
 * Return: SCARD_S_SUCCESS on success, error code otherwise
 */
LONG SCardDisconnect(SCARDHANDLE hCard,
		     SCARD_DISPOSITION dwDisposition);

/* Card communication functions */

/**
 * SCardTransmit - Transmit APDU command and receive response
 *
 * @hCard: Card handle
 * @pioSendPci: Protocol information (Input)
 * @pbSendBuffer: APDU command buffer
 * @cbSendLength: Command length
 * @pioRecvPci: Protocol information (Output)
 * @pbRecvBuffer: Response buffer
 * @pcbRecvLength: Response length
 *
 * Return: SCARD_S_SUCCESS on success, error code otherwise
 */
LONG SCardTransmit(SCARDHANDLE hCard,
		   const void *pioSendPci,
		   const BYTE *pbSendBuffer,
		   DWORD cbSendLength,
		   void *pioRecvPci,
		   BYTE *pbRecvBuffer,
		   DWORD *pcbRecvLength);

/**
 * SCardControl - Send control command to reader
 *
 * @hCard: Card handle
 * @dwControlCode: Control code
 * @pbSendBuffer: Send buffer
 * @cbSendLength: Send length
 * @pbRecvBuffer: Receive buffer
 * @cbRecvLength: Receive length
 * @lpBytesReturned: Bytes returned
 *
 * Return: SCARD_S_SUCCESS on success, error code otherwise
 */
LONG SCardControl(SCARDHANDLE hCard,
		  DWORD dwControlCode,
		  const BYTE *pbSendBuffer,
		  DWORD cbSendLength,
		  BYTE *pbRecvBuffer,
		  DWORD cbRecvLength,
		  DWORD *lpBytesReturned);

/**
 * SCardGetAttrib - Get card attribute
 *
 * @hCard: Card handle
 * @dwAttrId: Attribute ID
 * @pbAttr: Attribute buffer
 * @pcbAttrLen: Attribute length
 *
 * Return: SCARD_S_SUCCESS on success, error code otherwise
 */
LONG SCardGetAttrib(SCARDHANDLE hCard,
		    DWORD dwAttrId,
		    BYTE *pbAttr,
		    DWORD *pcbAttrLen);

/**
 * SCardSetAttrib - Set card attribute
 *
 * @hCard: Card handle
 * @dwAttrId: Attribute ID
 * @pbAttr: Attribute buffer
 * @cbAttrLen: Attribute length
 *
 * Return: SCARD_S_SUCCESS on success, error code otherwise
 */
LONG SCardSetAttrib(SCARDHANDLE hCard,
		    DWORD dwAttrId,
		    const BYTE *pbAttr,
		    DWORD cbAttrLen);

/* Error string functions */

/**
 * SCardGetErrorMessage - Get error message string
 *
 * @lError: Error code
 *
 * Return: Error message string
 */
const char *SCardGetErrorMessage(LONG lError);

#ifdef __cplusplus
}
#endif

#endif /* __LIBPCSC_SCARD_H__ */
