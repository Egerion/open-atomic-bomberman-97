#!/usr/bin/env bash
# Pre-push gate: clang-format over the lines this push ADDS OR CHANGES.
#
# .clang-format was advisory before this — lefthook ran clang-tidy and the test
# suite and nothing else — and it showed: at the time this was added, 170 of the
# repo's 333 .cpp/.hpp files did not match it, including an `if` in
# simulation.cpp whose body was never reindented when it was wrapped.
#
# WHY LINES, NOT FILES. Reformatting those 170 files is ~3.5k changed lines,
# which would bury every real change in the history it lands on, and this is a
# faithful port: several files deliberately lay code out to mirror the original's
# structure. So this checks only what a change TOUCHES, exactly as
# `git clang-format` would. (It does not use git-clang-format itself: that is a
# Python script, and the `python3` on a stock Windows PATH is the Microsoft Store
# stub, so the gate would fail on a perfectly good machine.) Pre-existing
# violations elsewhere in a file you edit do not block you; the lines you write
# do, so the repo converges instead of needing one big reformat commit.
#
# When clang-format's opinion would genuinely obscure a ported layout — an
# arithmetic table mirroring the binary's, say — wrap that block in
# `// clang-format off` / `// clang-format on` rather than skipping the gate.
#
# Usage: scripts/format.sh [base-commit]   (default: merge-base with origin/main)
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

CF="$(command -v clang-format || true)"
if [ -z "$CF" ]; then
  # Same probe as scripts/lint.sh: Build Tools (the IDE-less toolchain, all this
  # repo needs) installs under "Program Files (x86)", the IDE editions under the
  # 64-bit root. Searching only one fails the gate on a perfectly good machine.
  for root in "/c/Program Files/Microsoft Visual Studio" \
              "/c/Program Files (x86)/Microsoft Visual Studio"; do
    for cand in "$root/"*/*/VC/Tools/Llvm/x64/bin/clang-format.exe; do
      if [ -x "$cand" ]; then CF="$cand"; break 2; fi
    done
  done
fi
if [ -z "${CF:-}" ]; then
  echo "format: clang-format not found on PATH or under VS2022's LLVM tools." >&2
  echo "        Install the \"C++ Clang tools for Windows\" component in the" >&2
  echo "        Visual Studio installer, or put clang-format on PATH." >&2
  exit 1
fi

BASE="${1:-${BOMBER_FORMAT_BASE:-}}"
if [ -z "$BASE" ]; then
  for ref in origin/main main; do
    if git rev-parse --verify --quiet "$ref" >/dev/null; then
      BASE="$(git merge-base HEAD "$ref" 2>/dev/null || true)"
      [ -n "$BASE" ] && break
    fi
  done
fi
if [ -z "$BASE" ]; then
  echo "format: no origin/main or main to diff against; nothing to check." >&2
  exit 0
fi

# Deleted files have no worktree content to format; renames report their new path.
mapfile -t FILES < <(git diff --name-only --diff-filter=d "$BASE" -- \
                       '*.cpp' '*.hpp' | grep -v '^build/' || true)
if [ "${#FILES[@]}" -eq 0 ]; then
  echo "format: no C++ files changed since ${BASE:0:12}."
  exit 0
fi

fails=()
for f in "${FILES[@]}"; do
  [ -f "$f" ] || continue
  # Post-image hunk headers -> one --lines=A:B per changed range. `@@ -a,b +c,d @@`
  # (d omitted means 1; d == 0 is a pure deletion, which formats nothing).
  mapfile -t RANGES < <(git diff -U0 "$BASE" -- "$f" | awk '
    /^@@/ {
      n = split($3, p, ",");
      start = substr(p[1], 2) + 0;
      len = (n > 1 ? p[2] + 0 : 1);
      if (len > 0) printf "--lines=%d:%d\n", start, start + len - 1;
    }')
  [ "${#RANGES[@]}" -eq 0 ] && continue
  if ! "$CF" --dry-run --Werror "${RANGES[@]}" "$f" 2>&1; then
    fails+=("$f")
  fi
done

if [ "${#fails[@]}" -ne 0 ]; then
  echo "format: FAILED files (only the lines you changed are checked):" >&2
  printf '  %s\n' "${fails[@]}" >&2
  echo "format: fix with, per file:" >&2
  echo "        clang-format -i --lines=<first>:<last> <file>" >&2
  exit 1
fi
echo "format: ${#FILES[@]} changed file(s) clean on their changed lines."
