# compiler and flags
CC ?= cc
CFLAGS = -Wall -D_FILE_OFFSET_BITS=64 -Isrc

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
# macFUSE always installs to /usr/local including when homebrew prefix is
# /opt/homebrew. Override FUSE_PREFIX only for non-standard install.
FUSE_PREFIX ?= /usr/local
FUSE_CFLAGS := -I$(FUSE_PREFIX)/include
# macFUSE 4.0 renamed libosxfuse to libfuse. New installs might ship only
# libfuse, older ones only libosxfuse, and some one as symlink to the
# other. Link the first that exists by absolute path. Homebrew compiler
# strips -L flags, but not file arguments.
FUSE_LIBS := $(firstword $(wildcard \
	$(FUSE_PREFIX)/lib/libfuse.2.dylib \
	$(FUSE_PREFIX)/lib/libosxfuse.2.dylib \
	$(FUSE_PREFIX)/lib/libfuse.dylib \
	$(FUSE_PREFIX)/lib/libosxfuse_i64.2.dylib))
FUSE_MISSING_MSG := macFUSE not found under $(FUSE_PREFIX). Install it from https://macfuse.io
else
FUSE_CFLAGS := $(shell pkg-config --cflags fuse3 2>/dev/null)
FUSE_LIBS := $(shell pkg-config --libs fuse3 2>/dev/null)
FUSE_MISSING_MSG := libfuse3 not found by pkg-config. Install libfuse3 and its development headers
endif

# binary name
TARGET = prismafs

# source files
SRC = $(wildcard src/*.c)

# install dirs
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/share/man/man1


# man page name
MANPAGE = man/prismafs.1

# default target: Build the binary
all: $(TARGET)

# rule to compile the binary
$(TARGET): $(SRC)
	@if [ -z "$(FUSE_LIBS)" ]; then \
		echo "Error: $(FUSE_MISSING_MSG)" >&2; \
		exit 1; \
	fi
	$(CC) $(CFLAGS) $(FUSE_CFLAGS) -o $(TARGET) $(SRC) $(FUSE_LIBS)
	@echo "Build complete: $(TARGET)"

# install the binary and man page to the system path
install: $(TARGET) $(MANPAGE)
	@echo "Installing $(TARGET) to $(BINDIR)..."
	@mkdir -p $(BINDIR)
	@install -m 755 $(TARGET) $(BINDIR)/$(TARGET)
	@echo "Installing $(MANPAGE) to $(MANDIR)..."
	@mkdir -p $(MANDIR)
	@install -m 644 $(MANPAGE) $(MANDIR)/$(notdir $(MANPAGE))
	@echo "Installation complete."

# uninstall the binary and man page
uninstall:
	@echo "Removing $(TARGET) from $(BINDIR)..."
	@rm -f $(BINDIR)/$(TARGET)
	@echo "Removing $(MANPAGE) from $(MANDIR)..."
	@rm -f $(MANDIR)/$(notdir $(MANPAGE))
	@echo "Uninstallation complete."

# clean up build artifacts
clean:
	rm -f $(TARGET) $(TARGET)-static-linux
	@echo "Cleaned up build files."

# static-libfuse3 Linux build, avoid depending on the host libfuse3
# soname at runtime (glibc is dynamic, only libfuse3 static).
LIBFUSE3_STATIC ?= /usr/local/lib/libfuse3.a

static-linux: $(SRC)
	$(CC) $(CFLAGS) $(shell pkg-config --cflags fuse3) -o $(TARGET)-static-linux $(SRC) $(LIBFUSE3_STATIC) -pthread -ldl
	@echo "Build complete (static libfuse3): $(TARGET)-static-linux"

# run binary for testing
run: all
	@echo "Running $(TARGET)..."
	@./$(TARGET) -v