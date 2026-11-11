# Makefile for PX4 IFD Handler

CC = gcc
CFLAGS ?= -Wall -O2
CPPFLAGS += -I../../include -I/usr/include/PCSC

TARGET = libpx4ifd.so
SRCS = ifdhandler.c
OBJS = $(SRCS:.c=.o)

PKG_CONFIG ?= pkg-config
PCSC_DRIVER_DIR ?= $(shell $(PKG_CONFIG) --variable=usbdropdir libpcsclite)
PLIST_DIR = $(PCSC_DRIVER_DIR)/ifd-px4.bundle/Contents
INSTALL_DIR = $(PLIST_DIR)/Linux

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -shared -o $@ $^

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -c $<

install: $(TARGET)
	@test -n "$(strip $(PCSC_DRIVER_DIR))" || { \
		echo "Cannot determine the pcscd driver directory. Set PCSC_DRIVER_DIR explicitly." >&2; \
		exit 1; \
	}
	install -d "$(DESTDIR)$(INSTALL_DIR)"
	install -m 0644 $(TARGET) "$(DESTDIR)$(INSTALL_DIR)/"
	install -m 0644 Info.plist "$(DESTDIR)$(PLIST_DIR)/"

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all install clean
