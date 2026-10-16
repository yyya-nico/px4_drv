# Makefile for PX4 IFD Handler

CC = gcc
CFLAGS ?= -Wall -O2
CPPFLAGS += -I../../include -I/usr/include/PCSC

TARGET = libpx4ifd.so
SRCS = ifdhandler.c
OBJS = $(SRCS:.c=.o)

INSTALL_DIR = /usr/lib/pcsc/drivers/ifd-px4.bundle/Contents/Linux
PLIST_DIR   = /usr/lib/pcsc/drivers/ifd-px4.bundle/Contents

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -shared -o $@ $^

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -c $<

install: $(TARGET)
	install -d "$(DESTDIR)$(INSTALL_DIR)"
	install -m 0644 $(TARGET) "$(DESTDIR)$(INSTALL_DIR)/"
	install -m 0644 Info.plist "$(DESTDIR)$(PLIST_DIR)/"

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all install clean
