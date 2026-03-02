# PCSC-Lite Compatible Library Makefile

CC ?= gcc
CFLAGS ?= -Wall -Wextra -fPIC -pthread
CFLAGS += -I./include -I../include

# Directories
SRCDIR := src
TESTDIR := test
OBJDIR := .objs
LIBDIR := lib

# Source files
LIBSRC := $(SRCDIR)/libpcsc.c
LIBOBJ := $(OBJDIR)/libpcsc.o

TESTSRC := $(TESTDIR)/test_libpcsc.c $(TESTDIR)/sample_bcas_readcard.c
TESTOBJ := $(addprefix $(OBJDIR)/,$(notdir $(TESTSRC:.c=.o)))
TESTBIN := $(TESTDIR)/test_libpcsc $(TESTDIR)/sample_bcas_readcard

# Library targets
LIBSTATIC := $(LIBDIR)/libpcsc.a
LIBSHARED := $(LIBDIR)/libpcsc.so

# Targets
.PHONY: all clean install test

all: $(LIBSTATIC) $(LIBSHARED)

# Create directories
$(OBJDIR) $(LIBDIR):
	@mkdir -p $@

# Compile source files
$(OBJDIR)/libpcsc.o: $(SRCDIR)/libpcsc.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/%.o: $(TESTDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Static library
$(LIBSTATIC): $(LIBOBJ) | $(LIBDIR)
	ar rcs $@ $^
	@echo "Created static library: $@"

# Shared library
$(LIBSHARED): $(LIBOBJ) | $(LIBDIR)
	$(CC) -shared -o $@ $^ -lpthread
	@echo "Created shared library: $@"

# Test programs
$(TESTDIR)/test_libpcsc: $(OBJDIR)/test_libpcsc.o $(LIBOBJ)
	$(CC) $(CFLAGS) -o $@ $^ -lpthread
	@echo "Built test: $@"

$(TESTDIR)/sample_bcas_readcard: $(OBJDIR)/sample_bcas_readcard.o $(LIBOBJ)
	$(CC) $(CFLAGS) -o $@ $^ -lpthread
	@echo "Built sample: $@"

# Test targets
test: $(TESTBIN)
	@echo "\n=== Running unit tests ==="
	@./$(TESTDIR)/test_libpcsc

# Run sample (requires actual hardware)
sample: $(TESTDIR)/sample_bcas_readcard
	@echo "\n=== Running B-CAS sample (requires hardware) ==="
	@./$(TESTDIR)/sample_bcas_readcard

# Install targets (optional)
install: $(LIBSTATIC) $(LIBSHARED)
	install -d /usr/local/lib
	install -d /usr/local/include
	install -m 644 $(LIBSTATIC) /usr/local/lib/
	install -m 644 $(LIBSHARED) /usr/local/lib/
	install -m 644 include/scard.h /usr/local/include/
	@echo "Installed libpcsc to /usr/local"

uninstall:
	rm -f /usr/local/lib/libpcsc.a
	rm -f /usr/local/lib/libpcsc.so
	rm -f /usr/local/include/scard.h

# Clean
clean:
	rm -rf $(OBJDIR)
	rm -rf $(LIBDIR)
	rm -f $(TESTBIN)
	@echo "Cleaned build artifacts"

# Show help
help:
	@echo "libpcsc - PCSC-Lite Compatible Library"
	@echo ""
	@echo "Usage: make [target]"
	@echo ""
	@echo "Targets:"
	@echo "  all        - Build static and shared libraries (default)"
	@echo "  static     - Build static library only"
	@echo "  shared     - Build shared library only"
	@echo "  test       - Build and run unit tests"
	@echo "  sample     - Build and run B-CAS sample (requires hardware)"
	@echo "  install    - Install libraries and headers"
	@echo "  uninstall  - Uninstall libraries and headers"
	@echo "  clean      - Remove build artifacts"
	@echo "  help       - Show this help message"

# Additional targets
static: $(LIBSTATIC)

shared: $(LIBSHARED)

.PHONY: help
