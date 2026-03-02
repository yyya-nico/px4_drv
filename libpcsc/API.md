# libpcsc API Reference

## Core API Functions

### Context Management

#### SCardEstablishContext
```c
LONG SCardEstablishContext(SCARD_SCOPE scope,
                          const void *reserved1,
                          const void *reserved2,
                          SCARDCONTEXT *pContext);
```

**Purpose**: Establish a PC/SC context to access smart card readers.

**Parameters**:
- `scope`: Context scope
  - `SCARD_SCOPE_USER` (0): User context
  - `SCARD_SCOPE_TERMINAL` (1): Terminal context  
  - `SCARD_SCOPE_SYSTEM` (2): System context
- `reserved1`: Reserved (must be NULL)
- `reserved2`: Reserved (must be NULL)
- `pContext`: Output pointer to context handle

**Returns**: 
- `SCARD_S_SUCCESS`: Success
- `SCARD_E_NO_MEMORY`: Insufficient memory
- `SCARD_E_NO_READERS_AVAILABLE`: No readers found

**Notes**:
- Must be called before any other libpcsc functions
- Each context maintains its own reader list
- Context should be released with `SCardReleaseContext()`

**Example**:
```c
SCARDCONTEXT ctx;
LONG ret = SCardEstablishContext(SCARD_SCOPE_SYSTEM, NULL, NULL, &ctx);
if (ret == SCARD_S_SUCCESS) {
    // Use context...
    SCardReleaseContext(ctx);
}
```

---

#### SCardReleaseContext
```c
LONG SCardReleaseContext(SCARDCONTEXT hContext);
```

**Purpose**: Release a PC/SC context and free associated resources.

**Parameters**:
- `hContext`: Context handle obtained from `SCardEstablishContext()`

**Returns**:
- `SCARD_S_SUCCESS`: Success
- `SCARD_E_INVALID_HANDLE`: Invalid context handle

**Notes**:
- Closes all open devices associated with the context
- All connected cards must be disconnected first
- After calling, `hContext` becomes invalid

---

### Reader Enumeration

#### SCardListReaders
```c
LONG SCardListReaders(SCARDCONTEXT hContext,
                     const char *mszGroups,
                     char *mszReaders,
                     DWORD *pcchReaders);
```

**Purpose**: Get list of available smart card readers.

**Parameters**:
- `hContext`: Valid context handle
- `mszGroups`: Reader group filter (current implementation ignores this)
- `mszReaders`: Output buffer for reader names (NULL to query length)
- `pcchReaders`: Input/output buffer size in bytes

**Returns**:
- `SCARD_S_SUCCESS`: Success
- `SCARD_E_INVALID_HANDLE`: Invalid context
- `SCARD_E_INVALID_PARAMETER`: Invalid parameter
- `SCARD_E_INVALID_VALUE`: Buffer too small

**Reader Name Format**: `PX4-BCAS:N`
- N = reader index (0, 1, 2, ...)
- Maps to device `/dev/px4videoN`

**Example**:
```c
char readers[256] = {0};
DWORD len = sizeof(readers);
LONG ret = SCardListReaders(ctx, NULL, readers, &len);

if (ret == SCARD_S_SUCCESS) {
    char *reader = readers;
    while (reader[0] != '\0') {
        printf("Found: %s\n", reader);
        reader += strlen(reader) + 1;
    }
}
```

---

#### SCardGetStatusChange
```c
LONG SCardGetStatusChange(SCARDCONTEXT hContext,
                         DWORD dwTimeout,
                         struct scard_readerstate *rgReaderStates,
                         DWORD cReaders);
```

**Purpose**: Wait for status change on specified readers.

**Parameters**:
- `hContext`: Valid context handle
- `dwTimeout`: Timeout in milliseconds (0 = return immediately)
- `rgReaderStates`: Array of reader state structures
- `cReaders`: Number of elements in array

**Returns**:
- `SCARD_S_SUCCESS`: Status change detected
- `SCARD_E_INVALID_HANDLE`: Invalid context
- `SCARD_E_INVALID_PARAMETER`: Invalid parameter

**Reader State Structure**:
```c
struct scard_readerstate {
    const char *reader;         // Reader name (e.g., "PX4-BCAS:0")
    void *user_data;            // User-defined data (unused)
    DWORD current_state;        // Current state (updated by function)
    DWORD event_state;          // Event state (updated by function)
    DWORD atr_len;              // ATR length
    BYTE atr[33];               // ATR bytes
};
```

**State Values**:
- `SCARD_UNKNOWN`: Unknown state
- `SCARD_ABSENT`: No card present
- `SCARD_PRESENT`: Card present
- `SCARD_POWERED`: Card powered on
- `SCARD_SPECIFIC`: Card specific state

