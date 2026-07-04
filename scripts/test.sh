#!/usr/bin/env bash
# Pre-push gate: build + run the full doctest suite via the "headless"
# preset (no SDL3 fetch/build — every test binary only links bomber::sim,
# see tests/CMakeLists.txt), so this stays fast even though windows-fetch/
# windows-msvc are the presets used for day-to-day dev builds.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

cmake --preset headless
cmake --build --preset headless
ctest --test-dir build/headless -C Release --output-on-failure
