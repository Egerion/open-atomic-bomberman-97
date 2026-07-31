#!/usr/bin/env bash
# Pre-push gate: function complexity, as a RATCHET rather than a wall.
#
# The metric is clang-tidy's readability-function-cognitive-complexity, not raw
# McCabe cyclomatic complexity, and the difference is deliberate. Cognitive
# complexity charges NESTING — a branch three levels deep costs more than the
# same branch at the top — and charges nothing for a flat `switch` over an enum.
# That is the right bias for this codebase: it is full of faithful ports of the
# original's dispatch tables, which McCabe would score as catastrophic while a
# reader walks them without effort, and full of screen loops whose real problem
# is depth. What we want to push down is tangle, not arm count.
#
# Threshold 25 is clang-tidy's own default (roughly cyclomatic ~15).
#
# Why a ratchet: at the time this was written 70 of 597 functions were over the
# threshold, six of them past 100 and the worst at 251. Turning the check on as
# a hard gate would have blocked every push until all seventy were rewritten,
# and rewriting seventy functions in one change is how a refactor stops being
# reviewable. So complexity-baseline.txt records what was already there; a
# function in it may stay, but it may never get WORSE, and anything new must
# meet the threshold from its first line. The list only shrinks. Same shape as
# format.sh, which checks only the lines a push changes for the same reason.
#
# Usage:
#   scripts/complexity.sh            # check (the gate)
#   scripts/complexity.sh --update   # rewrite the baseline from the current tree
#
# --update is for AFTER a refactor lands: it re-records reality so improvements
# become the new ceiling. Never run it to make a red gate go green — that is
# precisely the ratchet failing open, and the diff will show a number going up.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

THRESHOLD=25
BASELINE="scripts/complexity-baseline.txt"
MODE="${1:-check}"

CT="$(command -v clang-tidy || true)"
if [ -z "$CT" ]; then
  # Both Program Files roots — Build Tools lands in the (x86) one. See lint.sh,
  # where searching only the 64-bit root failed the gate on a healthy machine.
  for root in "/c/Program Files/Microsoft Visual Studio" \
              "/c/Program Files (x86)/Microsoft Visual Studio"; do
    for cand in "$root/"*/*/VC/Tools/Llvm/x64/bin/clang-tidy.exe; do
      if [ -x "$cand" ]; then CT="$cand"; break 2; fi
    done
  done
fi
if [ -z "${CT:-}" ]; then
  echo "complexity: clang-tidy not found on PATH or under VS2022's LLVM tools." >&2
  echo "            Install the \"C++ Clang tools for Windows\" component, or" >&2
  echo "            put clang-tidy on PATH." >&2
  exit 1
fi

# Same include list as lint.sh, and the same hard failure when the FetchContent
# headers are missing: without them a TU fails to PARSE, and clang-tidy reports
# that as a cascade of nonsense rather than as a missing header. A half-blind
# run here would silently under-report complexity, which is worse than not
# running at all — it would let a new offender in while reading as clean.
BUILD_DIR="${BUILD_DIR:-build/windows-fetch}"
INCLUDES=(-Ilibs/sim/include -Ilibs/sim/src -Ilibs/assets/include \
          -Ilibs/audio/include -Ilibs/core/include -Ilibs/platform/include \
          -Ilibs/match/include -Ilibs/net/include -Ilibs/game/include)
missing=0
for dep_inc in "$BUILD_DIR/_deps/sdl3-src/include" \
               "$BUILD_DIR/_deps/ixwebsocket-src" \
               "$BUILD_DIR/_deps/nlohmann_json-src/include"; do
  if [ -d "$dep_inc" ]; then
    INCLUDES+=(-I"$dep_inc")
  else
    echo "complexity: ERROR: dependency headers not found: $dep_inc" >&2
    missing=1
  fi
done
if [ "$missing" -ne 0 ]; then
  echo "complexity: configure the windows-fetch preset at least once, or set" >&2
  echo "            BUILD_DIR=<path-to-an-existing-build-dir>." >&2
  exit 1
fi

CFG='{Checks: "-*,readability-function-cognitive-complexity", WarningsAsErrors: "", CheckOptions: [{key: readability-function-cognitive-complexity.Threshold, value: "'"$THRESHOLD"'"}, {key: readability-function-cognitive-complexity.IgnoreMacros, value: "true"}]}'

CURRENT="$(mktemp)"
RAW="$(mktemp)"
trap 'rm -f "$CURRENT" "$RAW"' EXIT

# --header-filter is what makes this cover the HEADER-ONLY components at all.
# clang-tidy suppresses diagnostics outside the main file by default, so the
# first version measured .cpp bodies only — and libs/core, libs/match and
# libs/platform have no .cpp at all, which left three whole components
# unmeasured while the gate reported a number that looked complete. A header
# included by many translation units is reported many times; the awk below keeps
# the maximum per (file, function), so the duplication costs time, not accuracy.
mapfile -t FILES < <(find libs apps -name "*.cpp" | grep -v "/build/")
echo "complexity: measuring ${#FILES[@]} files (threshold $THRESHOLD)..."
printf '%s\n' "${FILES[@]}" |
  xargs -P 8 -I{} bash -c '"$1" --quiet --header-filter="(libs|apps)/" --config="$2" "$3" -- -std=c++20 "${@:4}" 2>/dev/null' \
    _ "$CT" "$CFG" {} "${INCLUDES[@]}" > "$RAW"

# "<path>:<line>:<col>: warning: function 'NAME' has cognitive complexity of N"
# Keyed on file+function, NOT line: a line number moves every time something
# above it changes, which would make the baseline churn on unrelated edits.
# Overloads sharing a name in one file collapse to their maximum — accepted,
# since the gate's question is "did anything get worse here", not "which
# overload".
sed -n "s#^\(.*\):[0-9]\+:[0-9]\+: warning: function '\([^']*\)' has cognitive complexity of \([0-9]\+\).*#\3\t\1\t\2#p" "$RAW" |
  sed 's#\\#/#g' |
  sed 's#^\([0-9]*\)\t.*/\(libs\|apps\)/#\1\t\2/#' |
  awk -F'\t' '$2 ~ /^(libs|apps)\// { k=$2"\t"$3; if ($1+0 > m[k]) m[k]=$1+0 }
              END { for (k in m) print m[k]"\t"k }' |
  sort -k2 > "$CURRENT"