**Example**:
```c
struct scard_readerstate state = {
    .reader = "PX4-BCAS:0",
    .current_state = SCARD_UNKNOWN
};

SCardGetStatusChange(ctx, 1000, &state, 1);

if (state.event_state & SCARD_PRESENT) {
    printf("Card is present\n");
}
```

---

### Card Connection

#### SCardConnect
```c
LONG SCardConnect(SCARDCONTEXT hContext,
                 const char *szReader,
                 SCARD_SHARE_MODE dwShareMode,
                 DWORD dwPreferredProtocols,
                 SCARDHANDLE *phCard,
                 DWORD *pdwActiveProtocol);
```

**Purpose**: Connect to a smart card.

**Parameters**:
- `hContext`: Valid context handle
- `szReader`: Reader name (from `SCardListReaders()`)
- `dwShareMode`: Sharing mode
  - `SCARD_SHARE_EXCLUSIVE` (1): Exclusive access
  - `SCARD_SHARE_SHARED` (2): Shared access
  - `SCARD_SHARE_DIRECT` (3): Direct access
- `dwPreferredProtocols`: Preferred protocol(s)
  - `SCARD_PROTOCOL_T0` (0x0001): T=0
  - `SCARD_PROTOCOL_T1` (0x0002): T=1
  - `SCARD_PROTOCOL_RAW` (0x0004): Raw mode
  - Combine with bitwise OR for multiple protocols
- `phCard`: Output pointer to card handle
- `pdwActiveProtocol`: Output active protocol (optional)

**Returns**:
- `SCARD_S_SUCCESS`: Successfully connected
- `SCARD_E_INVALID_HANDLE`: Invalid context
- `SCARD_E_READER_UNAVAILABLE`: Reader not available
- `SCARD_E_NO_CARD`: No card in reader
- `SCARD_E_PROTO_MISMATCH`: Protocol mismatch

**Notes**:
- Automatically resets the card upon connection
- Only one connection per reader is supported
- B-CAS cards typically use T=0 protocol

**Example**:
```c
SCARDHANDLE card;
DWORD active_protocol;

LONG ret = SCardConnect(ctx, "PX4-BCAS:0",
                       SCARD_SHARE_SHARED,
                       SCARD_PROTOCOL_T0,
                       &card, &active_protocol);

if (ret == SCARD_S_SUCCESS) {
    printf("Connected, protocol: 0x%X\n", active_protocol);
}
```

---

#### SCardDisconnect
```c
LONG SCardDisconnect(SCARDHANDLE hCard,
                    SCARD_DISPOSITION dwDisposition);
```

**Purpose**: Disconnect from a smart card.

**Parameters**:
- `hCard`: Card handle from `SCardConnect()`
- `dwDisposition`: Action on disconnect
  - `SCARD_LEAVE_CARD` (0): Leave card as is
  - `SCARD_RESET_CARD` (1): Reset card
  - `SCARD_UNPOWER_CARD` (2): Unpower card
  - `SCARD_EJECT_CARD` (3): Eject card (if supported)

**Returns**:
- `SCARD_S_SUCCESS`: Successfully disconnected
- `SCARD_E_INVALID_HANDLE`: Invalid card handle

**Example**:
```c
SCardDisconnect(card, SCARD_RESET_CARD);
```

---

### Card Communication

#### SCardTransmit
```c
LONG SCardTransmit(SCARDHANDLE hCard,
                  const void *pioSendPci,
                  const BYTE *pbSendBuffer,
                  DWORD cbSendLength,
                  void *pioRecvPci,
                  BYTE *pbRecvBuffer,
                  DWORD *pcbRecvLength);
```

**Purpose**: Send APDU command to card and receive response.

**Parameters**:
- `hCard`: Valid card handle from `SCardConnect()`
- `pioSendPci`: Send protocol information (NULL = use negotiated)
- `pbSendBuffer`: APDU command buffer
- `cbSendLength`: Command length (max 255 bytes)
- `pioRecvPci`: Receive protocol information (ignored)
- `pbRecvBuffer`: Response buffer
- `pcbRecvLength`: Input/output response buffer length

**Returns**:
- `SCARD_S_SUCCESS`: APDU transmitted and response received
- `SCARD_E_INVALID_HANDLE`: Invalid card handle
- `SCARD_E_INVALID_PARAMETER`: Invalid parameter
- `SCARD_E_INVALID_VALUE`: Invalid value
- `SCARD_E_TIMEOUT`: Communication timeout
- `SCARD_E_COMM_ERROR`: Communication error

**APDU Format**:
```
[0] CLA - Class byte
[1] INS - Instruction byte
[2] P1  - Parameter 1
[3] P2  - Parameter 2
[4] LC  - Data length (optional)
[5..] Data (optional)
```

**Response Format**:
```
[0..n-2] Response data
[n-1]    SW1 - Status word 1
[n]      SW2 - Status word 2
```

