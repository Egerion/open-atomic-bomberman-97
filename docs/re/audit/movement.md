# Fidelity audit — `movement.cpp` (system 1, W1)

**Verdict: clean — 1 finding, low severity, currently dormant.** This system
has already been through two dedicated line-by-line passes before this audit
(the 2026-07-10 "Core-feel audit" and the 2026-07-16 movement/budget-order
note baked into `movement.cpp`'s own header comment), and both are visible,
cited, and cross-checked in the code. Re-deriving the arithmetic from the
native transliteration independently here reproduced the same conclusions:
budget accrual order, disease/skate/clog factoring, corner/glide/settle
resolution, the conveyor add/subtract, the opposite-key filter, the
reversed-controls application point, and the kick-probe handoff all match
the original exactly. The one new item found — `MovementSystem::ice_delay`
approximating each ring-buffer slot's age from a constant-cadence formula
instead of accumulating the real (non-uniform) per-sub-frame deltas — is a
genuine structural divergence from the original's algorithm, but it is
provably exact for every `ice_delay_ms` value currently shipped (the only
non-zero one, Hockey Rink's 250 ms, is an exact multiple of the 50 ms tick)
and only bites a hypothetical custom VALUELST scheme with a non-tick-aligned
ice-delay value.

Scope: `libs/sim/src/systems/movement.{hpp,cpp}` and the movement-relevant
slice of `libs/sim/src/simulation.cpp`'s `player_turn` (sub-frame loop,
input decode, ice, kick probe). Bomb-mechanics internals reached only
through the kick-probe *handoff* (`BombSystem::try_kick`) were skimmed for
consistency, not audited — that is system 2's scope.

References read in full: `native/src/game/batch_0x41F29B.cpp` (`sub_41F29B`,
the per-player per-frame update — budget-accrual math and movement dispatch
at lines 778-823, ice buffer at 412-436), `native/src/game/batch_0x41DAA7.cpp`
(`sub_41EC84`, the per-pixel mover, lines 770-982; `sub_41E61E`, the human
input decoder, lines 523-720), `docs/re/facts.md` ("Player movement —
CONFIRMED", "Ice / input-lag", "Canonical frame cadence", the 2026-07-10
"Core-feel audit" entry in full, the "Player state machine (+78)" table),
`docs/re/fidelity-audit.md`, `libs/sim/src/systems/movement.cpp`,
`libs/sim/src/systems/movement.hpp`, `libs/sim/src/systems/stage_actors.cpp`
(conveyor budget wiring), `libs/sim/src/simulation.cpp` lines 141-540,
`libs/sim/include/bomber/sim/player.hpp`, `libs/sim/include/bomber/sim/
constants.hpp`, `libs/sim/include/bomber/sim/tuning.hpp`, `tests/test_ice.cpp`.

---

## Finding 1 — `ice_delay`'s slot-age is a constant-cadence formula, not an accumulated real delta (dormant with shipped tuning)

**Original** (`sub_41F29B` ~23058-23078, `native/src/game/batch_0x41F29B.cpp`
lines 412-436): every call (once per displayed frame, humans only) first
ages **every** existing slot by the real measured frame delta, *then* shifts
the buffer down and inserts the fresh sample at slot 0.

The buffer is 30 slots of two fields each, stored interleaved — slot `k`'s
**age** at word index `2k` and its **direction** at word index `2k+1`. The
four passes run in this order, each over the whole buffer:

| # | pass | detail |
|---|---|---|
| 1 | **age** | for `k` = 0..29: slot `k`'s age += `dword_464958` — *age ALL slots by this frame's real delta* |
| 2 | **shift** | for `k` = 29 down to 1: slot `k` ← slot `k-1`, **both** fields (age and direction) |
| 3 | **insert** | slot 0's age ← 0, slot 0's direction ← the freshly resolved want-godir — *fresh sample, age 0* |
| 4 | **resolve** | for `k` = 0..29 ascending: write slot `k`'s direction into the player's requested-direction word at `+46`, then **break** as soon as slot `k`'s age is >= the level's ice delay `getvalue(dword_46499C + 450)` — *first slot old enough wins* |

Note pass 4's write happens **before** its break test, so the last slot
examined is the one that survives into `+46`.

Because the age-then-shift order applies each call's *own* delta only to
what is *already* in the buffer (the fresh insert always starts at age 0),
slot `k`'s age after it has ridden the buffer for `k` calls is the **sum of
the `k` most recent per-call deltas**, not `k` times an average. With the
canonical sub-frame pattern (`constants.hpp` `kSubFrameMs = {6,5,6,5,6,5,6,5,6}`,
summing to `kMsPerTick = 50` over `kSubFrames = 9`), any window of 9
*consecutive* deltas sums to exactly 50 (a periodic sequence: any full-period
window sums to the period total) — so slot ages are exact at tick boundaries
(`k` a multiple of 9) **regardless of phase**, but for `k` not a multiple of
9 the real cumulative sum depends on which specific 6s and 5s fall in the
window and can differ from a linear `k * 50/9` estimate by up to half a
sub-frame.

**Port** (`libs/sim/src/systems/movement.cpp` `MovementSystem::ice_delay`,
lines 151-181): the ring buffer stores only *directions* (`Player::ice_history`,
`int8_t[30]`), never per-slot ages, and the resolving index is computed once
from a closed-form estimate instead of real accumulation:

```cpp
for (int k = Player::kIceHistoryLen - 1; k > 0; --k) p.ice_history[k] = p.ice_history[k - 1];
p.ice_history[0] = static_cast<std::int8_t>(want_godir);

int k = (delay_ms * kSubFrames + kMsPerTick - 1) / kMsPerTick;  // ceil(delay_ms / (50/9))
if (k >= Player::kIceHistoryLen) k = Player::kIceHistoryLen - 1;
return p.ice_history[k];
```

This formula is exact whenever `delay_ms` is a multiple of `kMsPerTick` (50) —
which is exactly the case for the only tuning value the game ever ships
(`Tuning::ice_delay_ms[2] = 250`, Hockey Rink; every other level is 0 and
short-circuits before touching the buffer at all, `movement.cpp` line 159).
For a `delay_ms` that is *not* a multiple of 50 (only reachable by a custom
scheme writing VALUELST ids 450-460 to an off-tick value, e.g. 217 ms), the
formula's `k` can be off by one slot from what real per-slot age accumulation
would select — e.g. `delay_ms = 6`: the formula gives `k = ceil(6·9/50) = 2`,
but the real cumulative age after 1 push is already 6 (the first sub-frame
delta), so exact tracking would settle on `k = 1` — a one-slot (~5-6 ms)
difference in which buffered sample is surfaced.

**Visible effect**: none today — the shipped default (250 ms) round-trips
exactly, and `tests/test_ice.cpp` only exercises that value (confirmed by
inspection: `hockey_config()` hardcodes `level_index = 2`, no other
`ice_delay_ms` is ever set in any test or the default table). A custom
scheme setting a non-tick-aligned ice delay would see the input-lag onset
shift by up to one sub-frame (~5-6 ms) earlier or later than the original,
and in the boundary case above could occasionally surface a *stale-by-one*
buffered direction instead of the intended one for a single frame — a subtle
timing nuance on Hockey Rink specifically, imperceptible in practice but not
byte-faithful.

**Severity**: low (dormant with every currently-shipped tuning value; no
existing scenario or golden test reaches a non-tick-aligned `ice_delay_ms`).

**Confidence**: high (the divergence is derived algebraically from the
periodic-sequence identity above, not from a guess).

**Suggested fix**: if byte-fidelity for custom/off-tick ice-delay values is
ever wanted, replace the direction-only ring buffer with a parallel
`std::array<std::int32_t, 30> ice_age` and port the original's age-then-shift
loop verbatim (age every slot by `delta_ms`, shift, insert fresh at age 0,
walk from slot 0 for the first `age[k] >= delay_ms`) instead of the
closed-form index. This is a `Player`-state-shape change (new hashed field)
so it would need a golden recapture even though it is a no-op for every
current scenario; likely not worth the churn unless a scheme is found in the
wild that actually sets an off-tick ice delay.

---

## Verified faithful (no change)

- **Budget accrual order and magnitude** (`movement.cpp` lines 63-75 vs
  `batch_0x41F29B.cpp` lines 797-820): `base(42) + skates·getvalue(90) −
  clogs·getvalue(91)` baked into `Player::speed` at pickup/spawn time
  (`setup.cpp`, `powerups.cpp`) reproduces the original's per-tick recompute
  exactly (VALUELST values are match-constant, so baking in at mutation time
  is outcome-identical to the original's live re-read); disease factors
  (molasses `÷3`, hyper/super `×3/2`) applied to that base *before*
  delta-scaling, matching the original's own three successive rewrites of
  its speed local — first `speed ÷= 3`, then `speed = 3·speed / 2`, then
  `speed = delta · speed / 50` —
  in that exact order. Already the subject of a dedicated 2026-07-16 fix
  (see `movement.cpp`'s own header comment) — re-verified here against the
  batch transliteration and found correct.
- **Conveyor budget term** (`stage_actors.cpp` `move_on_actor`): case (a)
  (idle, belt forces movement, `use_player_speed=false`, budget = exactly
  `getvalue(190+idx)`, delta-scaled, no disease factor) and case (b) (player
  input present, belt add/subtract via `want_godir == belt_dir` /
  `want_godir == (belt_dir+2)&3`) both match `batch_0x41F29B.cpp` lines
  779-823 term-for-term, including the belt term being *separately*
  delta-scaled and applied *after* the disease-scaled speed term is already
  computed (not folded into the same division). The add/subtract direction
  test (the belt actor's own direction word at `+44` compared against the
  player's requested-direction word at `+46`, vs
  `belt_dir == (player_dir+2)&3`) is
  algebraically the involution-equivalent of the port's `want_godir ==
  belt_dir` / `want_godir == (belt_dir+2)&3` — verified by hand (both reduce
  to the same equality mod 4).
- **Per-pixel corner/glide/settle resolution** (`movement.cpp` lines 84-118
  vs `sub_41EC84` lines 814-951): `along`/`perp` computation matches
  (the original's own along/perp offset locals, confirmed via the `DX/DY`
  table dot-products, commutative
  with the port's `sx·dxg+sy·dyg` / `sy·dxg−sx·dyg`); the "no distance
  threshold, only which side of centre" advance/settle logic matches
  (`along<0 || passable(ahead)` gates the advance branch unconditionally,
  exactly as the port's `along < 0 || passable(...)`); the settle-back
  distance (`along * DX[(dir+2)&3]`) matches exactly. The batch
  transliteration's corner-round branch (the perpendicular-offset positive
  and negative cases) contains an
  apparent dead recheck of the already-known-blocked straight-ahead tile
  (traced by hand: the candidate tile coords that branch recomputes use the identical
  `dir`-indexed formula as the already-failed top-of-loop check, with no
  intervening state change) — this is very likely an unresolved
  register-misattribution artifact of the *batch transliteration specifically*
  (the batch's own header documents this exact class of bug for other
  variables in the sibling function, `sub_41F29B`), not a genuine original
  behaviour; `docs/re/facts.md`'s 2026-07-10 Core-feel audit (which had the
  actual Hex-Rays pseudocode, not just the batch guess) already lists "the
  per-pixel mover's corner/glide/settle resolution and its `(dir±1)&3`
  rotations" as **verified-identical, no change** — the port's L-shaped
  perpendicular-then-diagonal check (`movement.cpp` lines 98-112) is the
  correct reading and matches that prior verification. Not re-litigated as a
  new finding.
- **Step-on centring trigger** (`movement.cpp`'s `on_center`, lines 123-140):
  the original's pre-move predictive check ("the along-axis offset is
  exactly −1") and the port's
  post-move "landed exactly on axis-centre" check are provably the same
  event — traced by hand that `along == -1` unconditionally triggers a
  forward step this same iteration (via the `along < 0` branch), landing
  exactly on `along == 0`, and that a simultaneous perpendicular glide step
  cannot perturb the along-axis position (its direction is orthogonal to the
  travel axis by construction) — so checking "post-step, this axis is
  exactly centred" catches precisely the same tile-arrival events the
  original's pre-step predictive check does. Already the subject of an
  extensive doc comment in `movement.cpp` (lines 9-23) — independently
  re-derived here, not just trusted.
- **Kick probe handoff** (`simulation.cpp` lines 495-519 vs `sub_41EC84`'s
  along-offset-is-zero branch, lines 860-881): the original checks the kick condition
  *inside* the per-pixel loop (fires on every remaining budget iteration
  once parked at a blocked bomb's near-centre); the port checks once,
  post-move, using the player's final tick-end position. Proved these are
  outcome-equivalent by construction: a bomb tile is never actually
  passable (`grid::tile_open`/`bomb_at` exclude it), so the mover cannot
  walk *through* a bomb within a tick — any tick that would trigger an
  intermediate-tile kick necessarily also blocks and settles the player
  there, making that tile the tick's final position too; and repeated
  same-direction re-kicks are a documented no-op in `try_kick` (bombs.hpp/
  `try_kick`'s "b->dir == d: return" branch). Matches `docs/re/facts.md`'s
  Core-feel audit finding 1, already cited in the port's own comment.
- **`try_kick`'s "tile beyond the bomb must be passable" gate** (`bombs.cpp`
  lines 220-228): present and correctly separate from the probe's own
  centring test, matching `sub_41EC84`'s passability check on the tile one
  further `dword_45BECC`/`dword_45BEDC` step beyond the bomb's tile (via
  `sub_41E5C3`) before dispatching `sub_424708`.
- **Opposite-key resolution + last-index-wins bias** (`simulation.cpp` lines
  415-442 vs `sub_41E61E`'s opposite-key resolution tail,
  `batch_0x41DAA7.cpp` lines 666-711):
  count-pressed → conditional passability filter → last-surviving-index wins,
  matches exactly, including the GODIR ordering (0=Up,1=Right,2=Down,3=Left)
  that produces the documented "Left beats Right, Down beats Up" bias.
  Already pinned by golden tests per the 2026-07-10 audit; independently
  re-traced here against the batch source, not just trusted.
- **Reversed-controls (disease) application point** (`simulation.cpp` lines
  444-453 vs `sub_41F29B` line ~404): flip applied to the *resolved* godir,
  after the opposite-key filter, before the ice-buffer push, gated on
  `!p.ai` (`+16 != 1` in the original) — matches exactly, including AIs being
  exempt. Already documented as Core-feel audit finding 6.
- **Ice-buffer gating and AI exemption** (`movement.cpp` `ice_delay` lines
  151-159 vs `sub_41F29B` lines 412-436): AI players (`p.ai`/`+16==1`) never
  touch the buffer at all, matching the original's `!= 1` gate wrapping the
  *entire* age/shift/insert/resolve block, not just the resolve step. The
  port's "return early, buffer untouched, when off Hockey Rink" optimisation
  is outcome-identical to the original's "buffer keeps aging uselessly but
  resolution always picks slot 0 (age 0 always `<= 0`) since `delay_ms<=0`"
  — verified by hand, not just asserted; see also Finding 1 for the one
  place this system's buffer model is *not* bit-exact.
- **Movement budget loop mechanics**: original's `for(...; budget>0; budget-=100)`
  (decrement in the loop's increment-clause, after the body) vs the port's
  `while (budget>0) { budget -= 100; ...body...}` (decrement before the
  body) are semantically identical iteration sequences — verified by hand
  that no part of either body reads the budget variable's value, so the
  textual position of the decrement relative to the body is unobservable;
  both run exactly `ceil(budget/100)` iterations for the same starting
  value. Negative/fractional carry-over across ticks (no clamping to 0 in
  either) also matches.
- **Stun/pickup-pause vs the mover**: confirmed (independently re-derived
  by tracing how far `sub_41F29B`'s stun/pause gate flag actually reaches)
  that neither counter gates the
  per-pixel mover or the ice buffer — only new-input *acquisition*. The
  port's `sub_stunned`/`paused`/`frozen` flags in `simulation.cpp` gate
  exactly that slot (lines 379-403) and nothing else in the sub-frame loop,
  matching the extensive doc comment already in place (lines 160-230) and
  the "Player state machine (+78) — COMPLETE" facts.md table.
- **Frame-budget integer truncation** (`constants.hpp` `frame_budget`):
  `speed * delta_ms / kMsPerTick`, truncate-toward-zero, matches the
  original's "frame delta × the disease-scaled speed local, divided by
  `dword_46494C`, truncated back to `int`" — where the divisor is read as
  `unsigned` and the product is widened to 64-bit first. The
  `unsigned` divisor read doesn't change truncation behaviour here since
  the divisor (`dword_46494C` = 50) is always positive, so the usual
  arithmetic conversions produce an ordinary positive-divisor truncating
  division identical to the port's. No overflow risk at realistic speed
  values either side.
- **Deliberately-not-changed deviations** (already documented, not
  re-flagged): the original writes the glide's diagonal direction into the
  facing word mid-loop, sub-tick and cosmetic-adjacent (`docs/re/facts.md`
  Core-feel audit, "Deviations found but deliberately NOT changed"); the
  direction-change-cancels-kick/punch-anim bookkeeping (the original's
  "facing word at `+44` differs from requested-direction word at `+46`"
  test) is presentation/animation-state territory living in bombs.cpp's
  scope (system 2), not re-audited here.
