# ADR-0005: Deterministic computer-player AI (`AISystem`)

**Status:** Accepted — IMPLEMENTED & COMPLETE (Stages 2-5 landed 2026-07-05; all
8 behaviours + the RNG contract are live in `libs/sim/src/systems/ai.{hpp,cpp}`)
**Date:** 2026-07-05
**Deciders:** Ege
**RE basis:** `docs/re/ai.md` (BM95.EXE `sub_40A1C6` and the `off_45BA78[8]`
behavior table), extends ADR-0003 (determinism).

## Context

The computer opponent is the last large gameplay system still missing from the
port. The RE (`docs/re/ai.md`) established one decisive fact: in the original,
the AI is **not** a separate mover. Each tick, for every computer-controlled
player, the engine calls the brain (`sub_40A1C6`) in the *exact slot* where it
would otherwise read the human's DirectInput (`sub_41F29B` line 23028-23036,
gated on the player-type byte `+16 == 1`). The brain's only outputs are the
player's **input-flag bytes** — direction, bomb-key edge, action-key edge — the
identical bytes a keyboard sets. The same movement/bomb code then consumes them.

That maps one-to-one onto our architecture: we already funnel every player
through `TickInputs` → `player_turn` (simulation.cpp), and `PlayerInput` already
carries exactly `{up,down,left,right,action1,action2}` (types.hpp) — the same six
signals the AI writes. So the AI belongs in `libs/sim` as a system that
**computes each AI player's `PlayerInput` before movement**, and everything
downstream (the mover, bombs, the hash, golden) stays untouched.

The hard constraint is the determinism contract (ADR-0003 / CLAUDE.md): integer
math only, all randomness through `State::rng` with the RE'd **order and count**
of draws preserved, tick step order fixed, and every gameplay field hashed.
`docs/re/ai.md` §8 enumerates the exact per-tick draw sequence we must mirror.

## Decision

Add a new **`AISystem`** to `libs/sim` (`src/systems/ai.{hpp,cpp}`, private
header like the other systems) plus a hashed **per-player `Brain`** on `State`.
Each tick, *before* movement, `AISystem` fills `TickInputs::players[i]` for every
AI-controlled player from `State` + that player's `Brain`, using `State::rng`
for every decision draw. The rest of the tick is unchanged: `player_turn`
consumes the produced `PlayerInput` exactly as it consumes a human's.

### 1. AI is an input provider, not a mover

- `AISystem::decide(int i, PlayerInput& out)` writes `out.{up,down,left,right,
  action1,action2}` for AI player `i` and mutates `s.players[i].brain`.
