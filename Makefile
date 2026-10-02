BUILD_DIR ?= build
BUILD_TYPE ?= Release
JOBS ?= 2
CMAKE_ARGS ?=

.PHONY: all configure build test format-check gui

all: build

configure:
	cmake -S . -B "$(BUILD_DIR)" -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) $(CMAKE_ARGS)
	cmake -E create_symlink "$(BUILD_DIR)/compile_commands.json" compile_commands.json

build: configure
	cmake --build "$(BUILD_DIR)" --parallel $(JOBS)

test: build
	ctest --test-dir "$(BUILD_DIR)" --output-on-failure
	python3 test/test_build_wsl.py

format-check: configure
	cmake --build "$(BUILD_DIR)" --target format-check

gui: build
	./bin/backup-gui
