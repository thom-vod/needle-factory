# Top-level convenience Makefile for needle.
#
# Wraps the canonical CMake build so that `make`, `make install`, and `make
# test` work identically on Linux, macOS, and Windows (MSYS2). All real build
# logic lives in CMakeLists.txt — this is just a thin delegator so contributors
# don't have to memorise the cmake invocations.
#
# Common targets:
#   make             -- configure (if needed) and build
#   make install     -- install to $(PREFIX) (default: cmake's platform default,
#                       i.e. /usr/local on Linux/macOS; ~/.local on Windows)
#   make test        -- run the test suite
#   make clean       -- remove build artefacts (keeps cmake cache)
#   make distclean   -- remove the entire build directory
#   make reconfigure -- nuke cmake cache and reconfigure
#
# Common overrides:
#   make PREFIX=~/.local install
#   sudo make install                     # /usr/local on Linux/macOS
#   make BUILD_TYPE=Debug
#   make GENERATOR="Unix Makefiles"
#   make CMAKE_FLAGS="-DNEEDLE_ASAN=ON"

BUILD_DIR  ?= build
BUILD_TYPE ?= Release
CMAKE_FLAGS ?= -DNEEDLE_BUILD_SERVER=ON

# PREFIX defaults to CMake's platform default (unset → /usr/local on Linux and
# macOS, matching the historical Unix convention) except on Windows, where
# CMake's default is `C:/Program Files/needle` and requires admin to install
# to. There we default to ~/.local.
ifeq ($(OS),Windows_NT)
PREFIX ?= $(HOME)/.local
endif

# Only inject the prefix flags when PREFIX is set. On Linux/macOS this lets
# CMake pick its own default (/usr/local).
PREFIX_CONFIGURE_FLAG := $(if $(PREFIX),-DCMAKE_INSTALL_PREFIX="$(PREFIX)")
PREFIX_INSTALL_FLAG   := $(if $(PREFIX),--prefix "$(PREFIX)")

# Parallel job count: nproc on Linux, sysctl on macOS, NUMBER_OF_PROCESSORS on
# Windows, fallback to 4.
JOBS ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo $${NUMBER_OF_PROCESSORS:-4})

# Generator selection. Prefer Ninja when present — it's faster and avoids the
# "MSYS Makefiles via /usr/bin/sh sets TMP=/tmp" trap on Windows that breaks
# native MinGW gcc. Fall back to the platform's native make generator.
NINJA := $(shell command -v ninja 2>/dev/null)
ifneq ($(NINJA),)
GENERATOR ?= Ninja
else ifeq ($(OS),Windows_NT)
GENERATOR ?= MSYS Makefiles
else
GENERATOR ?= Unix Makefiles
endif

CACHE := $(BUILD_DIR)/CMakeCache.txt

.PHONY: all build configure reconfigure install test clean distclean help

all: build

# Configure runs only when the cache doesn't exist yet. Use `make reconfigure`
# to force a re-run after changing PREFIX, GENERATOR, or CMAKE_FLAGS.
configure: $(CACHE)

$(CACHE):
	cmake -B $(BUILD_DIR) -G "$(GENERATOR)" \
	    -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
	    $(PREFIX_CONFIGURE_FLAG) \
	    $(CMAKE_FLAGS)

build: configure
	cmake --build $(BUILD_DIR) -j $(JOBS)

install: build
	cmake --install $(BUILD_DIR) $(PREFIX_INSTALL_FLAG)

test: build
	$(BUILD_DIR)/needle_tests

clean:
	@if [ -d $(BUILD_DIR) ]; then cmake --build $(BUILD_DIR) --target clean; fi

distclean:
	rm -rf $(BUILD_DIR)

reconfigure:
	rm -f $(CACHE)
	$(MAKE) configure

help:
	@echo "needle build targets:"
	@echo "  make             configure (if needed) and build"
	@echo "  make install     install to PREFIX"
	@echo "  make test        run the test suite"
	@echo "  make clean       remove build artefacts"
	@echo "  make distclean   remove the entire build directory ($(BUILD_DIR))"
	@echo "  make reconfigure force cmake to re-run configure"
	@echo
	@echo "Current settings:"
	@echo "  BUILD_DIR  = $(BUILD_DIR)"
	@echo "  PREFIX     = $(if $(PREFIX),$(PREFIX),(cmake default — /usr/local on Linux/macOS))"
	@echo "  BUILD_TYPE = $(BUILD_TYPE)"
	@echo "  GENERATOR  = $(GENERATOR)"
	@echo "  JOBS       = $(JOBS)"