- It never moves the player, never touches bombs directly, never emits events.
  Movement, bomb drop, punch/grab/trigger all happen later in `player_turn` from
  the flags we set — so the AI is guaranteed consistent with the shipped
  movement code (the original's whole point).
- Mapping brain output → `PlayerInput` (from `docs/re/ai.md` §7):
  - godir `+46` (0..3 / -1) → set the one matching `up/right/down/left` (or none).
  - bomb-key edge `+56=1;+54=0` → `action1 = true` (the drop block is edge-gated
    in `player_turn` on `action1 && !prev_action1`, matching `+56 && !+54`).
  - action-key edge `+57=1;+55=0` → `action2 = true` (punch/trigger; same edge
    rule via `prev_action2`).
  Because the AI forces a fresh edge (`+54=0`/`+55=0`) whenever it wants an
  action, and `player_turn` already tracks `prev_action*`, a "hold" simply means
  the AI leaves `action1/2 = false` that tick.

### 2. Who is an AI player

Add `Player::ai` (bool, hashed). It corresponds to the original's `+16 == 1`
tag. `MatchConfig` gains a per-slot "controller = human|ai" and `build_state`
sets `players[i].ai` accordingly. `AISystem::decide` runs only for
`present && alive && ai` players; for everyone else `TickInputs` carries the
externally-supplied (human/replay/scripted) input untouched.

- Golden scenarios and every existing test set no AI players (`ai == false`
  everywhere by default), so the flag is `false` and the AI code path is never
  entered there — **golden stays byte-identical** (see §7).

### 3. Brain state on `State` (all hashed)

A `Brain` struct per player, stored as `Player::brain` (or a parallel
`std::array<Brain, kMaxPlayers>` on `State` — TBD in Stage 2; either way hashed).
Integer fields only, mirroring `docs/re/ai.md` §1.1 (we keep tile coords as
plain ints, not 16.16 — our sim already stores positions differently and the AI
only ever uses the *tile* `>>16`, so no fixed-point is needed):

| Field | Type | Mirrors | Purpose |
|-------|------|---------|---------|
| `personality` | `std::uint8_t` | +0 | always 0 while VALUELST 900==1; kept for exactness |
| `path_target_x/y` | `std::int16_t` | +4/+6 | current directed-path goal tile |
| `path_target_cost` | `std::int32_t` | +8 | danger score of the goal at capture (stale-check) |
| `has_path_target` | `bool` | +2 truthy | a directed goal is active |
| `pow_seek` | `{ bool active; std::int32_t timer; std::int16_t tx,ty; std::int8_t step_dir; }` | +24/+28/+32/+36 | the ranged-powerup pursuit |
| `enemy_seek` | `{ bool active; std::int32_t timer; std::int8_t target_slot; std::int8_t step_dir; }` | +10/+12/+16/+20 | the enemy pursuit (store the target **slot index**, not a pointer) |
| `state_flag` | `std::uint8_t` | +52 | 9 = "committed to a brick-blast drop" |
| `wander_dir` | `std::int8_t` | +64 | persistent wander godir |

Design deltas from the binary, all determinism-neutral:
- **Pointers → indices.** The original stores raw actor/cell pointers (+16/+32).
  We store the **player slot** or **tile** instead (hashable, snapshot-safe).
  The behaviors only ever read the target's tile and liveness, both recoverable
  from the slot/tile each tick.
- **Timers in ticks.** The original accumulates milliseconds (`+= dword_464958`)
  and times out at `10*msPerFrame`. At the fixed 20 Hz cadence that is exactly
  **10 ticks**; we store an integer tick countdown (same as the disease/warp
  timers already do). No wall clock enters the sim (ADR-0003).
- `state_hash()` gains every `Brain` field (hash.cpp). A `Brain` on a
  non-AI/absent player stays zero-initialised, so hashing it is a no-op for
  non-AI scenarios (golden unaffected — §7).

### 4. RNG through `State::rng`, preserving the RE'd order/count

Every AI draw goes through `bomber/sim/rng.hpp` on `State::rng`
(`random_below(s,n)` = `rand()%n`). The per-tick draw sequence must match
`docs/re/ai.md` §8 **exactly** — order and count are the contract (ADR-0003
rule 2). Concretely, per AI player per tick, in slot order:

1. **Draw A** — the leading scratch-alloc `rand()` (`sub_40A1C6` line 10359).
2. The **one** behavior that fires (chain short-circuits) contributes its draws
   in the fixed intra-behavior order, *including* the BFS tie-break draw
   (`2*(rand()%2)-1`) drawn once at the top of each pathfinder call.
3. **Draw B** — the trailing scratch-alloc `rand()` (line 10412).

Decision: **keep draws A and B.** They are almost certainly heap-debug residue
(their allocated bytes are unused), but they advance the PRNG, and reproducing
them costs one line each and keeps our per-tick draw COUNT equal to the
original's. (Corrected 2026-07-28: this used to say "guarantees exact stream
parity if we ever cross-check against the original". It cannot — the port is
xorshift32, the original is the wall-clock-seeded CRT generator, so no
cross-check of *values* is possible in either direction. A count cross-check is,
and that is what these draws preserve. See `docs/re/ai.md` §8.) `random_below` with the modulus discarded, or a bare
`next_random(s)`, models a bare `rand()`. (If a future decision drops them, do it
in one commit and re-baseline any AI-bearing golden — there are none today.)

The BFS helpers each draw their tie-break **once, before expansion**
(`docs/re/ai.md` §5.1/5.2), independent of path length — so the draw count is
stable regardless of board size. The pathfinders are otherwise pure integer BFS
over a 100-node frontier; no other randomness.

