# Makefile for PX4 IFD Handler

CC = gcc
CFLAGS = -Wall -O2 -fPIC -I../../include -I/usr/include/PCSC
LDFLAGS = -shared

TARGET = libpx4ifd.so
SRCS = ifdhandler.c
OBJS = $(SRCS:.c=.o)

INSTALL_DIR = /usr/lib/pcsc/drivers/ifd-px4/Contents/Linux
OLD_INSTALL_DIR = /usr/lib/pcsc/drivers/ifd-px4.bundle

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

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all install clean
