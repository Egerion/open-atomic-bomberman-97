# Fidelity audit — stage actors (conveyor / trampoline / warphole / dirarrow)

**Verdict: mostly faithful; one confirmed behavioural bug (conveyor idle-push
facing) plus one lower-confidence edge case worth a follow-up read.**

Scope: `libs/sim/src/systems/stage_actors.cpp`/`.hpp`, the actor-interaction
hooks in `libs/sim/src/systems/movement.cpp` and `libs/sim/src/systems/
bombs.cpp`, cross-read against `native/src/game/batch_0x41F29B.cpp`
(`sub_41F29B`), `native/src/game/batch_0x41DAA7.cpp` (`sub_41EC84`),
`native/src/game/batch_0x404852.cpp` (`sub_404E99`/`sub_405654`/`sub_4056CA`/
`sub_405A81`), and `native/src/game/batch_0x422DDD.cpp` (`sub_42331C`,
`sub_4230A5`, `sub_423188`), plus `docs/re/stage-actors.md`.

Method: read every arithmetic line of the four actor mechanics in the native
transliteration, matched byte offset by byte offset against the port. 1
confirmed finding, 1 flagged-for-follow-up. Everything else checked is listed
in "Verified faithful" below with the specific evidence.

## Findings

### 1. Conveyor idle-push: facing/pose reverts only when blocked, not always

**Original.** `sub_41F29B`, the idle-player (`godir == -1`) conveyor branch
(`native/src/game/batch_0x41F29B.cpp:779-796`, pseudo.c ≈23417-23427):

The branch runs, in this exact order (player record addressed by byte
offset, actor record likewise):

1. Look up the actor at the player's own tile: `sub_405654(tileX, tileY)`.
2. If there is **no** actor, or its type field (`+4`) is not 2 (conveyor),
   jump straight to the shared pose/draw tail (`LABEL_155`) — the conveyor
   branch has no effect at all.
3. The player's requested-direction word at `+46` ← the belt's direction
   word at the actor's `+44`.
4. `saved_facing` ← the player's facing word at `+44`. This is read here,
   BEFORE anything in this branch touches it, so it still holds the entry
   value.
5. The player's move-budget dword at `+116` += the belt budget — the
   conveyor speed **only**; no player-speed term is added on this path.
6. `sub_41EC84(player)` steps the player. **If it returns 0** (the push was
   not fatal), the requested-direction word at `+46` and the facing word at
   `+44` are BOTH written back to `saved_facing`, and the branch jumps to
   the shared tail (`LABEL_155`).

`sub_41EC84` (`native/src/game/batch_0x41DAA7.cpp:771-982`) returns `1`
*only* at line 962, when a flame kills the player mid-step
(`sub_41DE63(...)` truthy inside the per-pixel loop); every other path —
budget fully spent, whether the player actually shifted or was fully blocked
— falls through to the unconditional zero return at line 981. So
"`sub_41EC84` returned 0" is **true on essentially every tick** (false only
on the rare tick a flamed conveyor kills the player mid-push), meaning **the
revert of both the requested-direction word at `+46` and the facing word at
`+44` to the saved facing
fires unconditionally**, not "if fully blocked" as `docs/re/stage-actors.md`
§3 paraphrases it (`docs/re/stage-actors.md:216-221`, "revert if fully
blocked" — this doc line is itself a mild mischaracterisation worth fixing
alongside the code).

The saved facing is captured from `+44` *before* anything touches it this
tick — since the input-decode sync (`+44` ← `+46`,
`batch_0x41F29B.cpp:410`) only runs when there *was* input this tick
(`+46 != -1`), and this whole branch is gated on `+46 == -1` (no input), the
saved value is always whatever
facing the player last held from their most recent tick *with* real input —
i.e. **facing freezes at the last actively-chosen direction the instant a
player stops steering, and stays frozen through any number of idle
conveyor-push ticks**, even though the belt keeps sliding them across tiles.
Inside `sub_41EC84` itself, the per-pixel loop's own sync (the word at `+44`
← the word at `+46`, `batch_0x41DAA7.cpp:820`) makes facing track the belt
direction *transiently*, for the duration of that one call only — the
caller's revert erases it again before anything else reads it.

