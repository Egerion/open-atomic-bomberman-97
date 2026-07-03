cmake_minimum_required(VERSION 3.25)
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
add_subdirectory(libs/assets)
add_subdirectory(libs/sim)
add_subdirectory(libs/match)
add_subdirectory(apps/abtool)

enable_testing()
add_subdirectory(tests)

# SDL3 presentation layer + apps.
if(BOMBER_BUILD_VIEWER)
  include(cmake/BomberSDL3.cmake)
  add_subdirectory(libs/game)
  add_subdirectory(apps/game)
  add_subdirectory(apps/viewer)
endif()
