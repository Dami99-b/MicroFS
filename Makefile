CXX ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror -pthread
CPPFLAGS ?= -Iinclude
BUILD_DIR ?= build
SOURCES = src/block.cc src/block_cache.cc src/bloom_filter.cc src/db.cc src/env.cc \
	src/table.cc src/tick_store.cc src/version_set.cc src/wal.cc
TEST_SOURCES = $(SOURCES) tests/core_test.cc

.PHONY: all build test cli clean

all: test

build:
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(TEST_SOURCES) -o $(BUILD_DIR)/microfs_core_tests

cli:
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(SOURCES) tools/microfs_cli.cc -o $(BUILD_DIR)/microfs_cli

test: build
	$(BUILD_DIR)/microfs_core_tests

clean:
	rm -rf $(BUILD_DIR)