This is not purely cosmetic: `Player::facing` also drives punch/kick
direction (`libs/sim/src/systems/bombs.cpp:91,131,133,188`), and it picks the
stand-vs-walk sprite text at `LABEL_155` (`batch_0x41F29B.cpp:441`, which
reads the *same* `+46` field the revert just restored to a non﹣`-1`
value, so a pushed-idle player renders in a **walking** pose facing the old
direction, not the belt direction, and not idle either).

**Port.** `StageActorSystem::move_on_actor`, the `else if (conveyor)` branch
(`libs/sim/src/systems/stage_actors.cpp:155-169`):

```cpp
const Fixed fx = p.x, fy = p.y;
const Direction saved_facing = p.facing;
movement_.move(p, grid::from_godir(belt_dir), belt, &on_step_center, &sctx,
               /*use_player_speed=*/false, on_pixel, pixel_ctx, delta_ms);
if (p.x == fx && p.y == fy)
    p.facing = saved_facing;  // blocked: revert the forced facing
```

`MovementSystem::move` unconditionally sets `p.facing = d` at entry
(`libs/sim/src/systems/movement.cpp:28`), and the port only reverts it when
the push produced **zero net displacement** (the blocked case). Whenever the
belt actually pushes the player (the common case — an unobstructed lane),
the port leaves `p.facing == belt_dir`, which the original never does: the
original reverts facing to the pre-push value on that exact tick regardless
of whether the player moved.

**Visible effect.** A player standing idle on an unobstructed conveyor: the
port shows them facing/animating in the belt's direction while sliding; the
original keeps them facing (and posed walking in) whatever direction they
last actively chose, frozen for as long as they're parked on the belt. If
such a player punches or kicks a bomb the same tick (no directional input,
action key only), the port sends it in the belt direction; the original
sends it in the player's frozen pre-push direction.

**Severity:** Medium — very common trigger condition (any idle player parked
on any conveyor, every unblocked tick), but the gameplay-affecting half
(punch/kick direction) requires an action press with no directional input
the same tick, which is a narrower subset of that common case. The facing/
pose mismatch itself is presentation but is highly visible whenever
conveyors are in play.

**Confidence:** High — traced byte-for-byte through the transliteration with
cross-checked `HEXRAYS-FIX` register-recovery notes, and `sub_41EC84`'s two
return paths (`0` normal, `1` flame-death) are unambiguous in the disassembly-
derived source.

**Suggested fix.** Make the revert unconditional (drop the position-equality
gate), matching the original's "always, unless the push just killed the
player" behaviour — which is moot to special-case since `simulation.cpp`
already checks `p.alive` and returns immediately after `move_on_actor`
(`libs/sim/src/simulation.cpp:490-494`) before facing is read again:

```cpp
} else if (conveyor) {
    const Direction saved_facing = p.facing;
    movement_.move(p, grid::from_godir(belt_dir), belt, &on_step_center, &sctx,
                   /*use_player_speed=*/false, on_pixel, pixel_ctx, delta_ms);
    p.facing = saved_facing;  // sub_41F29B ~23427: the +44/+46 direction words
                              // always revert to the pre-push facing after a
                              // non-fatal push, not only when blocked.
}
```
Also correct `docs/re/stage-actors.md`'s §3 case-(a) snippet ("revert if
fully blocked" → "revert unconditionally after a non-fatal push"). A golden
recapture is needed if any golden scenario places a conveyor under an idle
player (worth double-checking; §8's determinism note says none currently do,
in which case this is presentation-only for existing goldens and only
affects hash-sensitive scenarios once a conveyor test is added).

### 2. Chained warphole/trampoline landing — untriggered in the original, port may over-trigger (flag only, not confirmed)

While tracing finding 1 I noticed a structural asymmetry that's worth a
follow-up but I could not fully confirm as a bug within this audit's budget.

