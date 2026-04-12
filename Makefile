BINARY_NAME := dbc-dispatcher
BUILD_DIR := bin
VERSION := $(shell git describe --tags --always --dirty 2>/dev/null || echo "dev")
SRC := src/main.c

CROSS_CC := arm-linux-gnueabihf-gcc
HOST_CC := gcc

CFLAGS := -Wall -Wextra -Os -DVERSION=\"$(VERSION)\"
CROSS_PKG_CONFIG := arm-linux-gnueabihf-pkg-config
HOST_PKG_CONFIG := pkg-config

CROSS_CFLAGS := $(CFLAGS) $(shell $(CROSS_PKG_CONFIG) --cflags libsystemd hiredis 2>/dev/null)
CROSS_LDFLAGS := -static $(shell $(CROSS_PKG_CONFIG) --libs --static libsystemd hiredis 2>/dev/null)

HOST_CFLAGS := $(CFLAGS) $(shell $(HOST_PKG_CONFIG) --cflags libsystemd hiredis 2>/dev/null)
HOST_LDFLAGS := $(shell $(HOST_PKG_CONFIG) --libs libsystemd hiredis 2>/dev/null)

.PHONY: build build-host build-arm dist clean

build:
	mkdir -p $(BUILD_DIR)
	$(CROSS_CC) $(CROSS_CFLAGS) -o $(BUILD_DIR)/$(BINARY_NAME) $(SRC) $(CROSS_LDFLAGS)

build-arm: build

build-host:
	mkdir -p $(BUILD_DIR)
	$(HOST_CC) $(HOST_CFLAGS) -o $(BUILD_DIR)/$(BINARY_NAME) $(SRC) $(HOST_LDFLAGS)

dist: build
	strip $(BUILD_DIR)/$(BINARY_NAME)

clean:
	rm -rf $(BUILD_DIR)