**Cross-player order:** iterate AI players in ascending slot index (0..9),
matching the original outer loop (`sub_420F07`), so the global per-tick AI draw
stream is deterministic.

### 5. Where in the tick order the AI runs (fixed — part of the contract)

`AISystem` runs at the **very top of the player loop, per player, immediately
before that player's `player_turn`** — the exact position of `sub_40A1C6` in
`sub_41F29B` (the AI resolves input, then the same function moves the player).
The cleanest faithful placement in `run_tick` (simulation.cpp):

```
for i in 0..kMaxPlayers:
    if players[i].present && players[i].alive:
        PlayerInput in = players[i].ai ? ai.decide(i)      // NEW: AI fills its own input
                                       : inputs.players[i]; //      humans/replay pass through
        player_turn(s, i, in, bombs, stage);               // unchanged consumer
# ...steps 2..7 unchanged (bombs, fuses, flames, field-vs-players, diseases, clock, compact)
```

This keeps the AI draws interleaved with movement in **slot order**, exactly as
the original interleaves `sub_40A1C6` and the mover per player. It is a
refinement of step 1 only; **steps 2-7 and their order are unchanged**, so no
existing golden step-order dependency shifts. (Alternative considered: a
separate "compute all AI inputs" pass before the whole player loop — rejected
because it would move all AI draws ahead of *all* movement, diverging from the
original's per-player interleave and needlessly perturbing draw order relative
to any future bomb/movement draw.)

The AI reads a **danger map** and an **obstacle grid** (`docs/re/ai.md` §4/§5).
Both are per-tick, derivable from `State`:
- **Danger grid** (`sub_424D37`): rebuild each tick from `State` — 1000 on
  `flame > 0` cells; for each live `Bomb`, write its threat scalar along the 4
  blast rays out to `bomb.flame`, stopping at walls/bombs, one past a brick; plus
  the closing-wall look-ahead (getvalue(910)=15) during `hurry`. Computed once
  per tick and shared by all AI players (as in the original), from `s.cells`,
  `s.flame`, `s.bombs`, and the enclosure state.
- **Obstacle grid** (`sub_409083`): 1 where a tile is solid/brick/bomb-occupied,
  else 0 — a cheap read over `s.cells` + `s.bombs`.
Both are scratch (recomputed, never hashed) — they are pure functions of hashed
state, like `s.events`.

### 6. Integer-only pathfinding (no floats)

The BFS (`sub_4092A1`/`sub_40970B`/`sub_409C1F`) is already integer: a
100-node frontier, godir steps, cost `+10` per ring, danger scores as ints. Port
it verbatim as integer C++. The only floats in the AI-adjacent original code are
in the *bomb-flight* physics (`sub_42331C` case 2, already ported integer-side)
and the *closing-wall* threat decay (plain `-= 10`, already integer) — none in
the decision/pathfinding path. The one care point is the danger scalar from bomb
field `+66` (`docs/re/ai.md` §4.2, flagged [VERIFY]): resolve it to an integer
tick/phase before use; it only orders live-bomb tiles, never vs flame's 1000.

### 7. Golden scenarios stay inert (confirmed)

`tests/sim/test_golden.cpp` drives players purely through scripted `TickInputs` (a
`pattern(t)` generator); there is **no AI-controlled player and no `ai` flag** in
any golden scenario (verified: the file constructs `Simulation(cfg)` with plain
player counts and feeds hand-built inputs). Because:

- `Player::ai` defaults `false` ⇒ `AISystem::decide` never runs in golden ⇒ **no
  AI RNG draws** are added to those ticks;
- a zero-initialised `Brain` on every (non-AI) player hashes to a constant, so
  adding the `Brain` fields to `state_hash()` shifts the golden constants **once,
  mechanically** (a pure field-addition, like past hash-layout growths) — after
  that one-time recapture every golden scenario is byte-stable across all AI
  stages;

**therefore none of the staged AI work below should move golden after the
initial `Brain`/`ai` field-addition recapture.** If a stage *does* change a
golden hash, that is a red flag that AI code leaked into a non-AI path — treat it
as a bug, not a rebaseline. New AI behaviour is validated by **dedicated AI
tests** (a headless `Simulation` with `ai=true` players and pinned brain
outcomes / input traces), never by perturbing golden.