**Original.** For an idle (no-input) player **not** standing on a conveyor,
`sub_41F29B`'s case-(a) branch jumps to the shared tail (`LABEL_155`)
*before* ever calling
`sub_41EC84` (`batch_0x41F29B.cpp:784-785`) — i.e. the per-pixel stepper,
which is the *only* place the warphole/trampoline step-on check lives (the
"this pixel step lands the player exactly on a tile centre" test,
`batch_0x41DAA7.cpp:831-858`), is **never invoked** for such a
player. So a player who is *relocated* onto a fresh tile by something other
than a per-pixel walk step — the warp midpoint teleport (`+28/+32 =
+20/+24`, `batch_0x41F29B.cpp:574-575`) or the trampoline apex relocation
(`batch_0x41F29B.cpp:539-540`) — gets no automatic re-check of the landing
tile. If that landing tile is itself a warphole or trampoline and the player
then sits idle off any conveyor, nothing fires until the player actually
walks off and back onto it (or the tile also happens to be a conveyor, which
re-enters `sub_41EC84` — I did not trace that sub-case's centre-landing test
on the very first iteration to confirm whether it could immediately re-fire).

**Port.** `trampoline_after_move`/`warphole_after_move`
(`libs/sim/src/systems/stage_actors.cpp:173-226`) run once per tick,
unconditionally, checking only "is the player centred on the tile, and not
already latched." A player placed exactly on a tile centre by `tick_warp`'s
midpoint relocation or `tick_bounce`'s apex relocation satisfies that check
immediately (their latch is for the *previous* actor, not the new one), so
if the destination tile is itself a trampoline/warphole, the port's safety
net looks like it would start a *second* warp/bounce the very next tick —
something the original, per the read above, does not do for a genuinely idle
player.

**Why not promoted to a full finding:** I could not confirm within budget
whether `sub_41EC84`'s centre-landing test could still fire (or some
equivalent immediate-refire condition) on the very first
per-pixel iteration after a conveyor-forced idle push starting exactly at a
tile centre, which would partially close this gap for the conveyor sub-case.
This also requires a level with two chained actors (a warphole/trampoline
whose exit/apax lands on another warphole/trampoline) to ever manifest —
none of the current test fixtures place actors this way (§8 of
`stage-actors.md`).

**Severity:** Low (narrow level-design precondition). **Confidence:**
Low-Medium (the asymmetry is real; the practical consequence needs a
dedicated trace or a repro level to confirm). **Suggested fix:** none yet —
flag for a follow-up read of `sub_41EC84`'s first-iteration centre-landing
test when called with the player already centred, or build a repro level with two
chained warpholes and compare against the real binary via the oracle.

## Verified faithful (no change)

- **Conveyor "moving" case (b) bonus/penalty** — `sub_41F29B:797-822` vs
  `stage_actors.cpp:142-154`: both the "with belt" bonus (`actor.dir ==
  requested_dir`) and "against belt" penalty (`actor.dir == (requested_dir+2)
  &3`) read the player's *input* direction (the `+46` word / `want_godir`), never
  the rendered facing, and use the actor at the player's tile *before* this
  tick's move — matches exactly. The doc's note that `+42`-hi and `+44` are
  the same underlying word (not two fields) is confirmed by the byte math:
  reading the actor's dword at `+42` and shifting right 16 lands on exactly
  the two bytes the 16-bit field at `+44` occupies.
- **Conveyor speed source & clamp** — VALUELST 189 (count=3) / 190-192
  (250/350/450), `dword_464930` selector clamped `[0, getvalue(189)-1]`
  (pseudo.c 7862-7865) vs `Tuning::conveyor_speed()`
  (`libs/sim/include/bomber/sim/tuning.hpp:165-173`): same clamp shape, same
  table, same default index (1).
- **Warp resolver `sub_405A81`** (`batch_0x404852.cpp:897-944`) — scans for
  another warphole (`type==1`, `slot != self`) whose `idno` (+44) equals this
  warphole's `linkto` (+46); no match ⇒ dest = own tile; zero RNG draws.
  Matches `docs/re/stage-actors.md` §5 exactly; the port pre-resolves this at
  setup into `State::warp_dest_*` (config-time, not per-tick), consistent
  with the "no RNG on the warp path" contract.
- **Warp two-phase timing** — `sub_41F29B` states 6/7, `+40 > 8` (9 ticks)
  each phase (`batch_0x41F29B.cpp:568-591`) vs `StageActorSystem::kWarpTicks
  = 18`/`kWarpMid = 9` (`stage_actors.hpp:31-32`, `tick_warp` at
  `stage_actors.cpp:193-206`): traced tick-by-tick — the port's down-counter
  relocates on the 9th processed tick, matching the original's relocate-on-
  the-9th-increment; total gated duration 18 either way.
