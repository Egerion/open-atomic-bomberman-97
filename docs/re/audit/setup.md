# Fidelity audit — match setup (system 10, W3): `libs/sim/src/setup.cpp` (`build_state`) + `libs/match/include/bomber/match/match_factory.hpp`

> **CORRECTION 2026-07-28.** Finding 2's closing note ("`born_with` (scheme)
> and `start_with` (VALUELST) ARE two independent, additive mechanisms") is
> **wrong and retracted** — see the struck-through paragraph at the end of
> that section for what the binary actually does and why the reasoning
> failed. Finding 2's own subject (the incompletely-wired baseline) is
> unaffected. The rest of this document stands.

**Verdict: mostly faithful, two new findings + one confirmed cross-reference.**
Finding 2 below (starting-inventory baselines) turns out to be the SAME gap
`docs/re/audit/powerups.md` §1 already flagged from the other side (its own
"Suggested fix" explicitly says "flagged for the setup/scheme audit (ledger
item #10)" — this is that follow-up). It is written up here in full both to
independently re-derive it from the native transliteration (it was) and to
resolve the one question that audit left open — see Finding 2's closing
note. The RNG-consuming steps
that were already the subject of prior audit passes — brick fill, the
Random-Start spawn shuffle, the dud-gate arm, the round-start input freeze,
born-with/Goldman-wheel overlays, and the (deliberately unpinned)
spawn-pocket clear — were independently re-derived here from
`native/src/game/batch_0x42583B.cpp` (`sub_4260F5` board build, `sub_4258E5`
powerup scatter), `native/src/game/batch_0x420D4E.cpp` (`sub_4214BC` player
placement, `sub_421793` Random Start shuffle, `sub_4213E0` match init) and
`native/src/game/batch_0x421E80.cpp` (`sub_422C13`/`sub_422C7A` dud gate,
`sub_421F7E` — confirmed to be the **head-hit** handler, not a round-start
mechanic despite that batch file's own header comment; out of scope here,
already covered by `docs/re/facts.md` "Head hit"), and all match the
existing citations line for line. No changes suggested there — see
"Verified faithful" below.

A fresh, line-by-line read of the two functions the existing docs cite but
had not fully transcribed arithmetic-for-arithmetic — `sub_4214BC`'s
per-player field writes and `sub_4258E5`'s powerup-under-brick scatter —
turned up three real, previously unflagged deviations:

1. **The powerup-under-brick scatter uses a completely different placement
   algorithm** than the original (list-removal vs. rejection-sampling),
   with a different RNG draw count/pattern and a different failure mode on
   sparse boards. Severity: **Medium**, Confidence: **High**. (New.)
2. **Only two of the fifteen per-kind starting-inventory baselines
   (ExtraBomb, Flame) are actually applied to a fresh player.** The other
   eleven kinds' baselines are parsed into `Tuning::start_with[]` but never
   written to `Player` at setup. Severity: **Low** on stock data (every
   baseline but bomb/flame is 0), **Medium** on a customized VALUELST.
   Confidence: **High**. (**Already flagged** by `audit/powerups.md` §1 —
   confirmed independently here, plus its one open question resolved.)
3. **Spawn-coordinate range handling uses `std::clamp` instead of the
   original's asymmetric wrap-negative/clamp-positive-overflow rule.**
   Severity: **Low** (only reachable with an out-of-range `.SCH` `-S` row).
   Confidence: **High** on the literal reading, **Low** on real-world
   reachability (no shipped scheme trips it).

Scope: `libs/sim/src/setup.cpp` (`build_state`) and
`libs/match/include/bomber/match/match_factory.hpp`
(`build_match_config`/`apply_actors`/`pick_stage`) only. The spawn-pocket
clear is explicitly **not** re-opened per the task brief — it was
freshly re-searched and re-pinned as NOT PINNED just one session ago
(`docs/re/facts.md` "Spawn-pocket clear", 2026-07-19); nothing in this pass
changes that conclusion or adds new evidence either way.

References read in full: `native/src/game/batch_0x42583B.cpp` (`sub_4260F5`,
`sub_4258E5`, and the tile/grid helpers `sub_425E36`/`sub_425FB9`/
`sub_426524`/`sub_42655F`/`sub_426599`/`sub_4265EB`/`sub_42665C`/
`sub_4266A3`), `native/src/game/batch_0x420D4E.cpp` (`sub_4214BC`,
`sub_421793`, `sub_4213E0`, and — to establish call order — the
`sub_410F81`-shaped block at lines 850-930), `native/src/game/
batch_0x410401.cpp` (the per-round init caller at lines 597-617, confirming
the `sub_4260F5 → sub_42633C → sub_4214BC → sub_4258E5 → sub_40551F →
sub_40151B` call order and that `sub_421793` is **not** in it — called once
per MATCH from a different, earlier function, not once per ROUND),
`native/src/game/batch_0x421E80.cpp` (`sub_421F7E`, `sub_422C13`,
`sub_422C7A`, `sub_422351`), `native/src/game/batch_0x41F29B.cpp` (lines
200-210 and 530-575, to confirm indices 5/6 vs 7/8 are the spawn-anchor vs.
live-position fields and that the live position is snapped from the anchor
on activation — resolving a question this pass raised about
`sub_4214BC`'s field writes, see "Verified faithful" #4), `docs/re/facts.md`
("Round-start input freeze", "Screen geometry / field placement", "Per-match
brick fill", "Spawn-pocket clear", "Head hit"), `docs/valuelst-map.md`,
`docs/re/id-audit.md` (specifically the "63 ids" dead-data list, which
turns out to be wrong about ids 63/64 — see Finding 2), `libs/sim/include/
bomber/sim/tuning.hpp`, `libs/sim/include/bomber/sim/player.hpp`,
`libs/sim/src/grid.hpp`, `libs/sim/src/systems/powerups.cpp`.

---

## Finding 1 — powerup-under-brick scatter: list-removal vs. the original's rejection-sampling scan

> **FIXED 2026-07-20 (batch 2).** `build_state`'s scatter now replicates
> `sub_4258E5`'s non-network branch DRAW-FOR-DRAW: per kind k in 0..12, a
> positive count places every unit / a negative -N runs `|N|` units each behind
> an interleaved 1-in-10 `random_below(s,10)` gate; each attempted unit draws a
> fresh `random_below(s,kGridWidth)` then `random_below(s,kGridHeight)`, retries
> ≤200 times, and is silently dropped on exhaustion (no candidate side-list).
> Verified against the 13-kind loop count, x-then-y order, and the
> "always-place flag, else a 1-in-10 roll" short-circuit. Golden B (setup + 6 checkpoints) and C
> recaptured; D/E zero all `spawn_counts` so their scatter draws nothing either
> way and stayed byte-identical (kExpectedRng / bounces / final rng UNCHANGED,
> verified before recapture); A never runs `build_state`. Pinned by
> `tests/sim/test_sim.cpp` two scatter tests. Visual golden left un-recaptured — it
> is contaminated by concurrent uncommitted renderer WIP (the pre-explosion
> `walking`/`bomb_pulse` frames, which this sim-only change cannot affect, also
> moved), so a clean recapture waits for that renderer work to land per the
> renderer row's own deferred-recapture convention.

**Original** (`sub_4258E5`'s non-network branch, `native/src/game/
batch_0x42583B.cpp` lines 220-255). The structure, in exact order:

- **Outer loop, kind `k` = 0..12.** `count ← getvalue(400 + k)`
  (`sub_412135`) — VALUELST 400-412 is the per-kind count. If this is a
  netgame (`sub_40C06A()`) **and** `k == 12`, `count` is forced to 0
  (netgame-only zeroing, N/A locally).
- **Sign split.** An `always_place` flag starts at 1; if `count` is
  negative, `always_place` ← 0 and `count` ← |count|.
- **Unit loop, `m` = 0..count-1.** The unit is attempted only if
  `always_place` is set **or** a fresh `rand_() % 10` comes up 0 — i.e. the
  negative-N form runs a 1-in-10 gate per unit, and the gate's draw is taken
  only when `always_place` is clear (C short-circuit).
- **Placement scan, `n` = 0..199** (UP TO 200 tries, first hit wins). Each
  try draws, in this order: a random column `rand_() % dword_4648AC`
  (1 draw), then a random row `rand_() % dword_4648B4` (1 draw). The try is
  accepted when the cell reads Brick (`sub_425FB9(x, y) == 2`) **and** has
  no powerup record yet (`sub_42542D(x, y)` falsy); on acceptance the
  152-byte powerup-cell record for kind `k` is written at that (x, y) and
  the scan breaks. If all 200 tries miss, the loop simply ends — the unit
  is dropped.

Each successful placement is **independent rejection sampling**: draw a
uniform-random `(x, y)` pair (2 draws), accept only if the cell is currently
a Brick (`sub_425FB9(...) == 2`) *and* has no powerup record yet
(`!sub_42542D(...)`), retry up to 200 times, and — critically — **silently
give up on that one unit if all 200 tries miss**. There is no side table of
"remaining brick candidates"; every try is a fresh, independent draw against
the whole board.

**Port** (`libs/sim/src/setup.cpp` lines 150-174):

```cpp
std::vector<std::pair<int, int>> bricks;
for (int y = 0; y < kGridHeight; ++y)
    for (int x = 0; x < kGridWidth; ++x)
        if (s.cells[y][x] == Cell::Brick) bricks.emplace_back(x, y);

for (int k = 0; k < kPowerupKinds; ++k) {
    ...
    for (int i = 0; i < count && !bricks.empty(); ++i) {
        std::uint32_t pick = random_below(s, static_cast<std::uint32_t>(bricks.size()));
        auto [bx, by] = bricks[pick];
        bricks.erase(bricks.begin() + pick);
        s.hidden[by][bx] = static_cast<PowerupType>(k);
    }
}
```

The port pre-builds the full list of Brick cells once, then spends exactly
**one** RNG draw per placement to pick a uniformly-random *remaining* list
index and remove it. The negative-N "how many units succeed the 1-in-10
roll" phase (lines 162-167) is faithfully ported — that part is a literal,
draw-for-draw match of the "always-place flag, else a 1-in-10 `rand()%10`
roll" gate — but the *placement*
phase that follows it is a different algorithm end to end:

- **Draw count/shape**: the original spends 2 draws per try, up to 200
  tries per unit (0-400 draws, average far fewer on a dense board but
  non-trivial on a sparse one); the port spends exactly 1 draw per unit,
  always. Since both draw from the same `State::rng` stream that gameplay
  continues to draw from afterward, this changes the RNG state the match
  starts play with relative to a literal port, for every scheme.
- **Failure mode**: the original can under-place — if a unit's 200 tries
  all land on non-Brick or already-occupied cells (a real possibility on a
  low brick-density scheme, or late in the loop once most bricks already
  hold a token), that unit is silently dropped with **no board effect and no
  compensating retry**. The port's `!bricks.empty()` guard means it only
  ever runs out when literally every Brick cell already holds a token — it
  cannot "miss" the way the original's blind scan can, so a sparse/low-`-B`
  scheme with counts that add up to more powerups than there are candidate
  bricks would over-place relative to the original (which would silently
  drop the overflow) — though see the code comment: the port already caps
  each *loop* to `!bricks.empty()`, so it cannot exceed the true brick
  count, it just distributes differently across kinds when bricks run out
  mid-scan.
- **Distribution**: both approaches are uniform over *available* cells at
  the moment of the draw, so on the shipped 90%-density `BASIC.SCH` (165
  cells, the vast majority Brick candidates) the resulting boards are
  statistically indistinguishable to the eye — this is why no golden test
  caught it. The divergence is real but low-amplitude on stock content.

**Visible effect**: none on stock schemes' *look* (uniform scatter either
way); a genuine RNG-stream and possible total-placed-count divergence on any
custom/sparse scheme, and an RNG-draw-count divergence from a literal port
on every scheme (relevant if this project ever tries to bit-match a captured
original RNG trace at setup, which `docs/valuelst-map.md`'s own "brick fill"
entry already notes is impossible against the *real* game because it seeds
off the wall clock — but matters for internal-consistency/golden-hash
purposes going forward).

**Severity**: Medium. **Confidence**: High (direct arithmetic comparison).

**Suggested fix**: if closer fidelity is wanted, replace the list-removal
loop with the literal 200-try `random_below(s, width)` / `random_below(s,
height)` rejection scan against `s.cells`/`s.hidden`, matching the original
draw-for-draw (2 draws/try, ≤200 tries, silent drop on exhaustion). This
would also naturally reproduce the original's under-placement behavior on
sparse schemes. Flag this as a "setup-only, load-bearing for reproducibility
only, not for real-game bit-matching" change like the brick-fill/spawn-
shuffle LCGs' own comments already do — and recapture the golden hashes,
since this changes the RNG state at the tick-0 boundary for every existing
scenario that has any Brick cells (i.e. all of them).

---

## Finding 2 — only ExtraBomb/Flame starting-inventory baselines are applied to a fresh player; the other 11 are parsed but unused at setup

**Status**: this is the same gap `docs/re/audit/powerups.md` §1 already
identified (its own text: "flagged for the setup/scheme audit (ledger item
#10)"). Independently re-derived below directly from the native
transliteration rather than taken on faith, and its one open question —
whether the scheme's `-P` "born with" flag and the VALUELST `start_with`
baseline are the same mechanism or two independent, additive ones — is
resolved at the end of this section.

**Original** (`sub_4214BC`, `native/src/game/batch_0x420D4E.cpp` lines
427-428, inside the per-player loop that runs for all 10 slots every round):

A single loop, `j = 0..14`, writes the byte at `+86 + j` of the player
record from `getvalue(50 + j)` (`sub_412135`). No condition guards it, and
nothing else in the function touches that byte range.

This unconditionally seeds **15** per-kind inventory-count bytes
(`+86..+100`) from VALUELST ids **50 through 64** — every player, every
round, no gating. `docs/valuelst-map.md` documents ids 50-62 as "starting
inventory per powerup kind" (13 kinds; default `1 bomb, 2 flame, rest 0`).
Ids 63/64 are the two extra slots the 15-wide loop also reads — `docs/re/
id-audit.md`'s "Part A(iii) dead data" list wrongly includes both as
"no `getvalue` call site found" (its computed-offset scan pattern evidently
didn't recognize `getvalue(j+50)` inside this loop as covering `j=13,14`);
they are read, just always 0 in the stock file, so the port's not treating
them as live kinds has no visible effect — worth a docs fix but not a code
one, noted here rather than opening a separate finding for it.

**Port** (`libs/sim/src/setup.cpp` lines 112-116):

```cpp
p.speed = s.tuning.start_speed;
p.max_bombs = s.tuning.start_with[static_cast<int>(PowerupType::ExtraBomb)];
p.flame = s.tuning.start_with[static_cast<int>(PowerupType::Flame)];
for (int k = 0; k < kPowerupKinds; ++k)
    if (config.born_with[k]) powerups.apply(p, static_cast<PowerupType>(k));
```

`Tuning::start_with[kPowerupKinds]` (`tuning.hpp` line 58, populated from
ids 50-62 by `case 50...62` in `Tuning::apply`, `tuning.hpp` line ~279) is
fully parsed for all 13 kinds — but at setup, only indices `ExtraBomb`(0)
and `Flame`(1) are ever copied onto a fresh `Player`. The other 11 kinds'
baseline values sit in `Tuning::start_with[2..12]` and are consumed
**only** as the surplus threshold inside `PowerupSystem::head_hit`/
`death_scatter` (`powerups.cpp` — `held_count(p, kind) > s.tuning.start_with[kind]`),
never as an initial grant. On the stock VALUELST (`docs/valuelst-map.md`:
"50–62 | ... | 1 bomb, 2 flame, rest 0") this is invisible — the untouched
baseline for every other kind is 0, matching a freshly-constructed
`Player`'s all-false/all-zero defaults exactly. But it means the port has
**no live code path** that would apply a nonzero baseline for Disease,
Kick, Skate, Punch, Grab, Spooger, Goldflame, Trigger, Jelly, or
SuperDisease if a custom `VALUELST.RES` (or a future in-game tuning screen)
set one — the original would hand every player that ability/count at round
one; the port silently would not.

**Visible effect**: none against any shipped data (all-zero baseline for
the affected 11 kinds). A real functional gap for any modified VALUELST
that sets ids 52-62 nonzero — currently untestable with existing fixtures
since none do.

**Severity**: Low (stock data) / Medium (customized data — a complete
silent no-op of a documented, `Tuning::apply`-wired VALUELST feature).
**Confidence**: High.

**Suggested fix**: replace the two hardcoded lines with a loop mirroring
`sub_4214BC`'s own unconditional 15-slot write —
`for (k=0;k<kPowerupKinds;++k) powerups.reset_to_baseline(p, k, s.tuning.start_with[k])`
(or equivalent direct field writes for the two counted kinds plus a
bool-flag set for the flag kinds) — run *before* the existing `born_with`
loop, exactly as the original's uniform baseline write precedes any
scheme-specific `-P born_with` overlay in the call order. Add a doctest
with a `Tuning` that sets a nonzero `start_with[Kick]` and asserts a fresh
player has `kick == true`. Also worth a one-line `docs/re/id-audit.md` fix:
move ids 63/64 out of the "63 ids, dead data" list (they ARE read, by the
15-wide loop above; they're just always-0 in the shipped file).

**~~Resolving `audit/powerups.md` §1's open question — `born_with` (scheme)
and `start_with` (VALUELST) ARE two independent, additive mechanisms.~~
WRONG — RETRACTED 2026-07-28.** They are ONE mechanism. See
`docs/re/facts.md` "The `.SCH` `-P` row's 2nd field is a COUNT that REPLACES
the starting inventory" for the evidence; the short version is that the
scheme reader `sub_403EEE`, after it closes the file, calls the VALUELST
**setter** `sub_4121BF(50 + kind, count)` for every `-P` row whose count is
`> 0`. That is the same value table `sub_412135` (`getvalue`) reads — same
base pointer, same index arithmetic — so the scheme literally overwrites the
id `sub_4214BC` then reads. "Born with" IS the starting-inventory baseline.

**Why this pass got it wrong, since the same trap is easy to fall into
again.** The reasoning above is an argument from ABSENCE: `sub_4214BC`
contains no scheme-table access, therefore the scheme must reach the player
by some other path, therefore the two are independent and additive. The
first step is true and the rest does not follow — the scheme reaches
`sub_4214BC` through the value table, which is exactly why `sub_4214BC` has
no scheme access to find. The pass never opened the scheme reader to check.
The consequences of the wrong conclusion, both fixed in the same commit as
this retraction:

- the port collapsed the parsed COUNT to a bool and applied it once,
  additively, so a scheme asking for 3 bombs granted 2;
- it routed the grant through `PowerupSystem::apply`, which runs the pickup
  dispatcher's mutual-exclusion evictions. Those SCATTER the evicted token
  and draw `State::rng`. `sub_4214BC`'s baseline write is a raw byte store
  with neither, so a scheme granting both Grab and Spooger kept both in the
  original while the port evicted one at setup and burned an RNG draw;
- it left the scheme's counts invisible to every OTHER reader of id 50+kind
  — notably the death-scatter and head-hit "above baseline" surplus tests
  — so a scheme-granted powerup was dropped on death when the original keeps
  it.

`MatchConfig::born_with` is gone; `match_factory` writes
`cfg.tuning.start_with[id] = count` instead. The one part of this finding
that stands unchanged is its actual subject: the baseline itself was
incompletely wired for 11 of 13 kinds, and that fix (the 13-kind block in
`setup.cpp`) was correct and remains.

---

## Finding 3 — spawn-coordinate range handling: `std::clamp` vs. the original's asymmetric wrap/clamp

**Original** (`sub_4214BC`, `native/src/game/batch_0x420D4E.cpp` lines
392-401, reading the (possibly Random-Start-shuffled) spawn arrays), in
order, for player slot index *i*:

| step | operation |
|---|---|
| 1 | X ← the spawn-X array `dword_46460C[i]` |
| 2 | Y ← the spawn-Y array `dword_46465C[i]` |
| 3 | while X < 0: X += `dword_4648AC` (grid width) — negative X: **WRAP** |
| 4 | while Y < 0: Y += `dword_4648B4` (grid height) — negative Y: **WRAP** |
| 5 | if X >= `dword_4648AC`: X = `dword_4648AC` − 1 — overflow X: **CLAMP** to max |
| 6 | if Y >= `dword_4648B4`: Y = `dword_4648B4` − 1 — overflow Y: **CLAMP** to max |

Negative spawn coordinates **wrap** (repeatedly add the grid dimension
until non-negative — e.g. `-1` on a 15-wide grid becomes `14`, the far
edge), while coordinates at or past the grid dimension **saturate** to the
last valid index. These are two different operations, not one symmetric
clamp.

**Port**: both `libs/match/include/bomber/match/match_factory.hpp` (line
73-74, at `.SCH` spawn-row parse time) and `libs/sim/src/setup.cpp` (lines
57-58, redundantly, at round-init time) use:

```cpp
int tx = std::clamp(config.spawns[i].x, 0, kGridWidth - 1);
int ty = std::clamp(config.spawns[i].y, 0, kGridHeight - 1);
```

`std::clamp` saturates on **both** ends — a negative input pins to `0`
rather than wrapping to `width - 1`/`height - 1`. Positive-overflow
handling already matches (saturate to `dim - 1`).

**Visible effect**: none on any shipped `.SCH` — spawn `-S` rows in the
built-in schemes are always valid in-range grid coordinates, so this path
is currently unreachable in practice. It would only manifest with a
hand-edited or malformed scheme carrying a negative `-S` coordinate, in
which case the port would place that spawn at the near edge (tile 0)
instead of the original's far-edge wrap.

**Severity**: Low. **Confidence**: High (literal reading), Low
(reachability — no known content trips it).

**Suggested fix**: if pursued, replace both clamp call sites with the
original's wrap-then-saturate sequence (`while (v<0) v+=dim; if (v>=dim)
v=dim-1;`) for exact parity. Given zero known reachable content, this is a
reasonable one to defer or explicitly accept as "our tunable" with a citing
comment rather than fix immediately.

---

## Verified faithful (no change)

- **Per-match brick fill** (`sub_4260F5`'s non-editor branch, row-major
  `rand()%100 >= density` roll only on `':'` candidates) — already
  confirmed and cited in `docs/re/facts.md` "Per-match brick fill" and
  `match_factory.hpp`'s own extensive comment; re-derived here from
  `batch_0x42583B.cpp` lines 483-499 and matches exactly, including that
  `#`/`.` cells consume no RNG draw.
- **Random Start spawn shuffle is a once-per-MATCH operation, not
  once-per-ROUND** — newly confirmed by tracing the actual call sites
  (`batch_0x410401.cpp`): `sub_4214BC` (player placement) is called from
  the per-round init sequence at line 613 (`sub_4260F5 → sub_42633C →
  sub_4214BC → sub_4258E5 → sub_40551F → sub_40151B`, matching
  `setup.cpp`'s own citation exactly), while `sub_421793` (the 200-swap
  shuffle) is called from a *different*, earlier function at line 925 —
  the one-time match-start negotiation/AI-init block, alongside
  `sub_40A140`/`sub_4148E5`, never re-entered per round. The port's
  architecture (shuffle once in `build_match_config`, reuse the same
  `spawns[]` for every round `build_state` constructs) is therefore
  correct, not a simplification — a question this pass set out to close
  after noticing `setup.cpp`'s citation only says "at round init" without
  distinguishing match-start from every-round.
- **Round-start input freeze** (`dword_4621E0 = dword_46494C *
  getvalue(30)`, set once after the per-player loop, not per player) —
  `docs/re/facts.md` "Round-start input freeze", re-derived from
  `batch_0x420D4E.cpp` line 454, matches `State::input_freeze` exactly.
  The companion `dword_4621E8 = dword_46494C * getvalue(32)` set right
  after it (line 455-456) is confirmed **presentation-only** — traced to
  `batch_0x41F29B.cpp` line 623, where it only selects which draw-colour
  index to pass to the sprite-queue call (`sub_415A9F`), with zero gameplay
  read anywhere else. Correctly unported **from the sim** — but it is not
  unimplemented: it is the round-start own-colour reveal, ported on the
  PRESENTATION side 2026-07-22 (88b5168) in `Renderer`, which reads
  getvalue(32) straight from the `ValueList`. See `docs/re/facts.md`
  "Round-start own-colour reveal" and the id 32 row in
  `docs/valuelst-map.md`. (This line previously read "Correctly unported"
  with no qualifier, which read as "not implemented"; corrected 2026-07-27.)
- **VALUELST ids 41 (bomb fuse length) and 42 (starting walk speed)** —
  `sub_4214BC` copies both onto per-player struct fields — the fuse length
  from `getvalue(41)` into a 16-bit field at element 37 of the player
  record, and the walk speed from `getvalue(42)` into element 28 — but
  since both
  values are identical across all players and never diverge afterward, a
  single global `Tuning::fuse_frames`/`Tuning::start_speed` (`tuning.hpp`
  lines 26-27, 226-227) read at point of use is behaviourally equivalent
  to a faithful per-player copy. Not a gap.
- **Player Y position is stored as tile-CENTRE, not tile-bottom** — this
  pass initially flagged `grid::tile_center_y` (`ty*kTileHF + kTileHF/2`,
  symmetric) against `sub_42655F`'s literal formula (`36*ty + 36 - 1 +
  originY`, i.e. tile-**bottom**, an asymmetric 35px-of-36 offset) as a
  possible bug, since `sub_4214BC` writes the tile-bottom-anchored value
  into the SPAWN-anchor fields (index 5/6) and `batch_0x41F29B.cpp` lines
  205-206/574-575 confirm those get copied verbatim into the LIVE movement
  position (index 7/8) the instant a player activates. This is already
  resolved and correctly ported: `docs/re/facts.md` "Screen geometry /
  field placement" documents that the port deliberately stores the
  CENTRE internally and adds `kTileH/2` only at the *renderer's* blit
  call, matching `sub_42655F`'s tile-bottom draw anchor visually without
  polluting the hashed simulation position with an asymmetric constant.
  `setup.cpp`'s use of the shared, symmetric `grid::tile_center_x/y` is
  consistent with this established, cited convention — not a setup-local
  deviation. No change.
- **Dud-gate arm** (`sub_422C13`: `base(id320) + rand()%max(1,rand(id321))`,
  seconds, converted ×`kTicksPerSecond`) — `docs/re/facts.md` "Dud bombs";
  `setup.cpp` lines 176-185 match `batch_0x421E80.cpp` lines 707-714
  arithmetic-for-arithmetic, draw-for-draw.
- **Born-with / Goldman-wheel overlay / clogs speed penalty** — the
  `config.born_with[k]` → `config.born_with_extra[i][k]` → `p.clogs`/
  `p.speed -=` ordering in `setup.cpp` lines 116-133 already carries
  citations to `docs/re/goldman-roulette.md` §4/§8/§9 confirming the
  overlay-after-baseline ordering and the "one more born-with unit, not a
  distinct grant mechanism" equivalence; re-confirmed here against the
  same sections, not re-litigated.
- **Powerup-scatter's per-kind negative-N "how many succeed" phase** (the
  always-place flag short-circuiting a 1-in-10 `rand()%10` roll, `|N|`
  iterations) — the port's `count`
  accumulation loop (`setup.cpp` lines 162-167) is a literal, draw-for-draw
  match of this specific sub-step; only the subsequent *placement*
  mechanism diverges (Finding 1).
- **Campaign rover/ghost seeding order and RNG-neutrality when count==0**
  — `setup.cpp` lines 136-148's citation to `docs/re/campaign.md`
  (ghosts-then-rovers, both after player placement, before the
  hidden-powerup loop) was spot-checked against `sub_40151B`'s own
  existence as a distinct, stage-start-only caller (not the per-round
  `sub_410B6E` sequence) and found consistent; not re-derived line-by-line
  here (out of the two functions this pass focused on) but no
  contradiction found.
- **`pick_stage`** (`match_factory.hpp` lines 224-230, VALUELST 1150-1160
  enabled-level filter, `seed % allowed.size()`) has no original-side
  round-init counterpart to compare against — stage *selection* is a menu
  UI concern (`sub_412135(dword_46499C+1150)` gating loop seen in
  `batch_0x410401.cpp` lines 568-576, itself outside the per-round
  sequence and outside this audit's two target files) — noted for
  completeness, not evaluated as a setup.cpp finding.