### 8. Staged implementation plan (each stage: code + its own tests)

Landing order chosen so each stage is independently testable against the sim and
never regresses golden. Every stage cites `docs/re/ai.md`.

- **Stage 2 — skeleton + flee/danger-avoidance + the step gate.**
  Add `Player::ai`, the `Brain` struct, `AISystem`, the tick-order hook (§5), the
  danger grid + obstacle grid builders (§4/§5), the flee BFS (`sub_40970B`), the
  "walk the path" danger branch of `sub_40B20F`, the flame-veto `sub_40A76E`, the
  wander fallback `sub_40A81F`, and draws A/B. Result: an AI that runs from bombs
  and drifts when safe. Tests: place a bomb next to an AI player and assert it
  steps to a danger-0 tile (and never onto flame); assert the draw count per tick
  matches §8's list for the flee+wander paths.
  *Golden: recapture ONCE here for the `Brain`/`ai` hash-layout growth, then
  frozen for all later stages.*

- **Stage 3 — directed pathfinding to targets & powerups.** DONE 2026-07-05.
  Added the directed BFS (`sub_4092A1`), the powerup scan (`sub_409C1F`), the
  ranged-powerup pursuit `sub_40BAF5` (getvalue(920)=4), and the directed branch
  of `sub_40B20F`. Result: the AI walks toward chosen tiles and picks up nearby
  powerups. Tests: powerup within range → AI paths to and collects it; BFS
  tie-break draw fires exactly once per invocation; unreachable target is
  abandoned per the `rand()%2` give-up. *Golden: unchanged (no new hashed field —
  `pow_seek`/`path_target_*` were added + hashed in Stage 2).* Also corrected a
  Stage-2 `sub_40A59D` over-rejection: the predicate does NOT reject powerup
  tiles (docs/re/ai.md §5.6 [CORRECTED]) — required for behaviour 5's final step
  onto its target. Only AI-players' draws move, so golden stays inert.

- **Stage 4 — bomb placement (bricks) + grab-glove.** DONE 2026-07-05.
  Added `sub_40AD8D` (blast-bricks, getvalue(915)=5, the `sub_423188` drop-tile
  clearance check, the `state_flag == 9` commit) as behaviour 3, and `sub_40BD44`
  (grab-glove) as behaviour 0. Both DROP by setting the bomb-key edge (`action1`)
  on the produced `PlayerInput`, so `player_turn` runs the normal
  `BombSystem::drop`/`try_grab` (the AI is an input provider — same dud-gate RNG a
  human drop takes). Behaviour 3 does NOT flee inside itself: `state_flag=9` marks
  the commit and behaviour 2 (higher priority) paths the AI out of the new blast
  on the following tick(s). **RE corrections logged in `docs/re/ai.md`:**
  (1) `sub_423188` is a drop-tile CLEARANCE check (no bomb here + blank cell), NOT
  an escape-route search — the original does no look-ahead before dropping;
  (2) behaviour 0's "hold while carrying" is actually a **lob** — it writes the
  bomb key up (`+56=0`) every carried tick, and the mover's carried-bomb throw
  block fires on `!+56`, so a grab-AI grabs its own bomb then throws it forward
  next tick (never an indefinite hold); (3) the team compare (`+62`) reduces, in a
  no-team match, to "the bomb is mine" (`owner==self`), the same reduction the
  mover's grab block already uses; (4) the column guard's `< v2` is the documented
  undefined-edx artifact — reproduced as "no bomb already in my column ⇒ may drop".
  *Golden: unchanged — `state_flag`/`path_target` were already hashed in Stage 2,
  so NO new hashed brain field and NO golden recapture.* `tests/sim/test_ai.cpp`
  extended (blast-drop-and-flee, no-brick / column-full suppression, grab-glove,
  bombing-AI replay-hash). Behaviours 1/4/6 remain stubs (Stage 5).

