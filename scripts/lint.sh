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
  for cand in "/c/Program Files/Microsoft Visual Studio/"*/*/VC/Tools/Llvm/x64/bin/clang-tidy.exe; do
    if [ -x "$cand" ]; then CT="$cand"; break; fi
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
SDL_INC="build/windows-fetch/_deps/sdl3-src/include"
if [ -d "$SDL_INC" ]; then
  INCLUDES+=(-I"$SDL_INC")
else
  echo "lint: warning: $SDL_INC not found — build the windows-fetch preset" >&2
  echo "      at least once so SDL3 headers exist, or libs/game/apps/game*" >&2
  echo "      files will fail to parse." >&2
fi

# The online lobby's FetchContent deps (ADR-0011, BOMBER_ENABLE_LOBBY): the WS
# client and the JSON header. Same treatment as SDL3 above — without them
# libs/net's lobby_client / lobby_messages / lobby_flow / stun_client fail to
# PARSE, which clang-tidy reports as a failure rather than skipping them.
for dep_inc in "build/windows-fetch/_deps/ixwebsocket-src" \
               "build/windows-fetch/_deps/nlohmann_json-src/include"; do
  if [ -d "$dep_inc" ]; then
    INCLUDES+=(-I"$dep_inc")
  else
    echo "lint: warning: $dep_inc not found — configure the windows-fetch preset" >&2
    echo "      (it fetches the lobby deps) or libs/net lobby files won't parse." >&2
  fi
done

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
