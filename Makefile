# Convenience wrapper around the CMake presets.
#
#   make run                      build everything, launch the game
#   make run SCHEME=<path.sch>    launch with a specific scheme
#   make viewer                   build & launch the animation viewer
#   make test                     build & run ctest
#   make survey                   validate all original assets (abtool)
#   make deploy                   install OPEN-BM95.exe into the game dir (runs in place)
#   make build / make clean
#
# GAME_DIR=<path> overrides game-install auto-detection for run/viewer/survey,
# and is also the default `make deploy` destination.
# Windows: needs GNU make (winget install ezwinports.make) + cmake on PATH.

# `make deploy` drops the runnable build straight into the game install so it
# runs in place next to the original assets — no dist/ staging. Your install
# lives wherever YOU put it, so nothing here is hard-coded: GAME_DIR (which
# run/viewer/survey also honour) wins, else the gitignored gamedir.txt that the
# game itself reads (libs/assets/src/install.cpp), else deploy stops and tells
# you. Override the destination alone with DEPLOY_DIR=<path>.
# The other targets need none of this — the game auto-detects the install via
# BOMBER_GAME_DIR, gamedir.txt, or the standard install paths.
GAMEDIR_FILE := $(if $(wildcard gamedir.txt),$(strip $(file < gamedir.txt)))
# The install every target that needs an EXPLICIT path uses. `run`/`viewer` don't
# — the game auto-detects — but `abtool survey` takes the dir as a required
# argument, and `deploy` has to be told where to write.
INSTALL_DIR := $(if $(GAME_DIR),$(GAME_DIR),$(GAMEDIR_FILE))
DEPLOY_DIR ?= $(INSTALL_DIR)

ifeq ($(OS),Windows_NT)
  PRESET ?= windows-fetch
  SHELL := cmd.exe
  .SHELLFLAGS := /C
  BUILD_DIR ?= build/$(PRESET)
  CONFIGURE_CMD := cmake --preset $(PRESET)
  BUILD_CMD := cmake --build --preset $(PRESET)
  BIN := $(subst /,\,$(BUILD_DIR))\Release
  GAME := $(BIN)\OPEN-BM95.exe
  VIEWER := $(BIN)\bomber_viewer.exe
  ABTOOL := $(BIN)\abtool.exe
  CTEST_ARGS := --test-dir $(BUILD_DIR) -C Release
  # Copy SDL3.dll next to the exe ONLY if a dynamic build produced one (the
  # default FetchContent preset links SDL3 statically, so no DLL exists).
  DEPLOY_SDL_CMD := if exist "$(BIN)\SDL3.dll" cmake -E copy "$(BIN)\SDL3.dll" "$(DEPLOY_DIR)"
  DEPLOY_EXE := $(DEPLOY_DIR)\OPEN-BM95.exe
else
  BUILD_DIR ?= build/make
  CONFIGURE_CMD := cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release \
                   -DBOMBER_FETCH_SDL3=ON $(CMAKE_FLAGS)
  BUILD_CMD := cmake --build $(BUILD_DIR) -j
  GAME := $(BUILD_DIR)/OPEN-BM95
  VIEWER := $(BUILD_DIR)/bomber_viewer
  ABTOOL := $(BUILD_DIR)/abtool
  CTEST_ARGS := --test-dir $(BUILD_DIR)
  # find matches nothing (and copies nothing) when SDL3 is linked statically.
  DEPLOY_SDL_CMD := find $(BUILD_DIR) -name 'libSDL3.so*' -exec cp {} "$(DEPLOY_DIR)/" \;
  DEPLOY_EXE := $(DEPLOY_DIR)/OPEN-BM95
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
ifeq ($(strip $(INSTALL_DIR)),)
	$(error survey needs your install: make survey GAME_DIR="<your BOMBRMAN install>", or put the path in gamedir.txt)
endif
	"$(ABTOOL)" survey "$(INSTALL_DIR)"

build: $(CACHE)
	$(BUILD_CMD)

$(CACHE):
	$(CONFIGURE_CMD)

configure:
	$(CONFIGURE_CMD)

test: build
	ctest $(CTEST_ARGS) --output-on-failure

# Install the runnable build DIRECTLY into the game directory so it launches in
# place beside the original assets: copy OPEN-BM95.exe (and SDL3.dll only if a
# dynamic build produced one — the default static link ships a single self-
# contained exe) into $(DEPLOY_DIR). Gameplay assets are still read at runtime
# from the player's own original install (clean-room, never bundled).
deploy: build
ifeq ($(strip $(DEPLOY_DIR)),)
	$(error deploy needs a destination: make deploy GAME_DIR="<your BOMBRMAN install>" (or DEPLOY_DIR=<path>))
endif
	cmake -E make_directory "$(DEPLOY_DIR)"
	cmake -E copy "$(GAME)" "$(DEPLOY_DIR)"
	$(DEPLOY_SDL_CMD)
	@cmake -E echo "Deployed OPEN-BM95 to $(DEPLOY_EXE)"

clean:
	cmake -E rm -rf build dist

help:
	@cmake -E echo "targets: run viewer survey build test configure deploy clean"
