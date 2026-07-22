# compiler and flags
CC ?= cc
CFLAGS = -Wall -D_FILE_OFFSET_BITS=64

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

# source file
SRC = prismafs.c

# install dirs
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/share/man/man1


# man page name
MANPAGE = prismafs.1

# fefault target: Build the binary
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
	@install -m 644 $(MANPAGE) $(MANDIR)/$(MANPAGE)
	@echo "Installation complete."

# uninstall the binary and man page
uninstall:
	@echo "Removing $(TARGET) from $(BINDIR)..."
	@rm -f $(BINDIR)/$(TARGET)
	@echo "Removing $(MANPAGE) from $(MANDIR)..."
	@rm -f $(MANDIR)/$(MANPAGE)
	@echo "Uninstallation complete."

# clean up build artifacts
clean:
	rm -f $(TARGET)
	@echo "Cleaned up build files."

# run binary for testing
run: all
	@echo "Running $(TARGET)..."
	@./$(TARGET) -v