# Compiler and flags
CC = clang
CFLAGS = -Wall -D_FILE_OFFSET_BITS=64 -I/usr/local/include
LDFLAGS = /usr/local/lib/libosxfuse.2.dylib

# Binary name
TARGET = prismafs

# Source file
SRC = prismafs.c

# Install directory
INSTALL_DIR = /usr/local/bin

# Default target: Build the binary
all: $(TARGET)

# Rule to compile the binary
$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)
	@echo "Build complete: $(TARGET)"

# Install the binary to the system path
install: $(TARGET)
	@echo "Installing $(TARGET) to $(INSTALL_DIR)..."
	@mkdir -p $(INSTALL_DIR)
	@cp $(TARGET) $(INSTALL_DIR)/
	@chmod +x $(INSTALL_DIR)/$(TARGET)
	@echo "Installation complete."

# Uninstall the binary
uninstall:
	@echo "Removing $(TARGET) from $(INSTALL_DIR)..."
	@rm -f $(INSTALL_DIR)/$(TARGET)
	@echo "Uninstallation complete."

# Clean up build artifacts
clean:
	rm -f $(TARGET)
	@echo "Cleaned up build files."

# Run the binary for testing
run: all
	@echo "Running $(TARGET)..."
	@./$(TARGET) --help
