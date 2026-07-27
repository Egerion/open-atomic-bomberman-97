# Fidelity audit: tile regeneration + rovers/ghosts

Read-only differential pass, part of the systematic sweep tracked in
`docs/re/fidelity-audit.md` (ledger rows 11-12). No code changed.

**Verdict — tile regeneration (`libs/sim/src/systems/tile_regen.cpp`):
FAITHFUL, no findings.** Every check (candidate order, draw count/order,
clear-radius formula, timer reset-before-attempt semantics, tick placement
relative to `EnclosureSystem`) matches `sub_426704`/`sub_422351`
(pseudo.c 27093-27132) line-for-line; the one deliberate deviation
(`present && alive` player gating) is already documented and justified in
`docs/re/facts.md` "Per-level tile regeneration".

**Verdict — rovers/ghosts (`libs/sim/src/systems/rovers.cpp`): ONE real
finding (medium severity, high confidence, confirmed via raw disassembly),
two inert/cosmetic literal deviations, everything else faithful.**

Total findings: **3** (1 medium, 2 informational/cosmetic).
Highest severity: **Medium — Finding 1, flame-death does not stop the
tick's movement or arm the same-tile landing-kill.**

---

## Finding 1 — flame-death doesn't stop the mover's per-pixel loop or gate the landing-tile kill

**System:** rovers

**Original** (`sub_401B5C`, pseudo.c 4839-4977; confirmed against raw
disassembly `native/tools/disasm.py 0x401B5C 0x401F76`): inside the
pixel-budget loop — which runs while the actor's move-budget dword at
`+116` is greater than 0 — the flame probe
(`sub_42708D`, disasm 401E24) sets the actor's dead flag at `+8` to 1
(401E35) and — only if kind matches — awards `sub_421C71` score
(401E3F-401E7D).
**Nothing branches out of the loop on this path**: execution falls straight
through into the landing-tile-kill retry loop (401E82-401EBF, `sub_421CB5`
+ `sub_41DE63`, capped at 10 tries on the **same** tile the flame just
killed the mover on), then commits the new position (401EC1-401ED0), then
at 401ED3 jumps unconditionally back to the loop's own top at 401C0F and
keeps consuming the rest of *this* tick's `move_budget` — potentially
crossing further flame tiles (re-arming the score award again each time,
since the dead-flag check is not re-tested inside the loop) and further
landing-tile kills. The dead flag is only consulted at the very **top** of
the function (401BAE tests the `+8` dead flag on entry and, when it is set,
zeroes the actor's active dword at `+0` and returns immediately), i.e. on
the **next frame's** call from `sub_401F76` — the actor
is drawn/moves through the remainder of the current frame's budget and is
only reaped (skipped, deactivated) starting the following frame.

**Port** (`libs/sim/src/systems/rovers.cpp:180-187`):

```cpp
if (grid::in_grid(ntx, nty) && s.flame[nty][ntx] > 0) {
    const std::uint8_t owner = s.flame_owner[nty][ntx];
    s.events.push_back({Event::Type::RoverDied, ...});
    r.alive = false;
    return false;
}
// Landing-tile kill (sub_421CB5 + sub_41DE63): ...
for (int i = 0; i < kMaxPlayers; ++i) { ... }
```

`step()` returns immediately on the first flame contact.

**Visible effect:**
1. The landing-tile-kill check for the tile the mover died on is skipped
   entirely — a human/network player standing on the exact tile a rover/
   ghost dies to flame on is *not* killed by the rover's own landing-kill
   (they may still die from the flame itself via the normal player-flame
   path, but that is a separate, unrelated system; the original kills them
   via *both* paths on that tile).
2. The mover vanishes instantly instead of continuing to move (and
   re-trigger flame-death/score-award, and further landing-kills at
   subsequent tiles) for the rest of that tick's `move_budget`. In the
   (rare, campaign-only) case of a rover/ghost crossing more than one
   flame tile in the same tick's budget, the original awards the flame
   owner's kill-score (VALUELST 1310/1320) once per flame tile crossed;
   the port awards it exactly once, always.
3. Net effect: the port slightly *under*-counts rover/ghost kill-score in
   multi-flame-tile crossings and *misses* a same-tile player kill that
   coincides with the mover's own flame death — both narrow, low-frequency
   edge cases (campaign-only, needs a mover and a human player and an
   active flame to overlap the same tick), consistent with the task's
   framing that this system is low player-facing frequency.

**Severity:** Medium (confirmed, reproducible control-flow divergence with
a real scoring/kill effect, but campaign-only and narrow to trigger).

**Confidence:** High — verified against raw x86 disassembly, not just
the decompiler's pseudocode (see the instruction-level trace above: there
is no branch out of the budget loop anywhere on the flame-death path, and
the tail at 401ED3 jumps unconditionally back to the loop top at 401C0F).

