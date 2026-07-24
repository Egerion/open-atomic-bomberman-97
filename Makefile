# Convenience wrapper around the CMake presets.
#
#   make run                      build everything, launch the game
#   make run SCHEME=<path.sch>    launch with a specific scheme
#   make viewer                   build & launch the animation viewer
#   make test                     build & run ctest
#   make survey                   validate all original assets (abtool)
#   make deploy                   assemble a runnable build (exe + SDL3) under dist/
#   make build / make clean
#
# GAME_DIR=<path> overrides game-install auto-detection for run/viewer/survey.
# Windows: needs GNU make (winget install ezwinports.make) + cmake on PATH.

DIST ?= dist

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
  DEPLOY_SDL_CMD := cmake -E copy "$(BIN)\SDL3.dll" "$(DIST)"
  DEPLOY_EXE := $(DIST)\bomber_game.exe
else
  BUILD_DIR ?= build/make
  CONFIGURE_CMD := cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release \
                   -DBOMBER_FETCH_SDL3=ON $(CMAKE_FLAGS)
  BUILD_CMD := cmake --build $(BUILD_DIR) -j
  GAME := $(BUILD_DIR)/bomber_game
  VIEWER := $(BUILD_DIR)/bomber_viewer
  ABTOOL := $(BUILD_DIR)/abtool
  CTEST_ARGS := --test-dir $(BUILD_DIR)
  DEPLOY_SDL_CMD := find $(BUILD_DIR) -name 'libSDL3.so*' -exec cp {} $(DIST)/ \;
  DEPLOY_EXE := $(DIST)/bomber_game
endif

CACHE := $(BUILD_DIR)/CMakeCache.txt

.DEFAULT_GOAL := run
.PHONY: run play viewer survey build test configure deploy clean help

run: build
	"$(GAME)" $(if $(GAME_DIR),"$(GAME_DIR)") $(if $(SCHEME),"$(SCHEME)")

# Deprecated alias for `run` (kept so old muscle-memory / docs keep working).
play: run

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

# Assemble a self-contained, runnable build: the Release exe + its SDL3 runtime
# library, copied into $(DIST)/ so the result can be zipped/shipped and launched
# directly. (Gameplay assets are still loaded at runtime from the player's own
# original install — clean-room, never bundled — so a deployed build auto-detects
# the install via BOMBER_GAME_DIR / gamedir.txt / the standard paths, same as a
# dev build.)
deploy: build
	cmake -E make_directory "$(DIST)"
	cmake -E copy "$(GAME)" "$(DIST)"
	$(DEPLOY_SDL_CMD)
	@cmake -E echo "Deployed a runnable build to $(DEPLOY_EXE)"

clean:
	cmake -E rm -rf build $(DIST)

help:
	@cmake -E echo "targets: run viewer survey build test configure deploy clean"
