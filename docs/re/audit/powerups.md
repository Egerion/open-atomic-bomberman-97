# Powerup system fidelity audit — `libs/sim/src/systems/powerups.cpp`

**Verdict: faithful.** One genuine, previously-unflagged gap (round-start
powerup baseline seeding, §1 — dormant under the shipped VALUELST, real for
any custom `start_with` override beyond bombs/flame). Everything else this
audit's brief called out — accumulation caps, head-hit kind-roll/scatter,
the scatter tile-search RNG contract, the random-powerup reroll, mutual-
exclusion eviction order, and death scatter — is a faithful, RNG-order-exact
port. The one item that looked like a bug on first read (the disease cure
roll's RNG-draw-count gating) turns out to be an **already-documented,
deliberately-accepted** deviation from a prior audit pass — re-verified
present and unchanged, cited rather than re-flagged per this audit's own
"skip documented-intentional items" rule.

Findings: **1** (new). Re-confirmed-but-not-new: 1 (disease cure roll).
Highest severity: **Medium** (finding 1 — dormant with the stock VALUELST,
real for custom tunings).

## Method

Line-by-line read of `libs/sim/src/systems/powerups.cpp` +
`libs/sim/src/systems/powerups.hpp` against:
- `native/src/game/batch_0x42459A.cpp`: `sub_42542D` (query, `@671-679`),
  `sub_4254F3`/`sub_4254B5`/`sub_425530` (pickup/clear, `@681-722`),
  `sub_4255B2` (scatter, `@724-765`), `sub_425383`/`sub_42568A` (spawn/write,
  `@646-669`/`@767-786`), `sub_425107`/`sub_425704` (flame-triggered
  relocate/reveal and burn-relocate, `@570-644`/`@788-827`).
- `native/src/game/batch_0x421E80.cpp`: `sub_421F7E` (head hit, `@112-166`).
- `native/src/game/batch_0x41DAA7.cpp`: `sub_41E16A` (mutual-exclusion evict,
  `@347-393`), `sub_41E21E` (pickup dispatch, `@395-...`), `sub_41DBFE`
  (death scatter, `@121-164`), `sub_41DF4C` (disease cure, `@269-285`).
- `native/src/game/batch_0x420D4E.cpp`: `sub_4214BC` (round-init player
  reset, `@360-458`), `sub_421CB5` (live-player-at-tile, `@663-679`, the
  *real* definition — see finding-adjacent note below).
- `native/src/game/batch_0x42583B.cpp`: `sub_425BED` (`@258-263`,
  strip-and-scatter wrapper), `sub_425C10` (`@265-280`, the 5-kind
  evictable-flag predicate).
- `docs/re/facts.md`, `docs/re/audit/diseases.md`, `docs/valuelst-map.md`,
  `libs/sim/include/bomber/sim/tuning.hpp`.

Cross-checked `sub_421CB5`'s role in `sub_4255B2`'s occupancy test: one
in-batch forward-decl comment in `batch_0x42459A.cpp:69` mislabels it
"flame-at-cell query", but every OTHER batch that forward-declares it
(`batch_0x401010.cpp:68`, `batch_0x40A140.cpp:46`, `batch_0x41F29B.cpp:115`,
`batch_0x422DDD.cpp:92`) — and its real definition,
`batch_0x420D4E.cpp:663-679` — agree it is the live-player-at-tile lookup
(it walks the player array and requires the present flag set and the
died flag clear, i.e. present && !dead). The port's `grid::player_at` and
its citation are correct; the one mislabeled comment is a guess-comment
artifact of that TU's own forward-decl block, not a second definition.

## Findings

### 1. Round-start powerup baseline is only seeded for ExtraBomb/Flame, not all 13 kinds

**Original.** `sub_4214BC` (`batch_0x420D4E.cpp:360-458`, pseudo.c
~23880-23935), the per-round player reset, includes a two-line loop
(`@427-428`): for `j` = 0..14 ascending, the player record's byte at
`+86 + j` ← `getvalue(50 + j)` (i.e. `sub_412135(j + 50)`).

This directly writes **every** kind's accumulated-count byte (`+86..+100`,
kinds 0..14 — the 13 real kinds plus 2 unused pad slots) from its VALUELST
`start_with` baseline (`getvalue(50+kind)`), for **every** player, **every**
round. It is a raw byte write — no `PowerupSystem::apply`-equivalent
clamp/evict logic runs here (that only happens for the one subsequent
Goldman-wheel bonus increment, `@430-449`, already correctly ported as
`MatchConfig::born_with_extra`).

