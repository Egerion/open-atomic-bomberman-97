cmake_minimum_required(VERSION 3.25)

# Link the MSVC runtime STATICALLY, so the shipped exe runs on a clean Windows
# with nothing installed. SDL3 was already static, which made the build look
# self-contained — but the exe still imported MSVCP140 / VCRUNTIME140 /
# VCRUNTIME140_1 from the Visual C++ Redistributable, which Windows does NOT
# ship. Handing the single .exe to someone without that redistributable failed
# to launch, and because the game is a GUI-subsystem app the loader error is
# easy to miss. (The api-ms-win-crt-* imports are the Universal CRT and ARE part
# of Windows 10+, so those were never the problem.)
#
# Must precede project(): CMP0091 makes the runtime a target property that the
# compiler check itself consumes, and it has to apply to every FetchContent
# dependency too — mixing static and dynamic CRTs across SDL3/IXWebSocket/the
# game is a link error at best and a heap mismatch at worst.
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(open_bomberman LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# All executables land in one predictable place: build/<preset>/ (single
# config) or build/<preset>/Release|Debug (VS). The Makefile and the docs
# rely on these paths; without this, targets defined in apps/* would scatter
# into build/<preset>/apps/*/Release.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

option(BOMBER_BUILD_VIEWER "Build the SDL3 apps (game + asset viewer)" ON)
option(BOMBER_FETCH_SDL3 "Download and build SDL3 via FetchContent instead of find_package/vcpkg" OFF)
# Online lobby control plane (ADR-0011): pulls IXWebSocket (WS/TLS) into
# libs/net.
#
# OFF is the DEFAULT, not the recommendation, and no preset uses it: all five in
# CMakePresets.json pin it ON, `headless` included. The default exists so a
# consumer embedding libs/sim + libs/net for the deterministic core alone can
# opt out of the WS/JSON/TLS dependencies; it is not the configuration this repo
# builds or tests.
#
# `headless` in particular means NO SDL, not "no dependencies". It was OFF there
# and that was a hole in the pre-push gate, not a feature: the whole lobby half
# of tests/net (protocol, stun, flow, setup, match_config, tls) sits inside an
# `if(BOMBER_ENABLE_LOBBY)` block, so the gate silently skipped every one of them
# and a broken suite reached main. Turning it on costs one FetchContent fetch.
#
# BOMBER_LOBBY_TLS (cmake/BomberIXWebSocket.cmake) is pinned ON in the presets
# for a sharper reason — see that file. Short version: with TLS off,
# tests/net/test_lobby_tls.cpp compiles to its "this build has no TLS support"
# variant and PASSES, so the certificate- and hostname-verification cases — the
# only checks that separate a verifying client from one that trusts anything —
# never run, and the suite reports green having verified nothing.
option(BOMBER_ENABLE_LOBBY "Build the online lobby client (WebSocket/TLS via IXWebSocket)" OFF)

# Shared warning flags (bomber::warnings), linked PRIVATE by every target.
add_library(bomber_warnings INTERFACE)
add_library(bomber::warnings ALIAS bomber_warnings)
if(MSVC)
  target_compile_options(bomber_warnings INTERFACE /W4 /utf-8)
  target_compile_definitions(bomber_warnings INTERFACE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(bomber_warnings INTERFACE -Wall -Wextra)
endif()

# Headless components (no SDL): asset loaders, the deterministic sim, glue.
add_subdirectory(libs/core)  # SDL-free shared vocabulary (ADR-0008); no deps, must precede sim
add_subdirectory(libs/assets)
add_subdirectory(libs/sim)
add_subdirectory(libs/match)
# The lobby client's WS/TLS dependency (ixwebsocket target) — provided before
# libs/net consumes it, mirroring how cmake/BomberSDL3.cmake precedes libs/game.
if(BOMBER_ENABLE_LOBBY)
  include(cmake/BomberIXWebSocket.cmake)
endif()
add_subdirectory(libs/net)   # netplay: deterministic-lockstep input codec + UDP transport (ADR-0010) + online lobby client (ADR-0011); SDL-free, depends on sim
# SDL3 (SDL3::SDL3), hoisted above libs/audio because that directory declares
# BOTH halves of the audio module: an SDL-free bomber::audio_core built
# everywhere, and the SDL-linked bomber::audio built only when the viewer is on.
if(BOMBER_BUILD_VIEWER)
  include(cmake/BomberSDL3.cmake)
endif()
# libs/audio used to sit entirely inside the BOMBER_BUILD_VIEWER block below,
# which meant the `headless` preset — the one scripts/test.sh builds for the
# pre-push hook — compiled NONE of it, so a break anywhere in the module only
# surfaced on a windows-fetch/CI build. Adding it here puts its SDL-free half
# back inside the gate. It must precede add_subdirectory(tests): the audio
# suites link bomber::audio_core instead of recompiling its sources.
add_subdirectory(libs/audio)
add_subdirectory(apps/abtool)

# The presentation stack. What was one 13k-line libs/game — renderer, asset
# store, UI chrome, input, editor, every screen, the whole netplay front end and
# the app shell in a single target with 73 public headers — is eight packages,
# declared here in dependency order so the graph is visible from one screen:
#
#   game_util -> input, render -> ui -> netui -> editor -> frontend -> netplay -> game
#
# bomber::game_util is FIRST and is declared UNCONDITIONALLY, because it is the
# SDL-free floor: the front-end state machine, the pixel/pacing rules, the
# match and results bookkeeping, the editor's grid model, the log seam. The
# `headless` preset — the one scripts/test.sh builds for the pre-push hook —
# compiles all of it, so tests/game's suites LINK it instead of recompiling its
# sources into their own binaries. That is the shape libs/audio was fixed into
# and the shape three separate "green having verified nothing" gates were fixed
# out of; keeping the SDL-free half below the SDL line is what makes it hold.
#
# Everything after it needs SDL3, so each of those directories early-returns
# unless BOMBER_BUILD_VIEWER is on, and libs/platform — which the SDL half needs
# for the frame clock/pacer — must precede them whenever that half is built.
add_subdirectory(libs/game_util)
if(BOMBER_BUILD_VIEWER)
  add_subdirectory(libs/platform)  # engine-base (ADR-0008): must precede the SDL packages
  add_subdirectory(libs/input)     # keyboard/gamepad/DOS scancodes (SDL-free headers)
  add_subdirectory(libs/render)    # textures, sprites, sequences, the world renderer
  add_subdirectory(libs/ui)        # Screen/ScreenContext, .BM viewer, dialog chrome
  add_subdirectory(libs/netui)     # chat + setup-link + F3 panel: net widgets inside screens
  add_subdirectory(libs/editor)    # the .SCH scheme editor
  add_subdirectory(libs/frontend)  # every front-end screen + MatchRunner
  add_subdirectory(libs/netplay)   # lobby, connect, online match, runner
endif()
add_subdirectory(libs/game)        # the app shell (GameApp); early-returns without the viewer

enable_testing()
add_subdirectory(tests)

# The SDL3 apps. (libs/audio's and libs/game's SDL halves are declared above,
# under the same BOMBER_BUILD_VIEWER guard.)
if(BOMBER_BUILD_VIEWER)
  add_subdirectory(apps/game)
  add_subdirectory(apps/viewer)
endif()
