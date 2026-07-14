# compiler and flags
CC = /usr/bin/clang
CFLAGS = -Wall -D_FILE_OFFSET_BITS=64 -I/usr/local/include
LDFLAGS = /usr/local/lib/libosxfuse.2.dylib

# binary name
TARGET = prismafs

# source file
SRC = prismafs.c

# install dirs
INSTALL_DIR = /usr/local/bin
MAN_DIR = /usr/local/share/man/man1

# man page name
MANPAGE = prismafs.1

# fefault target: Build the binary
all: $(TARGET)

# rule to compile the binary
$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)
	@echo "Build complete: $(TARGET)"

# install the binary and man page to the system path
install: $(TARGET) $(MANPAGE)
	@echo "Installing $(TARGET) to $(INSTALL_DIR)..."
	@mkdir -p $(INSTALL_DIR)
	@cp $(TARGET) $(INSTALL_DIR)/
	@chmod +x $(INSTALL_DIR)/$(TARGET)
	@echo "Installing $(MANPAGE) to $(MAN_DIR)..."
	@mkdir -p $(MAN_DIR)
	@cp $(MANPAGE) $(MAN_DIR)/
	@echo "Installation complete."

# uninstall the binary and man page
uninstall:
	@echo "Removing $(TARGET) from $(INSTALL_DIR)..."
	@rm -f $(INSTALL_DIR)/$(TARGET)
	@echo "Removing $(MANPAGE) from $(MAN_DIR)..."
	@rm -f $(MAN_DIR)/$(MANPAGE)
	@echo "Uninstallation complete."

# clean up build artifacts
clean:
	rm -f $(TARGET)
	@echo "Cleaned up build files."

# run binary for testing
run: all
	@echo "Running $(TARGET)..."
	@./$(TARGET) --help