**Suggested fix:** give `Rover` a "pending death" flag instead of removing
it immediately. On flame contact: record the flame-death event/score
exactly once (matching the original's per-flame-tile-crossed re-arm would
require *not* suppressing repeat awards — or, more simply and closer to
observed practical impact, keep the original's "mark dead, keep moving"
shape by not `return`-ing early), fall through to the landing-tile-kill
check for the current tile, keep consuming `move_budget` for the rest of
the tick exactly as before, and only stop scheduling movement for the
actor starting the *next* tick (deactivate before that tick's `step()`
rather than mid-tick). A test forcing a rover through two adjacent flame
tiles in one tick's budget (to pin the repeat-score-award behaviour) and a
test putting a live human player on the exact tile a rover dies to flame
on (to pin the same-tile landing-kill) would close the gap; no existing
`tests/test_rovers.cpp` case currently exercises either (checked: its two
flame-death cases place the flame one tile away with nothing else on it).

---

## Finding 2 — spawn placement rejects a bomb-occupied tile; the original doesn't (inert given the current call site)

**System:** rovers

**Original** (`sub_4019C2`, pseudo.c 4762-4789, cross-checked against
`native/src/game/batch_0x401010.cpp` and the raw disasm): the spawn
candidate loop tests only the candidate tile's state query
`sub_425FB9(col,row) != 1` (not solid) and `sub_422351(col,row,3)` (clear of
players). There is **no** `sub_422E48`
(grounded-bomb) check anywhere in this function.

**Port** (`libs/sim/src/systems/rovers.cpp:50-51`):

```cpp
if (s.cells[ty][tx] == Cell::Solid) continue;
if (grid::bomb_at(s, tx, ty) != nullptr) continue;   // <- not in the original
```

**Visible effect:** none observable today. `RoverSystem::spawn` is only
ever called from `setup.cpp`'s `build_state`, before any bomb has been
placed (`s.bombs` is empty at that point in every existing scenario), so
this extra condition never actually rejects a candidate. It would only
matter if `RoverSystem::spawn` were ever called later — e.g. a future
mid-round rover respawn feature — against a board that already has bombs
on it, where it would reject tiles the original would accept.

**Severity:** Informational / cosmetic (currently dead code, no behavioural
difference in any reachable scenario).

**Confidence:** High (both the native transliteration and the raw pseudo.c
line-range agree there is no bomb check in `sub_4019C2`; the port's own
call site is verified to run before bombs exist).

**Suggested fix:** none required while `spawn` is setup-only. If a future
change calls `spawn` mid-round, drop the extra `bomb_at` check for literal
fidelity, or explicitly document why the port intentionally strengthens it
(same treatment already given the `present && alive` clear-radius
deviation in `docs/re/campaign.md`).

---

## Finding 3 — mover's rover-kind passability adds a `burning == 0` term; provably a no-op given this codebase's own cell/burning invariant

**System:** rovers

**Original** (`sub_4017FA`, pseudo.c ~4729 area / batch_0x401010.cpp
lines 432-439): for the non-ghost (rover) case the function returns true
exactly when the tile-state query `sub_425FB9(col, row)` yields 0 — a pure
cell-type check, no flame/burning read anywhere.
`docs/re/facts.md`'s "Flame is NEVER checked" note independently confirms
`sub_425FB9`'s backing array (`dword_46222C`) is entirely separate from
the flame array `sub_42708D` reads.

**Port** (`libs/sim/src/systems/rovers.cpp:35`):

```cpp
return s.cells[ty][tx] == Cell::Blank && s.burning[ty][tx] == 0;
```

