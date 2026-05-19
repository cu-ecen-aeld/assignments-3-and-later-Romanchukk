# Makefile template for C programs
.PHONY: all clean help

# Cross-compilation support
CROSS_COMPILE ?=
CC ?= $(CROSS_COMPILE)gcc
CFLAGS ?= -Wall -Wextra -O1 -pthread
LDFLAGS ?= -pthread

# Target executable name
TARGET = aesdsocket

# Source files
SOURCES = aesdsocket.c

# Object files
OBJECTS = $(SOURCES:.c=.o)

# Default target
all: $(TARGET)
default: $(TARGET)

# Link object files to create executable
$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) $(OBJECTS) -o $@ $(LDFLAGS)

# Compile source files to object files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Clean build artifacts
clean:
	rm -f $(TARGET) $(OBJECTS)

# Help target
help:
	@echo "Available targets:"
	@echo "  all   - Build the $(TARGET) executable"
	@echo "  clean - Remove the executable and object files"
	@echo "  help  - Show this help message"
	@echo "Use CROSS_COMPILE=aarch64-none-linux-gnu- for cross-compilation"
