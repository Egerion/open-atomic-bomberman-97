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

INCLUDES=(-Ilibs/sim/include -Ilibs/sim/src -Ilibs/assets/include \
          -Ilibs/match/include -Ilibs/game/include)
SDL_INC="build/windows-fetch/_deps/sdl3-src/include"
if [ -d "$SDL_INC" ]; then
  INCLUDES+=(-I"$SDL_INC")
else
  echo "lint: warning: $SDL_INC not found — build the windows-fetch preset" >&2
  echo "      at least once so SDL3 headers exist, or libs/game/apps/game*" >&2
  echo "      files will fail to parse." >&2
fi

mapfile -t FILES < <(find libs apps -name "*.cpp" | grep -v "/build/")
echo "lint: checking ${#FILES[@]} files with $(basename "$CT")..."
printf '%s\n' "${FILES[@]}" | xargs -P 8 -I{} "$CT" {} -- -std=c++20 "${INCLUDES[@]}"
