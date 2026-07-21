# Fidelity audit — `libs/sim/src/systems/bombs.cpp` (+ `bombs.hpp`)

**Verdict: not clean.** Four genuine, unpinned divergences found (one
critical-severity, two high-severity "everywhere" arithmetic gaps, one
low-severity edge case). This system had already absorbed a large amount of
prior RE work (Core-feel audit 2026-07-10, Bomb/warphole reconciliation,
Chain-reaction timing, Bomb capacity, Trigger allowance, Goldflame — all in
`docs/re/facts.md`), so most of the placement/kick/punch/throw/grab/dud
logic is already faithful and well-cited; this pass adds four findings that
slipped past those earlier reads, plus a large coverage list of what was
re-verified and left untouched.

Method: line-by-line diff of `bombs.cpp`/`bombs.hpp` against the faithful
transliteration (`native/src/game/batch_0x422DDD.cpp` — `sub_422DDD`,
`sub_422E48`, `sub_422EDE`, `sub_4230A5`, `sub_423188`, `sub_423209`,
`sub_42325D`, `sub_42331C`; `native/src/game/batch_0x42459A.cpp` —
`sub_42459A`, `sub_4245B9`, `sub_4245DA`, `sub_42464B`, `sub_424708`,
`sub_42475F`, `sub_4247C5`, `sub_4248C6`, `sub_424987`, `sub_4249EA`,
`sub_424A50`, `sub_424AF4`, `sub_424B41`, `sub_424C47`), cross-checked
against `D:\...\BOMBRMAN\pseudo.c` at the cited line numbers (the native
`.cpp` transliterations proved to match pseudo.c verbatim at every spot
checked in this pass — no fresh disasm work was needed beyond what's already
flagged `HEXRAYS-FIX` in those files).

---

## Finding 1 — Explosions freeze once the round is down to ≤1 side; the port never freezes

**Severity: Critical** (changes the outcome of essentially every round's
final seconds). **Confidence: High** (plain, unflagged, unambiguous
arithmetic; no `HEXRAYS-FIX` register-loss caveat on this branch; corroborated
by two independent call sites elsewhere in the binary with matching
"round decided" semantics).

**Original.** `sub_42331C`'s per-bomb tail (pseudo.c 25601-25680,
`native/src/game/batch_0x422DDD.cpp:799-882`) gates the *entire* fuse-elapsed
accrual **and** the explosion-timeout check — and therefore the flame-arm
spread loop nested inside it — behind:

```c
// pseudo.c 25603
if ( sub_421969() > 1 )
{
  if ( *(_DWORD *)v75 != 2 && *(_WORD *)(v75+46) != 2 && *(_WORD *)(v75+46) != 3
    && *(_DWORD *)(v75+4) != 1 && *(_BYTE *)(v75+16) == 9 )
    *(_WORD *)(v75 + 68) += dword_464958;                 // fuse-elapsed accrual
  if ( *(_WORD *)(v75 + 68) >= *(_WORD *)(v75 + 74) )      // timeout check
  {
    *(_DWORD *)v75 = 0;
    ...
    for ( k = 0; k < 4; ++k ) { ... flame-arm spread ... }  // pseudo.c 25640-25706
  }
}
```

`sub_421969` (pseudo.c 24048-24056, `batch_0x420D4E.cpp:524-532`):

```c
int sub_421969()
{
  if ( dword_46489C ) return 2;        // campaign mode: force "always >1"
  if ( dword_464964 ) return dword_4621DC;  // team mode: teams-alive count
  return dword_4621D4;                 // standard match: players-alive count
}
```

