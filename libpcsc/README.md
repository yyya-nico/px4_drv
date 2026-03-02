# libpcsc - PCSC-Lite Compatible B-CAS Smart Card Library

A PCSC-Lite compatible userspace library for smart card communication via the px4_drv Linux driver, specifically designed for B-CAS (Broadcast Smart Card) applications.

## Overview

This library provides a standardized PCSC (PC/SC) interface for interacting with B-CAS smart cards connected through px4_drv compatible hardware (PX-MLT, PX-S1UR, ISDB2056, etc.).

### Features

- **PCSC-Lite Compatible API**: Standard `SCardEstablishContext()`, `SCardConnect()`, `SCardTransmit()`, etc.
- **B-CAS Card Support**: Read/write smart card data for Japanese ISDB-T/S digital broadcasts
- **Thread-Safe**: Mutex-protected context and card access
- **Linux Native**: Uses ioctl interface via px4_drv kernel module
- **Easy Integration**: Drop-in replacement for PCSC-Lite applications

## Requirements

### Build Time
- GCC or Clang
- POSIX-compliant system (Linux tested)
- pthreads library

### Runtime
- px4_drv kernel module loaded and device files accessible (e.g., `/dev/px4video0`)
- Appropriate permissions to access device files (typically requires root or udev rules)

## Building

### Build Static and Shared Libraries

```bash
cd libpcsc
make
```

### Build Only Static Library

```bash
make static
```

### Build Only Shared Library

```bash
make shared
```

### Run Unit Tests

```bash
make test
```

This runs basic functionality tests without requiring hardware.

### Run B-CAS Sample (Requires Hardware)

```bash
make sample
```

This builds and runs the sample B-CAS card reader. You must have:
- px4_drv device connected and kernel module loaded
- B-CAS card inserted in the reader
- Appropriate permissions

### Install Libraries (Optional)

```bash
sudo make install
```

Installs libraries to `/usr/local/lib/` and headers to `/usr/local/include/`.

### Uninstall

```bash
sudo make uninstall
```

## API Reference

### Context Management

```c
LONG SCardEstablishContext(SCARD_SCOPE scope,
                          const void *reserved1,
                          const void *reserved2,
                          SCARDCONTEXT *pContext);

LONG SCardReleaseContext(SCARDCONTEXT hContext);
```

### Reader Operations

```c
LONG SCardListReaders(SCARDCONTEXT hContext,
                     const char *mszGroups,
                     char *mszReaders,
                     DWORD *pcchReaders);

LONG SCardGetStatusChange(SCARDCONTEXT hContext,
                         DWORD dwTimeout,
                         struct scard_readerstate *rgReaderStates,
                         DWORD cReaders);
```

### Card Connection

```c
LONG SCardConnect(SCARDCONTEXT hContext,
                 const char *szReader,
                 SCARD_SHARE_MODE dwShareMode,
                 DWORD dwPreferredProtocols,
                 SCARDHANDLE *phCard,
                 DWORD *pdwActiveProtocol);

LONG SCardDisconnect(SCARDHANDLE hCard,
                    SCARD_DISPOSITION dwDisposition);
```

### Card Communication

```c
LONG SCardTransmit(SCARDHANDLE hCard,
                  const void *pioSendPci,
                  const BYTE *pbSendBuffer,
                  DWORD cbSendLength,
                  void *pioRecvPci,
                  BYTE *pbRecvBuffer,
                  DWORD *pcbRecvLength);
```

### Error Handling

```c
const char *SCardGetErrorMessage(LONG lError);
```

## Example Usage

