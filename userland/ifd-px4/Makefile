# Makefile for PX4 IFD Handler

CC = gcc
CFLAGS = -Wall -O2 -fPIC -I../../include -I/usr/include/PCSC
LDFLAGS = -shared

TARGET = libpx4ifd.so
SRCS = ifdhandler.c
OBJS = $(SRCS:.c=.o)

INSTALL_DIR = /usr/lib/pcsc/drivers/ifd-px4.bundle/Contents/Linux
PLIST_DIR   = /usr/lib/pcsc/drivers/ifd-px4.bundle/Contents
OLD_INSTALL_DIR = /usr/lib/pcsc/drivers/ifd-px4

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $<

install: $(TARGET)
	@if [ -e $(OLD_INSTALL_DIR) ]; then \
		rm -rf $(OLD_INSTALL_DIR); \
	fi
	install -d $(INSTALL_DIR)
	install -m 0755 $(TARGET) $(INSTALL_DIR)/
	install -m 0644 Info.plist $(PLIST_DIR)/

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all install clean
