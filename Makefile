# compiler and flags
CC ?= cc
CFLAGS = -Wall -D_FILE_OFFSET_BITS=64 -Isrc

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
FUSE_CFLAGS := -I/usr/local/include
FUSE_LIBS := /usr/local/lib/libosxfuse.2.dylib
else
FUSE_CFLAGS := $(shell pkg-config --cflags fuse3 2>/dev/null)
FUSE_LIBS := $(shell pkg-config --libs fuse3 2>/dev/null)
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