BINARY_NAME := dbc-dispatcher
BUILD_DIR := bin
VERSION := $(shell git describe --tags --always --dirty 2>/dev/null || echo "dev")
SRC := src/main.c

CROSS_CC := arm-linux-gnueabihf-gcc
CROSS_STRIP := arm-linux-gnueabihf-strip
HOST_CC := gcc

CFLAGS := -Wall -Wextra -Os -DVERSION=\"$(VERSION)\"
PKG_CONFIG := pkg-config
CROSS_PKG_CONFIG_ENV := PKG_CONFIG_LIBDIR=/usr/lib/arm-linux-gnueabihf/pkgconfig

CROSS_CFLAGS := $(CFLAGS) $(shell $(CROSS_PKG_CONFIG_ENV) $(PKG_CONFIG) --cflags libsystemd hiredis 2>/dev/null)
# The distro cross libhiredis records DT_NEEDED=libhiredis.so.1.1.0, a soname
# the Yocto image does not ship (it has libhiredis.so.1), so ARM binaries must
# take the static archive. libsystemd stays dynamic: libsystemd.so.0 matches.
CROSS_STATIC_LIBS := /usr/lib/arm-linux-gnueabihf/libhiredis.a
CROSS_LDFLAGS := $(shell $(CROSS_PKG_CONFIG_ENV) $(PKG_CONFIG) --libs libsystemd 2>/dev/null) $(CROSS_STATIC_LIBS)

HOST_CFLAGS := $(CFLAGS) $(shell $(PKG_CONFIG) --cflags libsystemd hiredis 2>/dev/null)
HOST_LDFLAGS := $(shell $(PKG_CONFIG) --libs libsystemd hiredis 2>/dev/null)

.PHONY: build build-host build-arm dist test clean

build:
	mkdir -p $(BUILD_DIR)
	$(CROSS_CC) $(CROSS_CFLAGS) -o $(BUILD_DIR)/$(BINARY_NAME) $(SRC) $(CROSS_LDFLAGS)

build-arm: build

build-host:
	mkdir -p $(BUILD_DIR)
	$(HOST_CC) $(HOST_CFLAGS) -o $(BUILD_DIR)/$(BINARY_NAME) $(SRC) $(HOST_LDFLAGS)

dist: build
	$(CROSS_STRIP) $(BUILD_DIR)/$(BINARY_NAME)

test:
	$(HOST_CC) $(HOST_CFLAGS) -o /tmp/dbc-dispatcher-media-test tests/media_test.c $(HOST_LDFLAGS)
	/tmp/dbc-dispatcher-media-test

clean:
	rm -rf $(BUILD_DIR)