- **Stage 5 — enemy targeting & aggression + remaining tunables.** DONE
  2026-07-05 — **the AI is now COMPLETE (all 8 behaviours live).** Added the
  enemy finder `sub_422718` (`pick_live_enemy`: two passes, `rand()%10` start
  each — pass 1 prefers a live human/skips other AI `+16==1`, pass 2 relaxes to
  any live opponent), the enemy pursuit `sub_40B8C2` (behaviour 6: `%50` acquire
  → finder → `%50` timeout give-up at 10 ticks → directed BFS → `%2` unreachable
  give-up), the bomb-near-enemy behaviour `sub_40ABED` (behaviour 4: column guard
  → Manhattan gate → 5-tile cross scan for a live enemy `sub_421CB5` → clearance
  `sub_423188` → `%5` drop), the punch behaviour `sub_40BE02` (behaviour 1:
  `%4` whim → orthogonal bomb scan → face + action2 edge → the shipped
  `try_punch`), and the remote-detonation whim in `sub_40B20F`'s safe branch
  (`trigger && !punch && rand()%10`). **RE findings logged in `docs/re/ai.md`:**
  (1) behaviour 4's OOB X-table `dword_45BAB0[0..4]` reads `{-1,0,0,0,1}` from the
  shipped BM95.EXE (the bytes after the 1-element `{-1}` are `0,0,0,1`), which
  with the Y-table `{0,-1,0,1,0}` forms a clean 5-tile plus/cross (LEFT/UP/SELF/
  DOWN/RIGHT) — a faithful-but-coherent OOB read, not a crash (§9.4);
  (2) behaviour 4's Manhattan gate is `abs(tileX)+abs(tileY) >= 3` over the
  actor's STALE `+20/+24` spawn/punch snapshot — a near-constant TRUE we
  reproduce from the current tile (the determinable analog, §3.4);
  (3) the enemy finder's two `rand()%10` draws are NOT in §8's table but ARE part
  of the RNG contract (inside behaviour 6's acquire). **TEAM:** the compares are
  `+62` (grab, unconditional — already reduced to owner==self in Stage 4) and
  `+84` (aggression, gated on the team-mode global `dword_464964`); with no team
  infrastructure in the sim, they reduce to `slot != self` (a `.sch` `extra`
  field exists but is unwired — team wiring is a documented follow-up).
  **NO new hashed field** (`enemy_seek` was hashed in Stage 2; no `Player::team`
  added) ⇒ *golden unchanged* — proven: a Stage-5-disabled differential build
  produces byte-identical golden hashes. `tests/sim/test_ai.cpp` +6 (punch, bomb-
  near-enemy + no-enemy control, seek-enemy latched + emergent, two-live-AI
  replay-hash); one Stage-3 emergent seed refreshed for the new draw stream.
  Tests: AI adjacent to a live enemy with an escape drops a bomb per the
  `rand()%5` gate and flees; punch fires only when a bomb is orthogonally
  adjacent and the `rand()%4` gate passes; a seek-enemy AI closes on a distant
  foe; a two-fully-live-AI replay stays bit-identical.

Deferred / out of scope: the **campaign rover/ghost** mover `sub_401B5C` and its
tunables (getvalue 1200/1205) — a different entity type, not the versus AI; port
only if campaign mode is scheduled.

## Options Considered

**AISystem writing `PlayerInput`, feeding the human path (chosen).** Faithful to
the original (the AI *is* an input source), zero divergence from the shipped
mover, and it slots into the existing `TickInputs` contract. Golden stays inert.

**A dedicated AI mover (AI moves the player directly).** Rejected: it would
duplicate the movement/bomb logic, risk drifting from the human path, and is not
what the binary does — the original explicitly funnels the AI through the same
mover as the keyboard.

**Presentation-side / non-deterministic AI (like the cosmetic RNGs).** Rejected:
the AI's decisions change gameplay state and must be identical in lockstep/replay
(ADR-0003). It has to run in the sim on `State::rng`.

## Consequences

- Easier: replays and (future) lockstep netplay include AI for free; AI is unit-
  testable headless against pinned brain outcomes; the AI can never desync from
  the mover because it shares it.
- Harder: the RNG order/count (`docs/re/ai.md` §8) must be reproduced exactly —
  every new behavior's draws land in the right place, and draws A/B bracket every
  update. One-time golden recapture for the `Brain`/`ai` hash growth in Stage 2;
  after that any golden movement is a bug.
- Revisit: the [VERIFY] items in `docs/re/ai.md` §9 (brain field packing, bomb
  `+66` scalar, drop-suppression comparands) before hashing brain state in
  Stage 2; campaign monsters if/when campaign mode is scheduled.

## Action Items

1. [x] Resolve `docs/re/ai.md` §9 [VERIFY] items against a Hex-Rays view (brain
   +2/+4/+6/+8 packing; bomb `+66`; `sub_40AD8D`/`sub_40ABED` comparands).
   DONE 2026-07-05 — see `docs/re/ai.md` §9 (RESOLVED): brain word +2 = has-
   target flag, +4/+6 = target tile X/Y, +8 = captured cost; bomb +66 =
   ELAPSED fuse phase (`100 + elapsed`); `sub_4245DA(x)` = bombs-in-column-x, but
   the `>= v2` comparand is an undefined edx (unpinnable from pseudo.c — safest
   interpretation "≥1 in column ⇒ skip", only gates Stage 4/5 behaviours);
   `sub_40ABED` scan = vertical cross with an original out-of-bounds X read;
   getvalue(905) is an unused reserved id.
2. [x] Stage 2: `Player::ai` + `Brain` + `AISystem` skeleton + flee/wander +
   danger/obstacle grids + draws A/B; add fields to `state_hash()`; recapture
   golden ONCE; add `tests/sim/test_ai.cpp`. DONE 2026-07-05 — `Player::ai` +
   `State::brains` (hashed), `libs/sim/src/systems/ai.{hpp,cpp}` (dispatcher with
   draws A/B, danger + obstacle grids, flee BFS `sub_40970B`, flame veto
   `sub_40A76E`, wander `sub_40A81F`; behaviours 0,1,3,4,5,6 stubbed in the
   dispatcher order for Stages 3-5), wired before each player's `player_turn` in
   `simulation.cpp`. Verified: AI flees a live bomb and survives; deterministic
   replay holds; RNG stream is bit-identical to a no-AI run for golden boards
   (proven A/B/D). Golden hashes need the one-time hash-layout recapture on the
   MSVC build (RNG-bearing golden assertions unchanged — the stream did not move).
3. [x] Stages 3-5 per §8, each with its own AI tests; golden stayed frozen.
   Stage 3 DONE 2026-07-05 (directed BFS `sub_4092A1`, powerup scan `sub_409C1F`,
   behaviour 5 `sub_40BAF5`, behaviour 2 directed branch; `sub_40A59D` corrected
   to not reject powerup tiles). No new hashed brain field ⇒ NO golden recapture.
   `tests/sim/test_ai.cpp` extended (seek-and-collect + emergent + replay-hash).
   Stage 4 DONE 2026-07-05 (behaviour 0 grab-glove `sub_40BD44`, behaviour 3
   blast-bricks `sub_40AD8D` with the `sub_423188` clearance check + `state_flag`
   commit; RE corrections to `sub_423188` = clearance-not-escape and behaviour 0 =
   grab-then-lob logged in `docs/re/ai.md` §3.0/§3.3). No new hashed brain field ⇒
   NO golden recapture. `tests/sim/test_ai.cpp` extended (blast-drop-and-flee, no-brick
   / column-full suppression, grab-glove, bombing replay-hash).
   **Stage 5 DONE 2026-07-05 — the AI is COMPLETE (all 8 behaviours live):**
   behaviour 1 punch `sub_40BE02`, behaviour 4 bomb-near-enemy `sub_40ABED`
   (with the RE'd OOB cross-table `{-1,0,0,0,1}`+`{0,-1,0,1,0}` and the stale
   `+20/+24` Manhattan gate), behaviour 6 seek-enemy `sub_40B8C2` + enemy finder
   `sub_422718` (two `rand()%10` passes), and the safe-branch remote-detonation
   whim (`sub_40B20F`, `trigger && !punch && rand()%10`). TEAM reduces to
   `slot != self` (no `Player::team`; team wiring is a documented follow-up).
   NO new hashed brain field ⇒ NO golden recapture (proven byte-identical via a
   Stage-5-disabled differential golden build). `tests/sim/test_ai.cpp` +6.
