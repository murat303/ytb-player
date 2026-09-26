HOST_BUILD_DIR := build/host
HOST_BIN := $(HOST_BUILD_DIR)/ytb_player_host
HOST_CXX := g++
HOST_CC := gcc
HOST_CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Iinclude -Ivendor/third_party -Ivendor/quickjs
HOST_LDLIBS := -lssl -lcrypto -pthread

HOST_CXX_SRCS := \
	src/common/app_paths.cpp \
	src/common/auth_store.cpp \
	src/common/content_locale.cpp \
	src/common/log.cpp \
	src/common/http_client.cpp \
	src/common/settings_store.cpp \
	src/common/youtube_catalog_service.cpp \
	src/common/youtube_resolver.cpp \
	src/common/throttling_decrypter.cpp \
	src/common/ump.cpp \
	src/common/sponsorblock.cpp \
	src/common/subtitles.cpp \
	src/host/main.cpp

# QuickJS powers the throttling (n-parameter) transform. It is C, so it must be
# built with the C compiler and linked into the C++ host binary.
QJS_DIR := vendor/quickjs
QJS_OBJS := \
	$(HOST_BUILD_DIR)/qjs/quickjs.o \
	$(HOST_BUILD_DIR)/qjs/dtoa.o \
	$(HOST_BUILD_DIR)/qjs/libregexp.o \
	$(HOST_BUILD_DIR)/qjs/libunicode.o

.PHONY: host test-ump clean-host

host: $(HOST_BIN)

test-ump: $(HOST_BUILD_DIR)/ump_test
	$(HOST_BUILD_DIR)/ump_test

$(HOST_BUILD_DIR)/ump_test: tests/ump_test.cpp src/common/ump.cpp include/newpipe/ump.hpp
	mkdir -p $(HOST_BUILD_DIR)
	$(HOST_CXX) $(HOST_CXXFLAGS) tests/ump_test.cpp src/common/ump.cpp -o $@

$(HOST_BUILD_DIR)/qjs/%.o: $(QJS_DIR)/%.c
	mkdir -p $(HOST_BUILD_DIR)/qjs
	$(HOST_CC) -O2 -w -I$(QJS_DIR) -c $< -o $@

$(HOST_BIN): $(HOST_CXX_SRCS) $(QJS_OBJS)
	mkdir -p $(HOST_BUILD_DIR)
	$(HOST_CXX) $(HOST_CXXFLAGS) $(HOST_CXX_SRCS) $(QJS_OBJS) -o $(HOST_BIN) $(HOST_LDLIBS)

clean-host:
	rm -rf $(HOST_BUILD_DIR)
