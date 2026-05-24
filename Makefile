.PHONY: help build test run clean distclean reconfig format format-patch tidy

BUILD_DIR := build
JOBS ?= $(shell nproc)
BUILD_TYPE ?= Debug

# Default to clang-22 so the Orthodoxy plugin engages. Override with CXX=.../CC=...
CXX ?= clang++-22
CC ?= clang-22

CMAKE_OPTS := -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
	-DCMAKE_CXX_COMPILER=$(CXX) -DCMAKE_C_COMPILER=$(CC)

.DEFAULT_GOAL := help

help:
	@echo "minesweeper-solver - Build Targets"
	@echo ""
	@echo "  build         Build the project"
	@echo "  test          Build and run unit tests"
	@echo "  run           Build and run the solver (original game + overlay)"
	@echo "  clean         Clean build artifacts"
	@echo "  distclean     Remove build directory"
	@echo "  reconfig      Reconfigure CMake build"
	@echo "  format        Run clang-format on sources"
	@echo "  format-patch  Check formatting (dry run)"
	@echo "  tidy          Run clang-tidy linter"
	@echo ""
	@echo "Variables: BUILD_TYPE=$(BUILD_TYPE) CXX=$(CXX) CC=$(CC) JOBS=$(JOBS)"

build: $(BUILD_DIR)/CMakeCache.txt
	@cmake --build $(BUILD_DIR) -j$(JOBS)

test: build
	@cd $(BUILD_DIR) && ctest --output-on-failure

run: build
	@$(BUILD_DIR)/src/minesweeper_solver

clean:
	@if [ -d $(BUILD_DIR) ]; then cmake --build $(BUILD_DIR) --target clean; fi

distclean:
	@rm -rf $(BUILD_DIR)

reconfig:
	@rm -rf $(BUILD_DIR)
	@cmake -B $(BUILD_DIR) -S . $(CMAKE_OPTS)

format: build
	@cmake --build $(BUILD_DIR) --target format

format-patch: build
	@cmake --build $(BUILD_DIR) --target format-patch

tidy: build
	@cmake --build $(BUILD_DIR) --target tidy

$(BUILD_DIR)/CMakeCache.txt:
	@cmake -B $(BUILD_DIR) -S . $(CMAKE_OPTS)
