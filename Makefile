# Portable entry points for the standalone CMake build.
# Override CMAKE_ARGS for dependency paths and CMAKE_GENERATOR if needed.
SOURCE_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
BUILD_DIR ?= $(SOURCE_DIR)/build-make
BUILD_TYPE ?= Release
JOBS ?= 4
CMAKE ?= cmake
CTEST ?= ctest
ifeq ($(OS),Windows_NT)
CMAKE_GENERATOR ?= MinGW Makefiles
else
CMAKE_GENERATOR ?= Unix Makefiles
endif

.DEFAULT_GOAL := all
.PHONY: all configure shaderstudio shadertoy_libretro test clean

all: shaderstudio shadertoy_libretro

configure:
	"$(CMAKE)" -S "$(SOURCE_DIR)" -B "$(BUILD_DIR)" -G "$(CMAKE_GENERATOR)" -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) $(CMAKE_ARGS)

shaderstudio: configure
	"$(CMAKE)" --build "$(BUILD_DIR)" --config $(BUILD_TYPE) --target wtfweg-shader-studio --parallel $(JOBS)

shadertoy_libretro: configure
	"$(CMAKE)" --build "$(BUILD_DIR)" --config $(BUILD_TYPE) --target shadertoy_libretro --parallel $(JOBS)

test: configure
	"$(CMAKE)" --build "$(BUILD_DIR)" --config $(BUILD_TYPE) --parallel $(JOBS)
	"$(CTEST)" --test-dir "$(BUILD_DIR)" -C $(BUILD_TYPE) --output-on-failure

clean:
	"$(CMAKE)" --build "$(BUILD_DIR)" --config $(BUILD_TYPE) --target clean