**Visible effect:** none. In this codebase's own model, `s.cells[y][x]`
only transitions Brick→Blank inside `FlameSystem::age_flames_and_bricks`
(`libs/sim/src/systems/flames.cpp:295-308`) in the same statement that
`s.burning[y][x]` reaches 0, and `s.burning` is only ever set alongside a
tile that is (and, until that same transition, remains) `Cell::Brick`
(`flames.cpp:126`, `spread_to`). So `s.cells[ty][tx] == Cell::Blank`
already implies `s.burning[ty][tx] == 0` at every point any system can
observe the grid — the appended `&& s.burning[...] == 0` term can never
independently flip the result. (Contrast `grid::tile_open`, used for
*player* movement, where the same-looking `burning > 0` guard is load-
bearing because it also blocks a tile whose crumble the player-facing
helper wants to keep closed even before `age_flames_and_bricks` flips
`s.cells` — not the situation here.)

**Severity:** Informational / cosmetic (provably redundant given an
invariant already enforced elsewhere in `libs/sim`, not a behavioural
bug).

**Confidence:** High (the invariant is a direct reading of
`flames.cpp`'s own write sites, not an inference).

**Suggested fix:** none required; optionally drop the redundant term for
literal fidelity, or add a one-line comment citing the invariant so a
future reader doesn't mistake it for load-bearing (the ghost branch,
`s.cells[ty][tx] != Cell::Solid`, correctly has no such term, which is the
asymmetry that makes the rover branch's addition look intentional rather
than copy-paste).

---

## Verified faithful (no change) — tile regeneration

- Gate: inert (zero cost, zero RNG) on every level but Haunted House
  (`regen_seconds[level] <= 0`), matching `sub_426818`'s
  `sub_412135(dword_46499C+340)` short-circuit.
- Interval: `regen_seconds * kTicksPerSecond` countdown, reset
  *unconditionally* (win or lose) at the top of the attempt cycle — matches
  the original re-arming its interval global `dword_464978` from the
  freshly-read interval value *before* the 100-attempt loop, not after.
