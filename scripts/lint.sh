#!/usr/bin/env bash
# Pre-push gate: clang-tidy over every production source file (libs/, apps/;
# tests/ is excluded — doctest macros trip several of these checks and the
# suite's own green-ness is already enforced by test.sh). Checks and
# WarningsAsErrors live in the repo's .clang-tidy, picked up automatically by
# directory-walk discovery, so this script and the IDE stay in sync.
#
# No compile_commands.json: the MSVC "Visual Studio" generator used by every
# CMake preset here can't emit one (that needs Ninja/Makefiles). Include dirs
# are therefore hand-listed below to match libs/*/include layout instead of
# using -p <builddir>. If the project ever adds a Ninja-based preset, this
# can switch to a real compilation database for full per-target accuracy.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

CT="$(command -v clang-tidy || true)"
if [ -z "$CT" ]; then
  # Both Program Files roots: the IDE editions install under the 64-bit one,
  # but Build Tools — the IDE-less toolchain, which is all this repo needs —
  # lands in "Program Files (x86)". Searching only the former fails the gate on
  # a perfectly good machine. It fails LOUDLY (the exit below says so), so
  # nothing was ever linted by accident — but it still blocks a push for no
  # reason, which is how this was found.
  for root in "/c/Program Files/Microsoft Visual Studio" \
              "/c/Program Files (x86)/Microsoft Visual Studio"; do
    for cand in "$root/"*/*/VC/Tools/Llvm/x64/bin/clang-tidy.exe; do
      if [ -x "$cand" ]; then CT="$cand"; break 2; fi
    done
  done
fi
if [ -z "${CT:-}" ]; then
  echo "lint: clang-tidy not found on PATH or under VS2022's LLVM tools." >&2
  echo "      Install the \"C++ Clang tools for Windows\" component in the" >&2
  echo "      Visual Studio installer, or put clang-tidy on PATH." >&2
  exit 1
fi

# One -I per libs/*/include (keep in sync with the component list in CLAUDE.md's
# architecture map). audio/core/platform were split out after this list was first
# written; without them clang-tidy can't resolve bomber/audio, bomber/core
# (e.g. geometry.hpp, pulled in by sim/constants.hpp) or bomber/platform, and
# every dependent TU fails to parse rather than being linted.
INCLUDES=(-Ilibs/sim/include -Ilibs/sim/src -Ilibs/assets/include \
          -Ilibs/audio/include -Ilibs/core/include -Ilibs/platform/include \
          -Ilibs/match/include -Ilibs/net/include -Ilibs/game/include)
# The FetchContent dependency headers (SDL3 for libs/game + apps, and the online
# lobby's WS/JSON pair for libs/net). These are REQUIRED, not optional: without
# them the dependent TUs fail to PARSE, and clang-tidy reports a parse failure as
# a cascade of nonsense diagnostics ("unused variable" on a variable that is
# plainly used) rather than as "I could not find a header". That misleads badly —
# it cost an agent a debugging detour — so a missing dir is a hard error with the
# fix spelled out, never a warning we then bury under bogus findings.
#
# Note the hard-coded build/windows-fetch path: if you lint from a git worktree
# whose build dir is elsewhere, configure that preset there (or point BUILD_DIR
# at an existing one) instead of letting the run proceed half-blind.
BUILD_DIR="${BUILD_DIR:-build/windows-fetch}"
missing=0
for dep_inc in "$BUILD_DIR/_deps/sdl3-src/include" \
               "$BUILD_DIR/_deps/ixwebsocket-src" \
               "$BUILD_DIR/_deps/nlohmann_json-src/include"; do
  if [ -d "$dep_inc" ]; then
    INCLUDES+=(-I"$dep_inc")
  else
    echo "lint: ERROR: dependency headers not found: $dep_inc" >&2
    missing=1
  fi
done
if [ "$missing" -ne 0 ]; then
  echo "lint: configure the windows-fetch preset at least once so FetchContent" >&2
  echo "      has downloaded SDL3 + the lobby deps:" >&2
  echo "        cmake --preset windows-fetch" >&2
  echo "      or set BUILD_DIR=<path-to-an-existing-build-dir>." >&2
  exit 1
fi

mapfile -t FILES < <(find libs apps -name "*.cpp" | grep -v "/build/")
echo "lint: checking ${#FILES[@]} files with $(basename "$CT")..."
# Every file must be CHECKED even after one fails: a bare `xargs "$CT"` stops
# feeding new batches once an invocation fails (observed: a failing batch left
# dozens of files unchecked, so re-runs "passed" on a random subset). Each
# invocation is wrapped to always exit 0 and record failures; the gate then
# fails once, at the end, with the full failing-file list.
FAILLOG="$(mktemp)"
trap 'rm -f "$FAILLOG"' EXIT
printf '%s\n' "${FILES[@]}" |
  xargs -P 8 -I{} bash -c '"$1" "$2" -- -std=c++20 "${@:3}" || echo "$2" >> "$0"' \
    "$FAILLOG" "$CT" {} "${INCLUDES[@]}"
if [ -s "$FAILLOG" ]; then
  echo "lint: FAILED files:" >&2
  sort "$FAILLOG" >&2
  exit 1
fi
echo "lint: all ${#FILES[@]} files clean."
