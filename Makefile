.PHONY: help build test run clean distclean reconfig format format-patch tidy \
        profile-build profile profile-analyze profile-clean

BUILD_DIR := build
PROFILE_BUILD_DIR := build-profile
PROFILING_DIR := profiling
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
	@echo "  profile-build   Configure+build the profiling runner (Release + -g)"
	@echo "  profile         Run callgrind on the runner -> $(PROFILING_DIR)/"
	@echo "  profile-analyze Annotate latest callgrind output -> reports/"
	@echo "  profile-clean   Remove callgrind outputs + reports"
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

# --- Profiling (callgrind/kcachegrind) --------------------------------------
# Separate Release build dir (never clobbers the Debug `build/`): -O3 for a
# realistic profile, -g + frame pointers so callgrind annotates source. clang-22
# kept so the Orthodoxy plugin still engages.
profile-build:
	@cmake -B $(PROFILE_BUILD_DIR) -S . \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_CXX_COMPILER=$(CXX) -DCMAKE_C_COMPILER=$(CC) \
		-DCMAKE_CXX_FLAGS_RELEASE="-O3 -g -fno-omit-frame-pointer"
	@cmake --build $(PROFILE_BUILD_DIR) --target minesweeper_profiler -j$(JOBS)

# Profile the dedicated runner (bounded pathological replay). Timestamped output
# + a stable `callgrind.out.minesweeper` symlink for downstream tools.
profile: profile-build
	@mkdir -p $(PROFILING_DIR)
	$(eval TS := $(shell date +%Y%m%d_%H%M%S))
	@echo "callgrind run (timestamp $(TS))..."
	valgrind --tool=callgrind \
		--callgrind-out-file=$(PROFILING_DIR)/callgrind.out.minesweeper.$(TS) \
		--dump-instr=yes --compress-pos=no --compress-strings=no \
		$(PROFILE_BUILD_DIR)/src/minesweeper_profiler
	@ln -sf callgrind.out.minesweeper.$(TS) $(PROFILING_DIR)/callgrind.out.minesweeper
	@echo "wrote $(PROFILING_DIR)/callgrind.out.minesweeper.$(TS)"

profile-analyze:
	@$(PROFILING_DIR)/scripts/analyze_profile.sh

profile-clean:
	@rm -rf $(PROFILING_DIR)/callgrind.out.* $(PROFILING_DIR)/reports/*.txt
	@echo "profiling artifacts removed"