- Attempt loop: up to 100 candidates, exactly 2 RNG draws per attempt
  (x then y) regardless of outcome, stop at first eligible tile — matches
  `sub_426704`'s `for (i<100) { rand%W; rand%H; if (eligible) { write;
  return; } }` shape.
- Eligibility conditions (blank / no powerup / no bomb / clear-radius) are
  individually correct; the port's check *order* (blank, bomb, powerup,
  clear-radius) differs from the original's textual order (blank, powerup,
  bomb, clear-radius), but since none of the four tests have side effects
  or consume RNG, the tests commute — reordering them is provably
  behaviourally inert.
- Clear-radius formula: Manhattan distance, matches id 695's own VALUELST
  comment and `sub_422351`'s abs+abs shape; `present && alive` player
  gating is a pre-existing, already-documented deviation (facts.md), not
  re-litigated here.
- Brick write: cell-type only, no powerup/hidden interaction, no sound/anim
  trigger — matches `sub_425F79`→`sub_425E9B`→`sub_425D22`'s plain tile
  write and the confirmed absence of any `sub_427961`/`sub_4278F2` call in
  `sub_426704`.
- Tick placement: `TileRegenSystem::update()` runs immediately before
  `EnclosureSystem::update()` (`simulation.cpp:708-709`), mirroring
  `sub_426704` being nested inside `sub_426818` before its wall stepper.
- Cadence model (real-time interval collapsed to a per-tick countdown,
  first attempt on tick 1 rather than emulating cross-round wall-clock
  leakage) is an already-documented, justified simplification
  (`docs/re/facts.md` "Cadence: process-lifetime clock..."), not a new
  finding.

## Verified faithful (no change) — rovers/ghosts

- Spawn order: ghosts spawn before rovers
  (`RoverSystem::spawn(Ghost, ...)` then `spawn(Rover, ...)` in
  `setup.cpp:147-148`), matching `sub_40151B`'s own call order
  (`sub_401B05` [ghost] before `sub_401AAE` [rover]).
- Spawn candidate draws: 2 RNG draws per attempt (x then y), up to 200
  attempts, accept on not-solid (bricks ARE a legal spawn tile) AND
  Manhattan distance > 3 from every present+alive player — matches
  `sub_4019C2`/`sub_422351(...,3)` (confirmed via raw disasm per
  `docs/re/campaign.md` "Spawning").
- Per-tick mover budget accrual: `speed * subFrameMs/50 + 100`, folded
  across all `kSubFrames` sub-frames into one call — matches
  `sub_401B5C`'s `+116 += +112*dword_464958/dword_46494C + 100` running
  once per displayed frame; the fold is valid because nothing in the
  budget-accrual/per-pixel-turn arithmetic depends on anything that
  changes between sub-frames within a single tick.
- Turn-decision logic (candidate-pixel-reaches-centre gate, ahead-tile
  probe using the CANDIDATE tile, 1-in-N "turn anyway" roll when open,
  unconditional turn when blocked, re-probe after turning): matches
  `sub_401B5C`'s along/perp centring math and roll shape one-for-one.
- Re-probe after turning uses the **freshly turned** direction, not a
  stale pre-turn one — **independently confirmed via raw disassembly**: at
  401DAD the actor's direction dword at `+0x2a` is re-loaded fresh from
  memory for the second ahead-check, i.e. *after* the turn has already been
  written back at 401D9E-401DA5. Note: the native transliteration file
  (`native/src/game/batch_0x401010.cpp`)'s C++ reuses a single
  `int dir42 = ...` local for both the first and second ahead-checks,
  which reads as if the second check used the stale pre-turn value — that
  reading is an artifact of the transliteration, not what the real
  assembly does; the port's `r.dir`-based re-test (freshly re-read after
  the turn, `rovers.cpp:159`) is the one that actually matches the
  binary. Flagged here for whoever maintains `native/` since it's a latent
  inaccuracy in that file, but it is outside this port-fidelity audit's
  scope to fix.
- Committed pixel position uses the PRE-turn direction's candidate
  (a turn taken this pixel steers only the *next* step) — matches.
- Ghost-vs-rover type-dependent walkability (ghosts phase through bricks,
  rovers don't; both always blocked by a grounded bomb) — matches
  `sub_4017FA`.
- Landing-tile kill: iterates to find every present+alive, non-AI,
  non-bounce/warp player on the actor's new tile and kills each via the
  same funnel as flame-death/enclosure-crush (owner -1, no kill-tally
  credit, powerup death-scatter) — functionally equivalent coverage to the
  original's capped-at-10, same-tile `sub_421CB5`/`sub_41DE63` retry loop
  (which, per its own AI-immunity/no-removal-on-skip shape, can only ever
  discover the *same* remaining live occupants the port's full-array scan
  also discovers).
- Round-pacing grace timer: accumulates ticks while `s.rovers` is empty
  (held at 0 while any rover/ghost lives), fires at the fixed
  `kHazardClearTicks = 40` (the confirmed fixed-2000ms), continues
  accumulating even once already cleared — matches `sub_4016DA` clause 3
  (`docs/re/campaign.md` "Round pacing — PINNED").
- `RoverSystem::tick()` is a true no-op (zero RNG, zero cost) for every
  non-campaign scenario, gated on `campaign_hazards_active` — matches the
  original's `dword_46489C` campaign-only gating.
- VALUELST 1205 ("human-avoidance bias") correctly left unconsumed —
  confirmed dead in the original itself, not a port gap.

## Sources

- `native/src/game/batch_0x401010.cpp` (full batch, spawn + mover +
  round-pacing).
- `D:\Program Files (x86)\INTRPLAY\BOMBRMAN\pseudo.c` lines 4740-4977
  (`sub_4019C2`/`sub_401AAE`/`sub_401B05`/`sub_401B5C`) and 27093-27132
  (`sub_426704`).
- `python native/tools/disasm.py 0x401B5C 0x401F76` (full linear
  disassembly of the mover, used to confirm Finding 1's control flow and
  the dir42-reload note under "Verified faithful").
- `docs/re/facts.md` "Per-level tile regeneration" (3714-3880),
  "Enclosure/HURRY arithmetic audit" (2585-2684), "Canonical frame
  cadence" / ADR-0006.
- `docs/re/campaign.md` "Spawning", "Per-tick mover", "Round pacing —
  PINNED", "Port status".
- `libs/sim/src/systems/tile_regen.cpp`, `libs/sim/src/systems/rovers.cpp`,
  `libs/sim/src/setup.cpp:136-148`, `libs/sim/src/simulation.cpp:647-656,
  708-709`, `libs/sim/src/systems/flames.cpp:90-134,295-308`,
  `libs/sim/src/grid.hpp:43-48`, `tests/test_rovers.cpp`,
  `tests/test_regen.cpp`.
