# Convenience wrapper around the CMake presets.
#
#   make play                     build everything, launch the game
#   make play SCHEME=<path.sch>   launch with a specific scheme
#   make viewer                   build & launch the animation viewer
#   make test                     build & run ctest
#   make survey                   validate all original assets (abtool)
#   make build / make clean
#
# GAME_DIR=<path> overrides game-install auto-detection for play/viewer/survey.
# Windows: needs GNU make (winget install ezwinports.make) + cmake on PATH.

ifeq ($(OS),Windows_NT)
  PRESET ?= windows-fetch
  SHELL := cmd.exe
  .SHELLFLAGS := /C
  BUILD_DIR ?= build/$(PRESET)
  CONFIGURE_CMD := cmake --preset $(PRESET)
  BUILD_CMD := cmake --build --preset $(PRESET)
  BIN := $(subst /,\,$(BUILD_DIR))\Release
  GAME := $(BIN)\bomber_game.exe
  VIEWER := $(BIN)\bomber_viewer.exe
  ABTOOL := $(BIN)\abtool.exe
  CTEST_ARGS := --test-dir $(BUILD_DIR) -C Release
else
  BUILD_DIR ?= build/make
  CONFIGURE_CMD := cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release \
                   -DBOMBER_FETCH_SDL3=ON $(CMAKE_FLAGS)
  BUILD_CMD := cmake --build $(BUILD_DIR) -j
  GAME := $(BUILD_DIR)/bomber_game
  VIEWER := $(BUILD_DIR)/bomber_viewer
  ABTOOL := $(BUILD_DIR)/abtool
  CTEST_ARGS := --test-dir $(BUILD_DIR)
endif

CACHE := $(BUILD_DIR)/CMakeCache.txt

.DEFAULT_GOAL := play
.PHONY: play viewer survey build test configure clean help

play: build
	"$(GAME)" $(if $(GAME_DIR),"$(GAME_DIR)") $(if $(SCHEME),"$(SCHEME)")

viewer: build
	"$(VIEWER)" $(if $(GAME_DIR),"$(GAME_DIR)")

survey: build
	"$(ABTOOL)" survey $(if $(GAME_DIR),"$(GAME_DIR)","D:\Program Files (x86)\INTRPLAY\BOMBRMAN")

build: $(CACHE)
	$(BUILD_CMD)

$(CACHE):
	$(CONFIGURE_CMD)

configure:
	$(CONFIGURE_CMD)

test: build
	ctest $(CTEST_ARGS) --output-on-failure

clean:
	cmake -E rm -rf build

help:
	@cmake -E echo "targets: play viewer survey build test configure clean"