**Port** (`libs/sim/src/setup.cpp:112-116`):

```cpp
p.speed = s.tuning.start_speed;
p.max_bombs = s.tuning.start_with[static_cast<int>(PowerupType::ExtraBomb)];
p.flame = s.tuning.start_with[static_cast<int>(PowerupType::Flame)];
for (int k = 0; k < kPowerupKinds; ++k)
    if (config.born_with[k]) powerups.apply(p, static_cast<PowerupType>(k));
```

Only kinds 0 (ExtraBomb) and 1 (Flame) get a direct baseline assignment.
The other 11 kinds (Kick, Skate, Punch, Grab, Spooger, Goldflame, Trigger,
Jelly, plus Disease/SuperDisease/Random which have no count byte anyway)
have **no** `start_with[k]` seeding path at all — they rely entirely on
`Player`'s C++ default member initializers (`kick=false`, `skates=0`, …,
`libs/sim/include/bomber/sim/player.hpp:37-53`) plus whatever
`config.born_with[k]` (the *scheme's* `-P` "born with" row, a distinct
concept from the VALUELST baseline — `libs/assets/src/sch.cpp:76`,
`libs/match/include/bomber/match/match_factory.hpp:108`) happens to flag.

**Visible effect.** With the shipped VALUELST (`start_with = {1,2,0,0,0,0,
0,0,0,0,0,0,0}`, `tuning.hpp:58`) this is silent: kinds 2..12's baseline is
already 0, matching the hardcoded C++ defaults exactly — no observable
divergence, and this is presumably why it survived every existing golden
test. It becomes visible the moment a VALUELST sets any of ids 52-62 (Kick,
Skate, Punch, Grab, Spooger, Goldflame, Trigger, Jelly) to a nonzero
baseline — a real customization category, since `Tuning::apply()` is
explicitly designed to keep "modified VALUELST files... working"
(`tuning.hpp:8`) and the port's own default table already demonstrates
nonzero baselines are a real shipped case (Flame=2). Under such a config
every player would silently start the round without the granted power in
this port, while the original hands it to them for free before round one.

**Severity:** Medium — zero effect under the shipped/default tuning
(explaining why no golden hash caught it), but a genuine, unconditional
fidelity gap for any VALUELST override of ids 52-62, which is squarely
inside this project's stated compatibility goal.

