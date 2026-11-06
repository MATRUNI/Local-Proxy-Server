# ==============================================================================
# Sankram (संक्रम) — High-Performance C Reverse Proxy & Edge Gateway
# Universal Cross-Platform Makefile (Linux, macOS, BSD, Windows/MinGW)
# ==============================================================================

CC ?= gcc
TARGET = build/sankram
DEBUG_TARGET = build/sankram_debug
SMALL_TARGET = build/sankram_small

# Source, Header and Object files
SRCS = $(wildcard src/*.c)
HEADERS = $(wildcard include/*.h)
OBJS = $(patsubst src/%.c, build/obj/%.o, $(SRCS))
DEBUG_OBJS = $(patsubst src/%.c, build/obj_debug/%.o, $(SRCS))
SMALL_OBJS = $(patsubst src/%.c, build/obj_small/%.o, $(SRCS))

# Header includes
INCLUDES = -Iinclude

# Base compilation flags
BASE_CFLAGS = -Wall -Wextra -Werror -std=gnu11 $(INCLUDES)
CFLAGS ?= -O3 -flto -march=native $(BASE_CFLAGS)
DEBUG_CFLAGS = -g -O0 -fsanitize=address,undefined $(BASE_CFLAGS)
SMALL_CFLAGS = -Os -flto -march=native -ffunction-sections -fdata-sections $(BASE_CFLAGS)

# Platform detection
UNAME_S := $(shell uname -s 2>/dev/null || echo Windows)

ifeq ($(UNAME_S),Darwin)
    # macOS with Homebrew OpenSSL
    OPENSSL_DIR := $(shell brew --prefix openssl@3 2>/dev/null || brew --prefix openssl 2>/dev/null || echo /opt/homebrew/opt/openssl)
    CFLAGS = -O3 -flto $(BASE_CFLAGS) -I$(OPENSSL_DIR)/include
    LDFLAGS = -L$(OPENSSL_DIR)/lib -pthread -lssl -lcrypto
    DEBUG_LDFLAGS = -L$(OPENSSL_DIR)/lib -pthread -lssl -lcrypto -fsanitize=address,undefined
else ifeq ($(findstring MINGW,$(UNAME_S)),MINGW)
    # Windows MinGW / MSYS2
    TARGET = build/sankram.exe
    DEBUG_TARGET = build/sankram_debug.exe
    LDFLAGS = -pthread -lssl -lcrypto -lws2_32
    DEBUG_LDFLAGS = -pthread -lssl -lcrypto -lws2_32 -fsanitize=address,undefined
else
    # Linux / BSD
    LDFLAGS = -pthread -lssl -lcrypto
    DEBUG_LDFLAGS = -pthread -lssl -lcrypto -fsanitize=address,undefined
endif

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

# Default target
.PHONY: all
all: $(TARGET)

# Production build
$(TARGET): $(OBJS)
	@mkdir -p build logs
	@echo "  [LD]   $@"
	@$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) -o $@
	@echo "✔ Build complete: $@"

# Compile each C source into an object file
build/obj/%.o: src/%.c $(HEADERS)
	@mkdir -p build/obj
	@echo "  [CC]   $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# Debug build with AddressSanitizer
.PHONY: debug
debug: $(DEBUG_TARGET)

$(DEBUG_TARGET): $(DEBUG_OBJS)
	@mkdir -p build logs
	@echo "  [LD]   $@ (AddressSanitizer enabled)"
	@$(CC) $(DEBUG_CFLAGS) $(DEBUG_OBJS) $(DEBUG_LDFLAGS) -o $@
	@echo "✔ Debug build complete: $@"

build/obj_debug/%.o: src/%.c $(HEADERS)
	@mkdir -p build/obj_debug
	@echo "  [CC]   $< (debug)"
	@$(CC) $(DEBUG_CFLAGS) -c $< -o $@

# Compact/Embedded size-optimized build (-Os + dead-code elimination + strip)
.PHONY: small
small: $(SMALL_TARGET)

$(SMALL_TARGET): $(SMALL_OBJS)
	@mkdir -p build logs
	@echo "  [LD]   $@ (size-optimized & stripped)"
	@$(CC) $(SMALL_CFLAGS) $(SMALL_OBJS) $(LDFLAGS) -Wl,--gc-sections -s -o $@
	@echo "✔ Compact build complete: $@"

build/obj_small/%.o: src/%.c $(HEADERS)
	@mkdir -p build/obj_small
	@echo "  [CC]   $< (compact)"
	@$(CC) $(SMALL_CFLAGS) -c $< -o $@

# Run server
.PHONY: run
run: $(TARGET)
	@./$(TARGET) -p 17777 -t 8

# Run quick sanity test
.PHONY: test
test: $(TARGET)
	@echo "Verifying Sankram binary..."
	@./$(TARGET) -h
	@echo "✔ Sankram sanity check passed."

# Clean build artifacts
.PHONY: clean
clean:
	@echo "Cleaning build artifacts..."
	@rm -rf build/obj build/obj_debug build/obj_small $(TARGET) $(DEBUG_TARGET) $(SMALL_TARGET)
	@echo "✔ Clean done."

# Global system install
.PHONY: install
install: $(TARGET)
	@install -d $(DESTDIR)$(BINDIR)
	@install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/sankram
	@echo "✔ Installed $(TARGET) to $(DESTDIR)$(BINDIR)/sankram"

# Uninstall
.PHONY: uninstall
uninstall:
	@rm -f $(DESTDIR)$(BINDIR)/sankram
	@echo "✔ Removed $(DESTDIR)$(BINDIR)/sankram"