- **Trampoline apex relocation** — `sub_41F29B` state 5
  (`batch_0x41F29B.cpp:519-545`) vs `tick_bounce`
  (`stage_actors.cpp:36-71`): apex fires once at `c == 15` (both
  implementations), the 100-attempt loop draws `rand()%5`/`random_below(s,5)`
  for X *then* Y unconditionally every attempt, requires both axes to differ,
  and rejects solid (`sub_425FB9`/`tramp_solid` via `grid::tile_open`) or
  bomb-occupied (`sub_422E48`/`tramp_bomb` via `grid::bomb_at`) candidates —
  order, count, and rejection tests all match.
- **Trampoline bounce duration** — VALUELST 680 = 30, apex at 680/2 = 15,
  confirmed against `tuning.trampoline_bounce_frames` and the down-counter
  math in `tick_bounce`.
- **Dirarrow is bomb-only** — confirmed by absence: no `sub_405654` call
  gated on type 0 anywhere in `sub_41F29B`/`sub_41EC84` (only conveyor and
  warphole lookups exist in the player paths); the bomb slide loop's
  tile-centre dirarrow re-steer (`batch_0x422DDD.cpp:703-711`) is the only
  place type 0 is consulted. Matches `bombs.cpp:422-433`.
- **Bomb-on-trampoline never happens** — `grep` over every native batch file
  confirms exactly one call site of `sub_427961(350)`
  (`batch_0x41DAA7.cpp:856`, inside the player stepper's "actor type field
  `+4` == 3" branch)
  and no actor-type-3 test anywhere in `sub_42331C`. Nothing to port; the
  port has no bomb-trampoline interaction either.
- **Bomb-on-warphole blocked like a wall** — `sub_4230A5`
  (`batch_0x422DDD.cpp:264-292`) and `sub_423188`
  (`batch_0x422DDD.cpp:298-306`) both end on the same composite verdict —
  *(no actor on the probed tile, OR an actor whose type field `+4` is not 1)
  AND `sub_425FB9(x, y) == 0`* — i.e. any type-1 actor makes the probed cell
  impassable to a bomb regardless of the underlying tile. Matches
  `bombs.cpp:334-335,474-481` (settle probe) and `:480-481` (slide
  cell-entry probe); `sub_405A81` (the warp resolver) has exactly one caller
  in the whole binary — the player stepper — confirming bombs never warp.
  Warp resolver call-site count re-verified directly via grep across all of
  `native/src/game/*.cpp` for this audit (single hit).
- **Sound mapping** — `sub_427961(350)`/`sub_427961(1330)`, single-id calls,
  vs `Event::Type::TrampolineBounce`/`WarpUsed` → SOUNDLST 350/1330; no
  conveyor/dirarrow sound in either version.
- **Warphole one-shot knockout** (`sub_4056CA` case 1's "the `+146` latch is
  still clear" block, `batch_0x404852.cpp:795-826`) — clears the warphole's
  own tile then one random in-bounds cardinal neighbour via
  `dword_45BECC`/`45BEDC`; confirmed setup-time-only (uses the same
  setup-only LCG as `-T,H` trampoline placement per `docs/re/
  stage-actors.md` §8), not `State::rng`, so no golden/hash impact — already
  marked `[VERIFIED 2026-07-04, DONE]` and re-confirmed here.

## Not re-litigated

Left untouched because `docs/re/stage-actors.md` already marks them
`[VERIFIED]`/`[CORRECTED]`/`[IMPLEMENTED]` with specific dated evidence and
my independent read of the same disassembly lines reached the same
conclusion: the coordinate-normalisation wraparound (§2), the `-T,H`
checkerboard-parity random placement (§2, setup-only RNG), the bomb-on-
conveyor `+100`/2-px nudge dance at `LABEL_21` (§6 item 1 — this is shared
machinery with the general kicked-bomb slide stepper, item #2 `bombs.cpp` in
the ledger, not conveyor-specific, so left for that audit), and the
conveyor/trampoline/dirarrow render-frame pacing in `sub_4056CA` (§3/§4
rendering, item #9 `renderer.cpp` in the ledger).
