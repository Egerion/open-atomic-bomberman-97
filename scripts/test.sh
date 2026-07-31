#!/usr/bin/env bash
# Pre-push gate: build + run the full doctest suite via the "headless"
# preset (no SDL3 fetch/build), so this stays fast even though windows-fetch/
# windows-msvc are the presets used for day-to-day dev builds.
#
# "headless" means NO SDL — not "no dependencies". It builds the lobby layer
# too (BOMBER_ENABLE_LOBBY, which is SDL-free and only pulls a WS client and a
# JSON header), because a gate that skips shipped code is not a gate: the lobby
# suites sat outside it for a while, and a test broken by 790d876 reached main
# unnoticed as a direct result.
#
# The same reasoning added bomber::audio_core here: libs/audio was declared only
# inside the root CMakeLists' BOMBER_BUILD_VIEWER block, so this preset compiled
# NONE of it and the audio suites hid that by recompiling its sources into their
# own binaries. Its SDL-free half is now a real target built here.
#
# WHAT THIS GATE STILL CANNOT COVER, and cannot be made to: anything that
# includes SDL. libs/audio/src/audio_engine.cpp, all of libs/game, libs/platform
# and apps/game|viewer are compiled only by the SDL presets — CI's linux/macos/
# windows-fetch matrix (.github/workflows/c-cpp.yml) — and, locally, by whatever
# preset you dev against. scripts/lint.sh is the nearer net: it runs clang-tidy
# over every libs/apps .cpp including those, so it PARSES them, but with clang
# rather than MSVC and only if build/windows-fetch has been configured once.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

cmake --preset headless
cmake --build --preset headless
ctest --test-dir build/headless -C Release --output-on-failure