```c
#include <libpcsc/scard.h>
#include <stdio.h>

int main(void)
{
    SCARDCONTEXT ctx = NULL;
    SCARDHANDLE card = NULL;
    
    // Establish context
    if (SCardEstablishContext(SCARD_SCOPE_SYSTEM, NULL, NULL, &ctx) 
        != SCARD_S_SUCCESS) {
        printf("Failed to establish context\n");
        return 1;
    }
    
    // List readers
    char readers[256] = {0};
    DWORD readers_len = sizeof(readers);
    SCardListReaders(ctx, NULL, readers, &readers_len);
    
    // Connect to card
    DWORD active_protocol;
    if (SCardConnect(ctx, readers, SCARD_SHARE_SHARED,
                     SCARD_PROTOCOL_T0, &card, &active_protocol)
        != SCARD_S_SUCCESS) {
        printf("Failed to connect to card\n");
        SCardReleaseContext(ctx);
        return 1;
    }
    
    // Transmit APDU command
    BYTE cmd[] = {0xB0, 0xA4, 0x04, 0x00, 0x07, 0xA0, 0x00, 0x00, 0x00, 0xB0, 0x01, 0x00};
    BYTE resp[256];
    DWORD resp_len = sizeof(resp);
    
    if (SCardTransmit(card, NULL, cmd, sizeof(cmd), NULL,
                     resp, &resp_len) != SCARD_S_SUCCESS) {
        printf("Failed to transmit APDU\n");
    }
    
    // Cleanup
    SCardDisconnect(card, SCARD_RESET_CARD);
    SCardReleaseContext(ctx);
    
    return 0;
}
```

## Architecture

### Layer Structure

```
Application (libpcsc API)
        ↓
libpcsc Library (libpcsc.c)
        ↓
ioctl Interface (PTX_BCAS_*)
        ↓
Kernel Driver (ptx_chrdev.c)
        ↓
Hardware Bridge (it930x.c)
        ↓
Smart Card (B-CAS)
```

### Reader Names

Readers are identified by names in the format: `PX4-BCAS:N` where N is the device index.

- `PX4-BCAS:0` → `/dev/px4video0`
- `PX4-BCAS:1` → `/dev/px4video1`
- etc.

## Implementation Notes

### Current Limitations

1. **Single Card per Reader**: Only one card can be connected per reader at a time
2. **B-CAS Only**: Currently optimized for B-CAS cards; other smart card types not fully tested
3. **T=0 Protocol**: T=1 and other protocols defined but not fully implemented
4. **No Real Timeout**: Timeout values are approximated using `usleep()`

### Future Enhancements

- [ ] ATR (Answer To Reset) parsing
- [ ] T=1 protocol support
- [ ] Proper timeout handling
- [ ] Multiple card support
- [ ] Better error reporting
- [ ] Windows support

## File Structure

```
libpcsc/
├── Makefile                 # Build configuration
├── README.md               # This file
├── include/
│   └── scard.h            # PCSC-Lite compatible header
├── src/
│   └── libpcsc.c          # Core library implementation
└── test/
    ├── test_libpcsc.c     # Unit tests
    └── sample_bcas_readcard.c  # B-CAS sample application
```

## Troubleshooting

### "No readers available"
- Check that px4_drv kernel module is loaded: `lsmod | grep px4_drv`
- Check device files exist: `ls -la /dev/px4video*`
- Check permissions: `sudo chmod 666 /dev/px4video*` (temporary)

### "Card not detected"
- Ensure B-CAS card is properly inserted
- Check reader is properly connected to the system
- Try `make sample` to test hardware directly

### Compilation Errors
- Ensure kernel headers are available
- Check that the px4_drv include files are accessible
- Verify pthread development libraries are installed

## Performance Considerations

- **Context Creation**: Automatically detects and opens all available px4_drv devices
- **Card Communication**: APDU commands are limited to 255 bytes total
- **Thread Safety**: All public functions are thread-safe via internal mutexes

## Standards and References

- PC/SC Part 3: Components for Inter-IC card based systems
- PCSC-Lite: https://pcsclite.apdu.fr/
- ISDB-T Specification: Digital Broadcast Service Specification

## License

GNU General Public License v2.0 - See LICENSE file for details

## Contributing

Contributions are welcome! Please ensure:
- Code follows existing style
- Tests pass: `make test`
- Builds without warnings: `make clean all`

## Support

For issues related to:
- **libpcsc**: File issues in this repository
- **px4_drv driver**: See https://github.com/tsukumijima/px4_drv
- **PCSC-Lite**: See https://pcsclite.apdu.fr/

## Version History

### 1.0.0 (Initial Release)
- Basic PCSC-Lite API implementation
- B-CAS card support
- Unit tests and sample application

---

**Last Updated**: 2026-03-03  
**Status**: Beta - Functional but may have edge cases