`dword_46489C` is the campaign-mode flag (set only from the campaign-select
menu, `batch_0x401010.cpp:354`; cleared at `batch_0x4293E5.cpp:954`) — so
this freeze is **exempted in campaign** (rovers/ghosts aren't "players", the
round doesn't end by elimination) but **applies in standard and team
matches**. The chain-detonation queue drain (top of `sub_42331C`, pseudo.c
25276-25290) unconditionally force-sets a queued bomb's elapsed to its
duration (`+68 = +74`), but that write is inert without this same gate —
so **chain reactions, trigger presses, and slide/land-into-flame contact all
stop propagating too**, not just plain fuse timeout. A second call site
(`batch_0x4293E5.cpp:1060`, `if (sub_421969() <= 1) sub_410522();`) fires a
distinct "round decided" handler on the exact same predicate, reinforcing
the reading.

**Port.** `BombSystem::tick_fuses` (`libs/sim/src/systems/bombs.cpp:554-567`)
decrements every armed bomb's fuse and calls `flames_.explode(i)` on
timeout unconditionally — no alive/side-count check anywhere in `bombs.cpp`,
and `Simulation::tick` (`libs/sim/src/simulation.cpp:682-685`) calls
`bombs.advance_bombs()`/`bombs.tick_fuses()` every tick regardless of
`alive_count(s)`/`sides_remaining(s)` (both already exist and are exactly
the primitives needed — `libs/sim/src/simulation.cpp:760-791`).

**Visible effect.** The instant a round drops to one remaining side (a kill
lands), the original **freezes every still-armed bomb on the field solid** —
no more fuse countdowns, no chain propagation, no trigger detonations —
until the round transitions. Our port keeps ticking: a bomb planted earlier
by the loser (or the winner) can still go off after the round is
"decided," potentially killing the sole survivor, burning terrain that
should have stayed put for the round-end screenshot, or completing a chain
the original would have left half-finished. This is reachable on **every
round that ends by elimination** (i.e., every standard/team round, not just
rare setups) whenever a live bomb is still armed at the moment of the last
kill — extremely common with any kind of crowded endgame.

**Suggested fix.** Gate `BombSystem::tick_fuses` (and, to mirror the queue
drain being inert too, the `flames_.explode`/chain-queue consumption it
triggers) on `sides_remaining(s) > 1`, with an exemption while
`!s.rovers.empty()` (campaign — mirrors `dword_46489C`'s forced `return 2`).
`sides_remaining()` already generalizes free-for-all (team 0 = distinct
sides) and team mode identically to `dword_4621D4`/`dword_4621DC`, so no new
primitive is needed. Needs a new test (freeze a field with 2 alive down to
1, confirm an already-armed bomb never explodes) and a golden recapture if
any golden scenario's tail overlaps a last-kill-with-live-bomb situation.

---

## Finding 2 — [WITHDRAWN] "Kicked/conveyor bombs are missing the flat +100/tick ground bonus"

**Status: FALSE POSITIVE. Applied 2026-07-20, then REVERTED the same day
after the native oracle contradicted it.** The port's base-speed slide was
already correct.

**The misread.** `sub_42331C`'s case 0 (bomb resting on a conveyor) and case
1 (kicked bomb already sliding) both fall into `LABEL_21` (pseudo.c
25393-25400, `batch_0x422DDD.cpp:512-554`). The finding quoted only the first
two lines and elided the rest with `...`:

```c
case 1:
  *(_DWORD *)(v75 + 116) += dword_464958 * *(_DWORD *)(v75 + 112) / (unsigned int)dword_46494C;  // += speed
LABEL_21:
  *(_DWORD *)(v75 + 116) += 100;                          // budget += 100
  *(_DWORD *)(v75 + 28) -= dword_45BECC[*(int *)(v75 + 42) >> 16];  // pos_x -= dir step  <-- ELIDED
  *(_DWORD *)(v75 + 32) -= dword_45BEDC[*(int *)(v75 + 42) >> 16];  // pos_y -= dir step  <-- ELIDED
  break;
```

The two elided lines are a **one-step position backoff**: before the shared
per-frame move loop runs, the bomb's position is stepped back one direction
unit. The move loop spends budget at 100 units per step, so the `+= 100`
funds exactly one step forward — which just re-establishes the position the
backoff removed. **The +100 and the backoff cancel; the net per-frame
displacement is the speed term alone.** The `getvalue(300)` (kicked) /
`getvalue(190+idx)` (conveyor) value IS the authentic per-tick rate, with no
bonus. (Flight, case 2, has neither the +100 nor a backoff — a genuinely
different path.)

**Oracle adjudication.** The `--oracle` native run slides a kicked bomb
~0.25 tile/tick at BOTH 1x and 9x frame cadence — cadence-invariant, exactly
what "the +100 is a wash" predicts (if it were a real per-frame bonus it
would scale with cadence and 9x would be ~9x faster). The clean-room's
pre-fix base-speed slide (1000 units/tick = ~0.25 tile/tick) matched this;
the "fix" that folded `+100 * kSubFrames` (=1900 units/tick) ran ~1.9x too
fast and DIVERGED from the oracle at t=4. Reverted in
`bombs.cpp advance_bombs`/`conveyor_carry`, `tests/test_fidelity_audit.cpp`,
`tests/test_kick_nuances.cpp`, golden E (`test_golden.cpp`), and the visual
goldens.

**Not to be confused with the rover +100** (rovers.cpp, `sub_401B5C`): that
IS a real net bonus, because that different function has NO paired backoff —
it computes the candidate as one pixel *forward* from the current position,
so its `+100` genuinely nets one extra pixel/frame (a rover/ghost advances
even at speed 0). Only the bomb path (`sub_42331C`) has the cancelling
backoff. The two must not be folded the same way.

---

## Finding 3 — A conveyor-carried bomb should freeze the instant it steps off the belt, not coast at kicked speed

**Severity: High** (common on any board with conveyors — changes where a
belt-launched bomb ends up on the very next tile past the belt's end).
**Confidence: High** — and INDEPENDENT of the withdrawn Finding 2. Finding 2
was about the slide *speed* (the +100, which turned out to be a wash); this
finding is about the motion *word* (`+46`) never being set to 1 in case 0, so
a belt bomb freezes the instant it leaves the belt rather than coasting. That
is a separate fact, and the native oracle VALIDATED it (the coast-stop
matches).

**Original.** Case 0 (pseudo.c 25362-25392, `batch_0x422DDD.cpp:512-547`)
is re-entered from the top of the per-bomb switch **every tick**, and
re-checks whether the bomb's *current* tile still carries a conveyor actor
(`v59 = sub_405654(col,row); if (v59 && v59[1]==2)`). If it does, the bomb
is pushed (case 0's body, falling into the shared movement loop via
`LABEL_21`, same as a kicked bomb). If it does **not** (the belt ended, or
never started), the case falls straight to `goto LABEL_115` — **skipping
the entire movement loop for that tick** — and, critically, the bomb's
motion word (`+46`) is **never written to 1** anywhere in case 0. So a
conveyor-only bomb's motion state stays 0 forever; the moment its resting
tile (at the *start* of a tick) is not a conveyor, it simply stops being
processed — it does not carry kicked-bomb momentum onto the next tile.
(Only `sub_42464B`, the kick handler, ever writes motion state 1 — see
`batch_0x42459A.cpp:120-144`, pseudo.c 25815-25839.) Also visible from this:
`sub_4247C5` ("kick + action2: stop my sliding bombs", pseudo.c 25877-25890)
gates on `*(_WORD*)(v4+46) == 1` — motion state **1 specifically** — so the
"stop own bombs" ability can halt a kicked bomb but structurally **cannot**
touch a conveyor-riding one (state stays 0).

**Port.** `BombSystem::conveyor_carry` (`bombs.cpp:515-530`) sets
`b.moving = true` **once**, the first tick a resting bomb is found on a
belt tile; from then on `advance_bombs` (`bombs.cpp:532-552`) sees
`b.moving == true` and calls `slide()` every tick with
`on_belt ? conveyor_speed() : kicked_bomb_speed` as the budget — i.e. once
the bomb leaves the belt, it keeps sliding at **kicked-bomb speed**
indefinitely (until an obstacle or an explicit stop), rather than freezing.
There is no field distinguishing "started moving via a kick" from "started
moving via a conveyor push," so `stop_own_sliding` (`bombs.cpp:246-253`,
gated only on `b.moving && !b.jelly`) also incorrectly lets a kicking
player's "stop my bombs" action halt their own conveyor-riding bombs, which
the original's motion-state-1-only gate cannot do.

**Visible effect.** A bomb pushed off the end of a conveyor belt keeps
gliding across open floor at kicked speed (potentially several extra tiles,
compounding with Finding 2's speed error) instead of stopping dead the
tick it clears the belt — a large, easily-noticed difference on any board
where a belt terminates onto open ground. Separately, "kick + action2 stop
my bombs" can freeze a belt-ridden bomb the original could never touch that
way.

**Suggested fix.** Track conveyor-origin motion as its own state (e.g. a
`Bomb::on_belt`/motion-kind distinction, or re-running the "is my current
tile still a conveyor" check every tick before continuing to slide a
belt-origin bomb, clearing `moving` the instant it isn't) so it freezes off-belt
instead of falling into the kicked-speed budget, and exclude that state from
`stop_own_sliding`'s target set. Needs new `tests/test_stage_actors.cpp`
cases (a short belt ending on open floor: bomb stops exactly at the belt's
last tile edge, does not coast further; "stop own bombs" has no effect on a
belt-riding bomb) and a golden recapture if any scenario uses conveyors.

---

## Finding 4 — Trigger detonate can fire on a bomb placed the exact same tick

**Severity: Low** (narrow timing window: requires a player to place a
trigger bomb and press the trigger-detonate action in the *same* tick, with
no older live trigger bomb available). **Confidence: High** (plain
unflagged arithmetic).

**Original.** `sub_424B41` (pseudo.c 26027-26067,
`batch_0x42459A.cpp:332-371`) scans for the OWNER's oldest live trigger
bomb by minimum creation stamp, seeded with the **current** tick:

```c
v5 = dword_464994;   // this tick's stamp — the seed/threshold, not +infinity
v6 = -1;
...
if ( ... && *(_DWORD *)(v4 + 64) < v5 )   // STRICTLY earlier than the running best
{ v5 = *(_DWORD *)(v4 + 64); v6 = v3; }
...
if ( v6 != -1 ) sub_423209(...);          // only fires if something matched
```

A bomb's creation stamp (`+64`) is set to `dword_464994` at placement time
(`sub_422EDE`, pseudo.c 25113 `v18[16] = dword_464994;`). Since
`dword_464994` only increases and is incremented once per frame *before*
the player-action pass (`docs/re/facts.md` "Per-tick call order" step 3), a
bomb placed **this same tick** has `+64 == v5` at the moment of the scan —
which fails the strict `<` test. If a player's only candidate trigger
bomb(s) were all placed this same tick, `v6` stays `-1` and **nothing
detonates this tick** (the player must wait at least one tick before their
freshly-placed trigger bomb becomes remote-detonable).

**Port.** `BombSystem::detonate_triggered`
(`bombs.cpp:196-218`) returns on the first matching bomb in vector order
with no creation-tick check at all:

```cpp
for (std::size_t bi = 0; bi < s_.bombs.size(); ++bi) {
    Bomb& b = s_.bombs[bi];
    if (b.active && b.trigger && b.owner == owner && !b.flying) {
        flames_.queue_chain(b.id);
        return true;
    }
}
```

The existing code comment ("our creation-ordered vector's first match
reproduces [the oldest-by-stamp scan]," also asserted in `facts.md` Core-feel
audit item 4) is correct for the common case but misses this specific
same-tick edge: it doesn't reproduce the *exclusion* of a bomb created this
very tick, because `Bomb` doesn't currently store a creation-tick stamp at
all to check against.

**Visible effect.** In the rare case a player's whole live trigger-bomb set
was placed on the identical tick as their trigger-detonate press (e.g. a
spooge run of trigger bombs immediately followed, same tick, by an action
press that also reads as trigger-detonate), the original silently no-ops
that press; the port detonates the oldest one in placement order right
away.

**Suggested fix.** Add a hashed `Bomb::created_tick` field (set in
`BombSystem::place`/`throw_carried` from `s_.tick`), and in
`detonate_triggered` track the minimum among candidates with
`created_tick < s_.tick`, matching the strict inequality — rather than
"first in vector order." Needs a new regression test (place + same-tick
trigger press → no detonation; next tick → detonates) and a golden check
(low risk of moving any existing golden, since it requires simultaneous
placement+press).

---

## Verified faithful (no change)

Re-derived from the transliteration/pseudo.c during this pass and found to
already match the port, or already correctly documented as a deliberate,
cited deviation:

- **Bomb kind exclusivity + trigger-over-jelly override order**
  (`sub_41EB13`/`sub_422EDE`, `bombs.cpp:place` lines 21-43) — jelly sets
  kind 2 first, trigger allowance then overrides to kind 1; matches.
- **Short-flame/goldflame ordering** (`sub_41EB13`) — short-flame forces 1,
  goldflame then overrides to `max(gridW,gridH)`; matches
  (`bombs.cpp:37-41`).
- **Dud gate wall-clock→tick substitution** — the original gates duds on
  real wall-clock `time_()`; the port deliberately measures the same
  seconds-derived thresholds in ticks for determinism. Already documented
  (`facts.md` "Dud bombs") and correctly cited in `bombs.cpp:44-61`.
- **Kick sound/redirect gating** (`sub_42464B`, pseudo.c 25815-25839) —
  sound plays unless (already-moving AND same direction); redirect snaps to
  tile centre and re-arms speed; same-direction re-kick is a silent no-op.
  Matches `bombs.cpp:220-244`.
- **Kick+action2 "stop own bombs" jelly exclusion** (`sub_4247C5`) — kind
  != 2 (jelly) gate matches `bombs.cpp:246-253` (motion-state-1-only gate
  is Finding 3's gap, everything else here is faithful).
- **Trigger-detonate motion exemption** (`sub_424B41`) — excludes only
  motion 2 (flying) and 3 (carried); a sliding trigger bomb is a legal
  target. Matches `bombs.cpp:196-218` (the timing nuance is Finding 4).
- **Grab underfoot probe** (`sub_422E48` via `bombs.hpp`'s doc) — motion 0
  and 1 both qualify (can grab your own sliding bomb); flying/carried
  excluded. Matches `try_grab` (`bombs.cpp:139-169`).
- **Throw restarts the fuse from the creation-time duration, not the
  frozen remnant** (`sub_41F29B` LABEL_246 `+68 = 0` before launch) —
  matches `throw_carried`'s use of `fuse_init` (`bombs.cpp:171-194`).
- **Bomb/warphole reconciliation** — bombs never warp; a warphole tile
  blocks a sliding bomb exactly like a wall (`sub_4230A5`/`sub_423188`),
  and a flying bomb cannot settle on one but hops onward
  (`sub_42331C` ~pseudo.c 25451-25461). Matches `slide()`'s cell-entry
  probe (`bombs.cpp:474-481`) and `fly()`'s landing verdict
  (`bombs.cpp:332-364`); extensively tested in `tests/test_stage_actors.cpp`.
- **Dirarrow re-steer gated on exact tile-centre on both axes**
  (`!v79 && !v80`, pseudo.c ~25532-25542) — matches `slide()`'s
  `at_centre` check (`bombs.cpp:404-434`).
- **Jelly bounce vs non-jelly stop** on both a blocked cell and a
  flame-contact — jelly reverses `(dir+2)&3` and keeps moving (ping-pong,
  sound 135), non-jelly halts (sound 130). Matches `slide()`
  (`bombs.cpp:385-397`, `482-500`).
- **Jelly veer roll during flight** — chance `1/max(1,getvalue(667))`,
  side `rand()%2`, rolled only once the bomb has travelled ≥3 tiles
  (`+72 >= 3`), only for kind==2. The port's per-hop `to_x/to_y`
  bounds check is structurally different from the original's per-pixel
  `v66/v65` in-bounds check but provably equivalent at the only boundary
  crossings that matter (crossings before the 3-tile threshold are inert
  in the original regardless of bounds, and the port's launch geometry
  already only checks at the hop-completing boundary). `bombs.cpp:271-283`.
- **Landing-tile occupancy verdict for a flying bomb** — wall, resting
  bomb, and ANY floor powerup (hidden or visible) all block a landing; a
  live player check is nested *inside* that clear verdict and runs before
  the warphole probe; a warphole blocks settling but not the victim scan.
  Matches `fly()` (`bombs.cpp:295-364`); extensively tested and previously
  corrected per `facts.md` "Chain-reaction timing" / "Player state machine
  (+78) — COMPLETE".
- **Bomb capacity as a derived live-bomb count**, including the chain
  ownership-transfer slot move — lives in `flames.cpp` (own audit item),
  but `bombs.cpp`'s `place`/`try_grab`/`throw_carried` bookkeeping around
  `Player::bombs_placed`/`trigger_placed` is consistent with it. See
  `facts.md` "Bomb capacity is a derived live-bomb count."
- **Fuse pause while flying, and never-ticks for trigger kind** —
  `tick_fuses`'s `!b.flying` gate and the `fuse = -1` trigger sentinel
  reproduce `sub_42331C`'s `motion != 2 && motion != 3 && kind != 1` fuse-
  accrual gate (`bombs.cpp:554-567`) — modulo Finding 1's missing
  alive/side-count term.
- **Spooge run stop conditions and cascading fuse stagger** — stops at a
  live player, a powerup, a blocked tile, an existing bomb, or exhausted
  supply; the k-th bomb gets a `+k`-tick fuse stagger via
  `sub_422EDE`'s elapsed-init. Matches `spooge_ahead`
  (`bombs.cpp:89-106`); already covered by `tests/test_spooge.cpp` and
  cited in `facts.md` Core-feel audit item 3.
- **No bombs on a warphole tile** (drop refusal) — matches `drop()`
  (`bombs.cpp:73-87`); tested in `tests/test_stage_actors.cpp`.
- **Kicked-bomb speed source is VALUELST id 300, punch/throw is id 301,
  conveyor is id 190+idx** — the *sources* are correct (only the +100
  ground-speed term, Finding 2, and case-0-vs-case-1 continuity, Finding 3,
  are missing).

## Not re-derived (out of this system's scope, flagged for the owning audit)

- The chain-detonation queue drain (`sub_423209`/top-of-tick
  `dword_462210 != dword_464994` reset) and the flame-arm spread loop
  (`k<4` direction fan, kick-safe-dir `+56` skip) live mostly in
  `flames.cpp` — this pass only touched the parts of that machinery visible
  from `bombs.cpp` (`queue_chain` call sites, `stop_pending`).
- Case 3 of the bomb-mover switch (`*(WORD*)(v75+46)==3`, "punch-chain
  link," `sub_424AF4`/`sub_42325D`, the `+148` mutual-link pointer and the
  effect-pool records `sub_422991`/`sub_422A9F`) appears to be network-sync
  bookkeeping (remote-client bomb reconciliation), not local gameplay
  physics — `sub_422EDE`'s `a10` "punch-chain" parameter is always passed
  `0` from every gameplay call site found in this pass. Not chased further;
  flag for a networking-focused pass if the port ever adds remote play.
- The off-tile "bump count" (`+72`) increment condition (pseudo.c
  ~25577-25582) mixes what read as tile-column integers against
  pixel-scale fields (`+20`/`+24`) with no `HEXRAYS-FIX` annotation
  resolving the apparent type mismatch — left unflagged as a finding here
  (too speculative without a fresh disasm pass); the port's
  tiles-then-hops abstraction (launch 3 tiles, then 1-tile hops) already
  matches the documented behavioural intent ("+72 >= 3 tiles"), so there is
  no evidence of a *visible* divergence, just an unresolved decompiler
  ambiguity worth a follow-up disasm check.
- The kicked-bomb sprite wobble render offset (`sin_`/`getvalue(660/661)`,
  case 2's draw-only tail) is a presentation concern belonging to
  `renderer.cpp` (ledger item 9), not `libs/sim`.