**Confidence:** High on the original's behaviour (the loop is unambiguous,
directly adjacent to the already-cited/ported Goldman-wheel block in the
same function). Medium on the fix shape: whether the scheme's `-P`
`born_with` flag is meant to be an *additional* per-scheme grant on top of
this baseline (as the port currently treats it, via
`PowerupSystem::apply`'s evict-aware path) or is itself just an authoring
front-end for the same `start_with` ids, wasn't resolved by this pass — the
`.SCH`→VALUELST interaction at load time is outside `powerups.cpp` (it's
`libs/assets/src/sch.cpp` / `libs/match`), so flagged for the setup/scheme
audit (ledger item #10) rather than fully re-derived here.

**Suggested fix.** In `libs/sim/src/setup.cpp`'s per-player init block, seed
all 13 kinds directly from `s.tuning.start_with[k]` (matching
`sub_4214BC`'s raw byte-write loop) before the existing `born_with`
overlay loop runs — e.g. a `PowerupSystem::reset_to_baseline`-style call (or
a small inline switch mirroring it) for `k = 0..kPowerupKinds-1`, replacing
the current two hand-picked lines. Recapture goldens only if a golden
config's tuning actually sets a nonzero baseline outside ids 50/51 (none
currently do, per the "visible effect" note above).

## Re-confirmed, not newly flagged (documented deviation)

### Disease cure roll: RNG draw gated on `disease_timer > 0`

`sub_41E21E`'s pre-switch cure roll (`batch_0x41DAA7.cpp:404-414`) draws
`rand() % max(getvalue(125),1)` on **every** pickup whenever
`diseases_curable` (id 124) is true, independent of whether the picking-up
player currently has a disease — `sub_41DF4C` unconditionally re-zeroes the
disease fields either way, so curing a healthy player is a no-op with real
effect but the RNG draw still happens. `DiseaseSystem::maybe_cure_on_pickup`
(`libs/sim/src/systems/diseases.cpp:75-79`) short-circuits the draw itself
behind `p.disease_timer > 0`, via `&&`. This is the exact deviation already
identified, analyzed, and **deliberately accepted** by the 2026-07-10 audit
pass: `docs/re/facts.md:2508-2514` ("outcome-identical... RNG-draw-count
differs only on the already-documented healthy-pickup path") and
re-confirmed present-and-unchanged by `docs/re/audit/diseases.md:250-257`
("the one deliberately-not-replicated RNG-draw-count deviation... not
re-flagged here"). This pass independently re-derived the same root cause
from `sub_41E21E`/`sub_41DF4C` before finding the existing citations, and
concurs it is out of this audit's scope to re-litigate — cited here per the
brief's "skip documented-intentional items" instruction, not re-opened.

One sub-byte not previously called out: the original's modulus is clamped
to a **minimum of 1** (`dword_46498C = max(getvalue(125),1)`,
`batch_0x410401.cpp:510-514`) so it *never* skips the draw even when
`diseases_curable` is on and `getvalue(125)==0` (that config makes
`rand()%1` always 0, i.e. "always cure"). The port's guard is
`disease_cure_chance > 0 && random_below(...)==0`, which skips the draw
entirely (and never cures) at `disease_cure_chance<=0`. Negligible in
practice (default id 125 = 10, `tuning.hpp:198`; only reachable with a
deliberately-degenerate custom VALUELST) and downstream of the
already-accepted gating deviation above, so not raised as an independent
finding — noted for whoever eventually revisits `maybe_cure_on_pickup`.

## Verified faithful (no change)

- **Accumulation caps (VALUELST 550-562).** `sub_41E21E`'s post-switch
  clamp (`if (getvalue(kind+550)) { if (count>limit) count=limit; }`,
  `@487-494`) is strictly-greater-than, matching `PowerupSystem::apply`'s
  `limited()` lambda (`min(v, lim)` when `lim>0`) exactly for the counted
  kinds (ExtraBomb/Flame/Skate). For the seven flag kinds (Kick, Punch,
  Grab, Spooger, Goldflame, Trigger, Jelly, each capped at limit 1 in the
  shipped VALUELST) the original's raw counter would otherwise exceed 1 on
  repeat pickups absent an intervening eviction, but the same post-switch
  clamp fires after *every* pickup and forces it back to 1 — making the
  byte counter behave exactly like a boolean, which is what the port
  stores directly. Confirmed equivalent by construction, not just by
  default-tuning coincidence.
- **Head-hit scatter** (`PowerupSystem::head_hit` vs `sub_421F7E`,
  `batch_0x421E80.cpp:112-166`): the stun write (the stun countdown word at
  `+58` ← 16), drop count
  `powers_lost_min + rand()%max(1,powers_lost_rand)` — computed with the
  identical clamp-then-modulus order — the original's pre-decrement
  "count down until it passes −1" loop running exactly `n` times, the inner
  `rand()%15` kind roll (up to
  200 tries) gated on `held_count(kind) > start_with[kind]` (strict
  greater-than: the held count must exceed the baseline, at `@151`), and the
  break-on-first-hit
  semantics all match line-for-line. `sub_425BED(player, kind)` is confirmed
  (`batch_0x42583B.cpp:258-263`) to be a **thin wrapper that ignores its
  player-base argument and just calls `sub_4255B2(kind)`** (scatter) — the
  original scatters *then* decrements the counter byte, while the port
  calls `remove()` then `scatter()` (reversed order). Confirmed harmless:
  `remove()` draws no RNG and never touches board state, so the swap
  changes neither the RNG stream nor the outcome — verified faithful
  despite the cosmetic reordering.
- **`rand()%15` reading past the 13 real kinds (indices 13/14).** Both
  `sub_421F7E`'s kind roll and `sub_41DBFE`'s death-scatter loop iterate a
  wider index space than `kPowerupKinds` (15 for the head-hit roll range,
  0..14 for the death loop). Traced to `sub_4214BC`'s same baseline loop
  (`@427-428`, `j<15`): index 13/14's counter byte is *initialized to*, and
  never subsequently written away from, `getvalue(50+j)` — so the surplus
  test (`count > baseline`) is always `false==false`, never fires, for
  those two pad slots. `PowerupSystem`'s `held_count`/`surplus` gating on
  `kind < kPowerupKinds` is behaviorally identical (always excluded, not
  just conveniently never observed) — verified faithful, not merely
  untested.
- **`sub_4255B2` scatter tile search** (`PowerupSystem::scatter`,
  `batch_0x42459A.cpp:724-765`): the `kind==13` (clogs, outside
  `kPowerupKinds`) short-circuit is unreachable in the port by construction
  (clogs is never represented as a `PowerupType`, per
  `tuning.hpp`/`player.hpp`'s own citations) — consistent, not a gap. Two
  RNG draws per roll (x via `rand()%w`, y via a *second* `rand()` whose
  remainder is taken — not a re-use of the x draw), matching the port's
  `random_below` × 2. The inner guard is decremented *before* the tile-type
  test (the pre-decrement "inner budget exhausted → break" test runs ahead
  of the `sub_425FB9` call), so the 100th roll of
  an inner budget is drawn but never tested — matching the port's
  `if (--guard <= 0) return` placement exactly (both draw-then-check).
  Outer retry cap of 100, each with a fresh inner budget of 100, matches.
  Occupancy predicate — free tile (`sub_425FB9==0`) required first (solid/
  brick tiles silently re-roll, burning no outer attempt), then rejected
  (burning one outer attempt) on a **grounded** bomb (`sub_422E48`, which
  itself excludes motion states 2/3 — punched-flying and carried-in-hand —
  matching the port's `bomb_at`'s `!b.flying` exclusion; state 0/1
  kicked-sliding bombs are correctly still occupancy-blocking in both), any
  powerup record (`sub_42542D`), or a live present-and-not-dead player
  (`sub_421CB5`, confirmed above) — matches `PowerupSystem::scatter`
  exactly, including the burning-tile carve-out already documented in the
  port's own comment (a scattered token CAN land on burning ground, the
  original never checks `s.burning`... *correction*: the port's code
  actually does check `s.burning[y][x] > 0` alongside `cells != Blank` in
  the free-tile gate. Re-checked against `sub_425FB9` — that helper itself
  is the single "tile type" query used throughout the codebase for the
  cell-type + brick-crumble check, and `docs/re/facts.md`'s existing
  "Scatter occupancy test" entry already pins its 0/1/2 semantics; nothing
  in this pass contradicts the current `Cell::Blank && !burning` gate).
- **Random powerup resolution** (`simulation.cpp:83-98` vs `sub_41E21E`
  case `0xC`, `@476-483`): up to 200 tries of `rand()%12` (Random itself,
  kind 12, excluded from the modulus range), accept the first roll whose
  `dword_4647E0[kind]` (scheme-forbidden table, `s.forbidden` in the port)
  is false, then re-enter the pickup-dispatch switch from the top as that
  kind — one draw per try,
  same order, same 200-try cap, same "fully exhausted → no effect" fallback
  when every kind is scheme-forbidden. Matches.
- **Mutual-exclusion eviction** (`PowerupSystem::apply`'s per-case `evict()`
  calls vs `sub_41E21E`'s per-case `sub_41E16A()` calls,
  `batch_0x41DAA7.cpp:432-465`): increment-then-evict order, and — critical
  for the RNG-draw-order contract, since eviction draws `scatter()` — the
  exact per-case evict ORDER all match: Punch→evict Trigger; Grab→evict
  Spooger; Spooger→evict Grab; Trigger→`trigger_placed=0` THEN
  trigger=true THEN evict Punch THEN evict Jelly; Jelly→evict Trigger.
  `sub_425C10`'s evictable-kind predicate (`batch_0x42583B.cpp:265-280`,
  true only for {5,6,7,9,10} = Punch/Grab/Spooger/Trigger/Jelly) matches
  the port's `evict()` never being called for any other kind. The
  Trigger-eviction bomb downgrade (`sub_424C47`, converting live trigger
  bombs of the evicted owner to normal timed bombs with a fresh full fuse,
  gated on `a2==9 && !trigger_flag`) matches `PowerupSystem::evict`'s
  owner-scoped bomb/carried-bomb downgrade loop, including firing only
  when the flag actually cleared.
- **Death scatter** (`PowerupSystem::death_scatter` vs `sub_41DBFE`,
  `batch_0x41DAA7.cpp:121-164`, confirmed via its real call site at
  `batch_0x41F29B.cpp:826-850` — the death-animation-complete
  path, exactly as the port's own citation claims; one in-batch forward-decl
  comment elsewhere mislabels this function "clear disease timers", another
  guess-comment artifact like the `sub_421CB5` one above, contradicted by
  its real body which is unambiguously the powerup surplus-scatter): the
  flag-kind branch (`sub_425C10` true — scatter once, reset straight to
  baseline) vs the counted-kind branch (scatter-and-decrement in a loop
  while `count>baseline`) collapse, for real gameplay kinds, into the exact
  single `for (have = held_count; have > baseline; --have) scatter()`
  the port uses (every flag kind's count is always 0 or 1, so "scatter once
  and reset" and "scatter-while-decrementing" are the same sequence of RNG
  draws when they ever differ by at most 1). Kind range 0..14 in the
  original again includes the two always-inert pad slots, matching the
  `kind < kPowerupKinds` note above. The port's death-tick-vs-anim-end
  timing collapse is already explicitly flagged as a deliberate, documented
  divergence in the port's own comment — not re-litigated here.
- **Goldflame stored as a flag, not `flame=99`** (`PowerupSystem::apply`
  case `Goldflame`): confirmed against `sub_41E21E` case 8, which
  increments the player record's byte at `+94` and nothing else — no
  interaction with the flame counter byte at `+87` —
  the blast-reach computation itself is outside `powerups.cpp`
  (`BombSystem::place`, ledger item #2); not re-derived here.
- **Hidden-under-brick reveal/relocate** (`sub_425107`) — this audit's
  brief lists it under `powerups.cpp`'s scope, but the only live call site
  (`batch_0x422DDD.cpp:870`, the flame-ignition path) and the port's actual
  implementation both live in `FlameSystem` (`libs/sim/src/systems/
  flames.cpp:159-226`, `relocate_overpowered_here` + the reveal block in
  `spread_to`), which is ledger item #3, already carrying its own detailed,
  disassembly-pinned citations (draw order, the two-pass 200-try swap/move
  search, the always-both-draws-even-on-reject contract). Spot-checked
  against `sub_425107` (`batch_0x42459A.cpp:570-644`) during this pass —
  consistent with the existing `flames.cpp` citations — but the full
  differential belongs to that system's own audit pass, not duplicated
  here.

## Provenance

`native/src/game/batch_0x42459A.cpp` (powerup grid: query/pickup/spawn/
scatter/relocate, `@1-829`), `batch_0x421E80.cpp` (head hit, `@112-166`),
`batch_0x41DAA7.cpp` (mutual-exclusion evict `@347-393`, pickup dispatch
`@395-514`, death scatter `@121-164`, disease cure `@269-285`),
`batch_0x420D4E.cpp` (round-init `@360-458`, live-player query
`@663-679`), `batch_0x42583B.cpp` (`sub_425BED`/`sub_425C10`,
`@258-280`), `batch_0x422DDD.cpp` (bomb-at/motion-state exclusion,
`@149-171`, `@490-920`), `batch_0x410401.cpp` (`dword_464980`/
`dword_46498C` init, `@505-519`). Cross-checked against
`docs/re/facts.md` ("Disease system", "Overpowered-powerup relocation",
"Scatter occupancy test", the 2026-07-10 audit block `@2411-2584`),
`docs/re/audit/diseases.md`, `docs/re/audit/flames.md`, and
`docs/valuelst-map.md` (ids 50-62, 400-412, 550-562, 670-671).
