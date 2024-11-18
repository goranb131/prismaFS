# Compiler and flags
CC = clang
CFLAGS = -Wall -D_FILE_OFFSET_BITS=64 -I/usr/local/include
LDFLAGS = /usr/local/lib/libosxfuse.2.dylib

# Binary name
TARGET = prismafs

# Source file
SRC = prismafs.c

# Install directories
INSTALL_DIR = /usr/local/bin
MAN_DIR = /usr/local/share/man/man1

# Man page name
MANPAGE = prismafs.1

# Default target: Build the binary
all: $(TARGET)

# Rule to compile the binary
$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)
	@echo "Build complete: $(TARGET)"

# Install the binary and man page to the system path
install: $(TARGET) $(MANPAGE)
	@echo "Installing $(TARGET) to $(INSTALL_DIR)..."
	@mkdir -p $(INSTALL_DIR)
	@cp $(TARGET) $(INSTALL_DIR)/
	@chmod +x $(INSTALL_DIR)/$(TARGET)
	@echo "Installing $(MANPAGE) to $(MAN_DIR)..."
	@mkdir -p $(MAN_DIR)
	@cp $(MANPAGE) $(MAN_DIR)/
	@echo "Installation complete."

# Uninstall the binary and man page
uninstall:
	@echo "Removing $(TARGET) from $(INSTALL_DIR)..."
	@rm -f $(INSTALL_DIR)/$(TARGET)
	@echo "Removing $(MANPAGE) from $(MAN_DIR)..."
	@rm -f $(MAN_DIR)/$(MANPAGE)
	@echo "Uninstallation complete."

# Clean up build artifacts
clean:
	rm -f $(TARGET)
	@echo "Cleaned up build files."

# Run the binary for testing
run: all
	@echo "Running $(TARGET)..."
	@./$(TARGET) --help
