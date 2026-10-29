CXX ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -pthread
CPPFLAGS ?= -Iinclude
BUILD_DIR ?= build
SOURCES = src/env.cc src/wal.cc tests/core_test.cc

.PHONY: all build test clean

all: test

build:
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(SOURCES) -o $(BUILD_DIR)/microfs_core_tests

test: build
	$(BUILD_DIR)/microfs_core_tests

clean:
	rm -rf $(BUILD_DIR)
