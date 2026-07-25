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
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

cmake --preset headless
cmake --build --preset headless
ctest --test-dir build/headless -C Release --output-on-failure
