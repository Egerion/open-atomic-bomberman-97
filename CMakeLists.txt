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
# libs/net. OFF by default so the headless CI gate + pure sim/net builds need no
# TLS backend; the GUI presets (windows-fetch/msvc, linux, macos) turn it ON.
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
add_subdirectory(apps/abtool)

enable_testing()
add_subdirectory(tests)

# SDL3 presentation layer + apps.
if(BOMBER_BUILD_VIEWER)
  include(cmake/BomberSDL3.cmake)
  add_subdirectory(libs/platform)  # engine-base (ADR-0008): must precede libs/game
  add_subdirectory(libs/audio)     # audio module (ADR-0008 stage 2): precedes game
  add_subdirectory(libs/game)
  add_subdirectory(apps/game)
  add_subdirectory(apps/viewer)
endif()