# Anchoring on the LAST /libs/ or /apps/ rather than on the repo's own name is
# load-bearing, and the first version got it wrong in a way that failed OPEN.
# It stripped up to the first "open-atomic-bomberman-97/", which is correct in
# the main checkout and wrong in a git worktree — those live UNDER the repo, at
# .claude/worktrees/agent-X/, so the surviving path began ".claude/..." and fell
# straight through the libs|apps filter. Every warning was discarded, the gate
# printed "OK - 0 known offender(s)" against a 66-entry baseline, and --update
# would have rewritten that baseline to nothing. A gate that reports success
# having measured zero files is the same failure this repo has already been
# bitten by three times (ENABLE_LOBBY, LOBBY_TLS, libs/audio) and it is why the
# self-check below exists: a run that finds no measurable file now says so and
# fails, instead of congratulating itself.
if [ ! -s "$CURRENT" ] && [ -s "$RAW" ]; then
  echo "complexity: ERROR: clang-tidy produced output but no path resolved under" >&2
  echo "            libs/ or apps/. The path normalisation is broken for this" >&2
  echo "            checkout layout; the gate would pass having measured nothing." >&2
  exit 1
fi

if [ "$MODE" = "--update" ]; then
  cp "$CURRENT" "$BASELINE"
  echo "complexity: baseline rewritten — $(wc -l < "$BASELINE") function(s) over $THRESHOLD."
  echo "            Review the diff: any number that went UP needs a reason."
  exit 0
fi

if [ ! -f "$BASELINE" ]; then
  echo "complexity: no $BASELINE — create it with: scripts/complexity.sh --update" >&2
  exit 1
fi

# .gitattributes normalises this file to CRLF in the working copy, and the two
# readers below disagree about that: awk (the Git Bash build) strips the CR,
# bash's `read` keeps it. That mismatch is invisible in the pass/fail loop, which
# reads the LF-only CURRENT, and it silently broke the `gone` counter, which
# reads the baseline — every entry failed to match, so a Windows run reported
# "65 offenders, 70 now under threshold", two numbers that cannot both be true.
# Cosmetic in isolation, except that the summary then appends "re-baseline with
# --update" to EVERY run, training the operator toward the one command this
# script's own header calls the way the ratchet fails open. Read one normalised
# copy instead of trusting two tools to agree about a line ending.
BASE_LF="$(mktemp)"
trap 'rm -f "$CURRENT" "$RAW" "$BASE_LF"' EXIT
tr -d '\r' < "$BASELINE" > "$BASE_LF"

fail=0
improved=0
while IFS=$'\t' read -r val file fn; do
  [ -z "${file:-}" ] && continue
  base="$(awk -F'\t' -v f="$file" -v n="$fn" '$2==f && $3==n { print $1 }' "$BASE_LF")"
  if [ -z "$base" ]; then
    echo "complexity: NEW over-threshold function: $fn ($val) in $file" >&2
    fail=1
  elif [ "$val" -gt "$base" ]; then
    echo "complexity: WORSE: $fn in $file went $base -> $val" >&2
    fail=1
  elif [ "$val" -lt "$base" ]; then
    improved=$((improved + 1))
  fi
done < "$CURRENT"

# A baselined function that dropped below the threshold vanishes from CURRENT
# entirely, so count those too — they are the ratchet actually turning.
gone=0
while IFS=$'\t' read -r val file fn; do
  [ -z "${file:-}" ] && continue
  if ! awk -F'\t' -v f="$file" -v n="$fn" '$2==f && $3==n { found=1 } END { exit !found }' "$CURRENT"; then
    gone=$((gone + 1))
  fi
done < "$BASE_LF"

if [ "$fail" -ne 0 ]; then
  echo "complexity: FAILED. Flatten it (guard clauses, early return, a named" >&2
  echo "            predicate, a helper) — or, if the shape is a faithful port of" >&2
  echo "            the original's dispatch, say so in a comment citing sub_XXXX" >&2
  echo "            and re-baseline deliberately with --update." >&2
  exit 1
fi

msg="complexity: OK — $(wc -l < "$CURRENT") known offender(s), none new, none worse"
if [ "$improved" -gt 0 ] || [ "$gone" -gt 0 ]; then
  msg="$msg ($improved improved, $gone now under threshold — re-baseline with --update)"
fi
echo "$msg."