**Example**:
```c
// SELECT application
BYTE cmd[] = {
    0xB0,                      // CLA: B-CAS class
    0xA4,                      // INS: SELECT
    0x04,                      // P1: By AID
    0x00,                      // P2
    0x07,                      // Lc: 7 bytes
    0xA0, 0x00, 0x00, 0x00,   // AID
    0xB0, 0x01, 0x00
};

BYTE resp[256];
DWORD resp_len = sizeof(resp);

LONG ret = SCardTransmit(card, NULL, cmd, sizeof(cmd),
                        NULL, resp, &resp_len);

if (ret == SCARD_S_SUCCESS) {
    printf("Response: ");
    for (int i = 0; i < resp_len; i++)
        printf("%02X ", resp[i]);
    printf("\n");
}
```

---

#### SCardGetAttrib / SCardSetAttrib
```c
LONG SCardGetAttrib(SCARDHANDLE hCard,
                   DWORD dwAttrId,
                   BYTE *pbAttr,
                   DWORD *pcbAttrLen);

LONG SCardSetAttrib(SCARDHANDLE hCard,
                   DWORD dwAttrId,
                   const BYTE *pbAttr,
                   DWORD cbAttrLen);
```

**Purpose**: Get/Set card attributes.

**Current Status**: Not implemented (returns `SCARD_E_NOT_READY`)

---

### Error Handling

#### SCardGetErrorMessage
```c
const char *SCardGetErrorMessage(LONG lError);
```

**Purpose**: Get human-readable error message for an error code.

**Parameters**:
- `lError`: Error code returned by other libpcsc functions

**Returns**: Pointer to error message string (static)

**Example**:
```c
LONG ret = SCardConnect(...);
if (ret != SCARD_S_SUCCESS) {
    printf("Error: %s (0x%08lX)\n", 
           SCardGetErrorMessage(ret), 
           (unsigned long)ret);
}
```

---

## Error Codes

| Code | Name | Meaning |
|------|------|---------|
| 0x00000000 | SCARD_S_SUCCESS | Operation successful |
| 0x80100003 | SCARD_E_INVALID_HANDLE | Invalid handle |
| 0x80100004 | SCARD_E_INVALID_PARAMETER | Invalid parameter |
| 0x80100006 | SCARD_E_NO_MEMORY | Not enough memory |
| 0x80100001 | SCARD_E_SYSTEM_CANCELLED | System cancelled operation |
| 0x8010000A | SCARD_E_TIMEOUT | Operation timeout |
| 0x8010000B | SCARD_E_SHARING_VIOLATION | Sharing violation |
| 0x8010000C | SCARD_E_NO_CARD | No card in reader |
| 0x8010000F | SCARD_E_PROTO_MISMATCH | Protocol mismatch |
| 0x80100010 | SCARD_E_NOT_READY | Device not ready |
| 0x1D | SCARD_E_NO_SERVICE | Service not available |
| 0x2E | SCARD_E_NO_READERS_AVAILABLE | No readers available |

---

## Data Types and Constants

### Basic Types
```c
typedef int32_t LONG;
typedef uint8_t BYTE;
typedef uint32_t DWORD;
typedef void *SCARDCONTEXT;
typedef void *SCARDHANDLE;
```

### Scope Values
```c
typedef enum {
    SCARD_SCOPE_USER = 0,      // User scope
    SCARD_SCOPE_TERMINAL = 1,  // Terminal scope
    SCARD_SCOPE_SYSTEM = 2     // System scope
} SCARD_SCOPE;
```

### Share Modes
```c
typedef enum {
    SCARD_SHARE_EXCLUSIVE = 1, // Exclusive access
    SCARD_SHARE_SHARED = 2,    // Shared access
    SCARD_SHARE_DIRECT = 3     // Direct access
} SCARD_SHARE_MODE;
```

### Dispositions
```c
typedef enum {
    SCARD_LEAVE_CARD = 0,      // Leave card as is
    SCARD_RESET_CARD = 1,      // Reset card
    SCARD_UNPOWER_CARD = 2,    // Unpower card
    SCARD_EJECT_CARD = 3       // Eject card
} SCARD_DISPOSITION;
```

---

## Compatibility Notes

This implementation is designed to be PCSC-Lite compatible for common operations. However:

1. **Reader Names**: Use format `PX4-BCAS:N` instead of system-specific names
2. **Protocols**: T=0 is fully supported; T=1 and other protocols may have limited support
3. **Timeouts**: Approximate via `usleep()`; not precise kernel-level timeouts
4. **Attributes**: `SCardGetAttrib()` and `SCardSetAttrib()` not implemented
5. **Multiple Connections**: Only one connection per reader supported

For full PCSC-Lite compatibility, refer to: https://pcsclite.apdu.fr/

---

## See Also

- [README.md](README.md) - Usage guide and examples
- [libpcsc.c](src/libpcsc.c) - Implementation source
- [scard.h](include/scard.h) - Header file
