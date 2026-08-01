# Fidelity audit — `renderer.cpp` (system 9, W2)

**Verdict: mostly faithful on draw ORDER/OFFSET/pacing (all previously-fixed
items re-verified correct), but the per-player POSE-SELECTION tail
(`sub_41F29B`'s `LABEL_239`/shared-tail region, pseudo.c ~23080-23411) has
four real, previously-undocumented deviations: player-vs-player draw order is
Y-sorted instead of fixed slot order; the idle "cornerhead" fidget re-rolls on
a wrong clock (a guessed tick-duration instead of the fidget's own animation
length); kick/punch pose duration is a guessed constant instead of the
sequence's own frame count; and the "picking up a bomb" pose's displayed
FRAME is walk-phase-driven in the original, not an elapsed-since-grab
counter. All four are presentation-only (no golden/hash impact). Highest
severity: the player draw-order finding (#1) — it changes which sprite wins
an overlap on every multiplayer round, not just a rare pose edge case.**

Scope: `libs/render/src/renderer.cpp` + `libs/render/include/bomber/render/renderer.hpp`.

## Fix status — 2026-07-20 (all four FIXED, presentation-only)

All four findings fixed in `renderer.cpp`/`.hpp` with citing comments; no
`libs/sim` change, no `state_hash()` impact.

- **F1** — removed the `std::sort`; `draw_world` iterates slots 0..9 in raw
  order (skip `!present || !alive`), matching `sub_420F07`.
- **F2** — `sample_movement` now (re-)rolls a cornerhead variant on entry and
  again once `panic_elapsed_` reaches that variant's own `steps.size()`
  (ANI-cycle completion), still gated on `boxed_in`; `draw_world` uses
  `panic_elapsed_` as the phase. `panic_ticks_`/`kPanicSpread` dropped;
  replaced by `panic_active_`/`panic_elapsed_`. One RNG draw per re-roll
  (variant only), off `panic_lcg_` — no `State::rng` touch.
- **F3** — `on_events` sets `kick_pose_`/`punch_pose_` from
  `seqs_->kick/punch[render_colour][facing].steps.size()`; `draw_world` phase
  is `a->steps.size() - kick_pose_[i]`. `kActionPoseTicks` removed.
- **F4** — CONFIRMED against the binary (`native/tools/disasm.py sub_41F29B`,
  0x420350-0x420379: the walk-phase word at `+48` is loaded, signed-divided
  by the literal 3, and the quotient handed straight to the ANI frame picker
  — so the walk-phase-over-3 frame really is recomputed unconditionally at
  the shared tail, NOT a Hex-Rays artifact). Pickup-pose frame changed to
  `moving_[i] ? walk_phase_[i]/3 : 0`; `pickup_pose_` kept as the exit timer.

**Verification.** Build green (windows-fetch, Release). An instrumented
(`BOMBER_FIXPROBE`) run over the full 200-tick scripted demo showed NONE of
the four paths ever fire (no player overlap, no kick/punch/grab event, no
boxed-in idle fidget) — so the fixes move no pinned visual-golden frame; the
`walking` shot (t10, two players) stays byte-identical, confirming the F1
loop restructure did not regress normal rendering (BMP eyeballed:
players/flames/bombs/powerups composite correctly). Visual goldens therefore
NOT recaptured — any drift observed was from the concurrent (uncommitted)
`libs/sim` fixes, which must own the eventual shots.txt recapture against the
final sim (see tests/visual/shots.txt note).

References read in full: `native/src/game/batch_0x41F29B.cpp` (`sub_41F29B`
draw tail, all 808 lines), `native/src/game/batch_0x420D4E.cpp` (`sub_420F07`
per-player draw loop, `sub_420D4E`/`sub_420E39` gold twinkle),
`native/src/game/batch_0x4293E5.cpp` (`sub_42A191` per-frame call sequence),
`native/src/game/batch_0x426C4C.cpp` (`sub_426D06` flame/brick-burn draw),
`native/src/game/batch_0x41DAA7.cpp` (`sub_41EC84` walker, `sub_41EB13`
sprite emission). Raw `pseudo.c` cross-read for the two pose-tail findings
(lines 23380-23412) to rule out a transliteration artifact. `docs/re/facts.md`
"Draw order" / "Draw order — tile layer addendum" / "Walk leg-cycle pacing" /
"Canonical frame cadence" / disease-flash entries; `docs/valuelst-map.md` id
330.

---

## Finding 1 — Player draw order is Y-sorted; the original draws in fixed slot order 0..9

**Original**: `sub_420F07` (`native/src/game/batch_0x420D4E.cpp` lines
147-224, pseudo.c 23628-23724 per `docs/re/facts.md` line 1895):

The whole player pass is one ascending loop, `i = 0..9`, whose body calls
`sub_41F29B` on player `i`'s record — the roster array at `dword_461BC4`,
stride 38 dwords (152 B) per player. Each of those calls draws that player's
shadow, body and carried bomb INLINE, before the loop advances to the next
slot. There is no second pass and no sorting step anywhere in the function.

`facts.md` line 1895-1897 already states this plainly: *"29527 `sub_420F07`
— the PLAYER PASS (23628-23724): slots 0..9 ascending, `sub_41F29B` per
player, each player's FULL turn completing before the next slot."* There is
no Y-comparison, no depth buffer, no re-ordering of any kind — slot 9 always
draws over slot 0 when they overlap, regardless of vertical position.

**Port**: `libs/render/src/renderer.cpp` lines 667-673 (`draw_world`):

```cpp
// Players, bottom-anchored, in Y order so lower players draw in front.
std::array<int, sim::kMaxPlayers> order{};
int n = 0;
for (int i = 0; i < sim::kMaxPlayers; ++i)
    if (s.players[i].present && s.players[i].alive) order[n++] = i;
std::sort(order.begin(), order.begin() + n,
          [&s](int a, int b) { return s.players[a].y < s.players[b].y; });
```

**Visible effect**: whenever two players' sprites overlap (adjacent tiles,
a kick/punch knockback graze, a trampoline landing, a warp-in on top of
someone), the port draws whichever player is physically higher up the screen
UNDER the one lower down (a pseudo-3D "closer wins" rule). The original
instead always draws the higher-numbered SLOT on top, independent of
position — e.g. player 1 is always visually in front of player 0 even when
player 0 is standing "in front of" (below) player 1 on screen. This is a
small but constant, every-round-visible difference, not an edge case.

**Severity**: Medium (silent, every match, but only visible on sprite
overlap — never breaks gameplay).
**Confidence**: High (direct citation of the actual draw-loop code, and
`facts.md` already independently states the slot-ascending order for an
unrelated reason).
**Suggested fix**: drop the `std::sort` in `draw_world`; iterate
`i = 0..kMaxPlayers-1` in raw slot order (same skip condition:
`present && alive`).

---

## Finding 2 — Idle "cornerhead" fidget re-rolls on a fabricated tick-duration; the original re-rolls when the chosen variant's own animation finishes one playthrough

**Original**: `sub_41F29B`, entry (`native/src/game/batch_0x41F29B.cpp`
lines 344-364, pseudo.c ~23006-23013) and exit (lines 592-612, pseudo.c
~23396-23411 region — the exit account below is taken from raw `pseudo.c`
23397-23411; the function is actually `sub_41DAA7`'s caller in the same
block, address range ~0x420AEA-0x420C2B, reached via the "action state
greater than 4" branch):

Entry — the variant is rolled ONCE, and only when the player is freshly
boxed in (the fidget word at `+78` still 0). The branch key is the count of
blocked neighbours, so "fewer than 4" means at least one side is open:

- **at least one open side** — if `+78` currently holds a value in 20..39
  (a fidget in progress), clear it to 0. Un-boxing cancels the fidget.
- **fully boxed AND `+78 == 0`** (not fidgeting yet) — take
  `getvalue(330)` (`sub_412135(330)`) floored at 1, call it `n`; set
  `+78 ← rand_() % n + 20`. That is a VARIANT SELECTOR in the range
  20..20+n-1, **not a duration**. Reset the elapsed-frame counter at `+80`
  to 0.

Exit — driven by the chosen variant's OWN sequence length, re-evaluated
every tick while the player stays boxed in:

1. `sub_4518D0` formats the sequence name `"cornerhead%u"` with the index
   `+78 − 20` (the variant index recovered from the selector).
2. `sub_41D957` resolves it to a handle.
3. `sub_41DAA7(handle, elapsed)` picks the frame from the ELAPSED counter at
   `+80`, NOT from a raw tick number.
4. The elapsed counter advances by the file's standard per-frame accrual:
   `+82 += dword_464958`, then while `+82` is positive it sheds
   `dword_46494C` per iteration and bumps `+80` once each time round.
5. `sub_41DA5C(handle)` yields THIS variant's own last-frame index
   (statecnt−1).
6. If elapsed >= that index, clear both `+78` and `+80` — the ANI has
   finished, so the fidget exits and, because the player is still boxed in,
   is re-rolled on the very next tick.

`getvalue(330)` (VALUELST id 330, confirmed = 13, "how many cornerhead
animations there are") is used **only** as the modulus for picking WHICH
variant (`rand() % n`) — it never bounds a tick duration. The value at
`+78` itself (20..32) is the variant selector, held fixed until the
variant's own `CORNERHEAD<n>.ANI` sequence completes one full playthrough
(`elapsed >= statecnt`), at which point it resets to 0 and — because the
player is still boxed in — gets immediately re-rolled to a (possibly
different) variant on the very next tick. The re-roll cadence is therefore
however long THAT PARTICULAR variant's art actually is, not a fixed spread.

**Port**: `libs/render/src/renderer.cpp` lines 371-378 (`sample_movement`) +
lines 760-764 (`draw_world`), and `kPanicSpread` (renderer.cpp line 37):

```cpp
if (panic) {
    if (--panic_ticks_[i] <= 0) {
        panic_variant_[i] = static_cast<int>(panic_roll() % kCornerheadVariants);
        panic_ticks_[i] = 20 + static_cast<int>(panic_roll() % kPanicSpread);  // kPanicSpread = getvalue(330) = 13
    }
} else {
    panic_ticks_[i] = 0;
}
...
if (panic_ticks_[i] > 0 && !q.cornerhead[body_colour][panic_variant_[i]].steps.empty()) {
    a = &q.cornerhead[body_colour][panic_variant_[i]];
    ph = static_cast<std::size_t>(s.tick);   // raw global tick, not elapsed-since-entry
}
```

`docs/valuelst-map.md` id 330's own description ("BOTH the number of
cornerhead sequences AND the idle fidget duration spread") repeats the same
misreading — id 330 is only ever used as the variant-count modulus in the
original; there is no separate "duration" concept at all.

**Visible effect**: two compounding differences. (1) The port re-rolls a new
random variant every `20 + rand()%13` ticks (1.0-1.6 s) uniformly, regardless
of how long that variant's art actually runs, so a short cornerhead ANI would
loop several times per "duration" in the original but the port always cuts
it off at the guessed spread; a long one would get cut off mid-playthrough in
the port but never in the original. (2) The port's `ph = s.tick` starts each
newly-chosen variant's playback at an arbitrary phase (whatever `s.tick %
statecnt` happens to be), instead of always starting a fresh playthrough at
frame 0 the way the original's `elapsed`-from-0 counter guarantees — so the
fidget can visibly "jump into the middle" of its own animation instead of
playing cleanly start-to-finish.

**Severity**: Low-Medium (cosmetic, RNG-only, no gameplay impact — but a
frequently-seen idle state in any stalled-out corner situation).
**Confidence**: High (direct pseudo.c/transliteration citation for both the
roll site and the exit condition; the file's own "elapsed >= statecnt" idiom
is identical to the already-correctly-ported kick/punch/pickup states).
**Suggested fix**: mirror the pickup-pose pattern already used correctly
elsewhere in this file (`pickup_pose_[ev.player] = seq.steps.size()`): when
(re-)entering the fidget, pick `panic_variant_` at random (`panic_roll() %
kCornerheadVariants`, unchanged) and set an elapsed-frame counter to 0; each
tick, advance it and compare against `q.cornerhead[body_colour][variant]
.steps.size()`; on reaching it, re-roll a fresh variant and reset elapsed to
0 (still gated on `boxed_in`, exactly as today). Use the elapsed counter
(not `s.tick`) as `ph`. `kPanicSpread`/its VALUELST-330-as-duration framing
can be dropped.

---

## Finding 3 — Kick/punch pose duration is a guessed constant; the original ties it to the sequence's own frame count (same pattern already used correctly for "pickup")

**Original**: `sub_41F29B` (`native/src/game/batch_0x41F29B.cpp` lines
469-504, pseudo.c ~23080-23110), KICK (state 1) and PUNCH (state 2) —
described here for KICK; PUNCH is identical in shape. In order:

1. `sub_4518D0` formats the `"kick <dir>"` sequence name and `sub_41D957`
   resolves it to a handle.
2. `sub_41DAA7(handle, elapsed)` picks the frame from the state's ELAPSED
   counter — the word at `+80` (the high half of the dword at `+78`).
3. The elapsed counter advances by the file's standard per-frame accrual:
   `+82 += dword_464958`, then while `+82` is positive it sheds
   `dword_46494C` per iteration and bumps `+80` once each time round.
4. `sub_41DA5C(handle)` yields KICK.ANI's OWN last-frame index. Once the
   elapsed count reaches or passes it, both `+78` (state) and `+80`
   (elapsed) are cleared — the pose exits when KICK.ANI's own length is
   exhausted.
5. The branch then jumps straight to the shared shadow+body draw site
   (`LABEL_239`), keeping its elapsed-based frame index.

This is the identical "elapsed vs. this sequence's own `sub_41DA5C`
statecnt" idiom used for pickup (state 4, already ported correctly) and
cornerhead (Finding 2) — there is no fixed tick budget anywhere in the
original; every one of these transient poses plays for exactly as long as
its own ANI has frames.

**Port**: `libs/render/src/renderer.cpp` line 17 and its own comment admits
the guess:

```cpp
// How long a kick/punch pose stays up before returning to walk/stand. The
// KICK/PUNCH sequences are short, so ~8 ticks (~0.4 s) reads cleanly.
constexpr int kActionPoseTicks = 8;
...
case sim::Event::Type::BombKicked:
    if (ev.player >= 0 && ev.player < sim::kMaxPlayers)
        kick_pose_[ev.player] = kActionPoseTicks;
    break;
```

Contrast with the pickup pose right next to it (`on_events`,
`Event::Type::BombGrabbed`), which correctly reads the real sequence length:

```cpp
const auto& seq = seqs_->pickup[render_colour(s, ev.player)][...];
pickup_pose_[ev.player] = static_cast<int>(seq.steps.size());
```

**Visible effect**: if the shipped `KICK.ANI`/`PUNBOMB*.ANI` sequences have
more or fewer than 8 frames, the pose either cuts off before the animation
finishes (frames beyond `kActionPoseTicks` never shown — the pose reverts to
walk/stand mid-swing) or holds an extra static frame after the animation has
already completed once. Either way it does not match the original's
"play exactly once, full length" behaviour.

**Severity**: Low-Medium (short, frequent animation; wrong-length looks
"off" but doesn't affect gameplay).
**Confidence**: High (the port's own comment already flags this as
unconfirmed; the original mechanism is identical to the neighbouring,
correctly-ported pickup case).
**Suggested fix**: same pattern as `BombGrabbed`: on `BombKicked`/
`BombPunched`, set `kick_pose_[ev.player]`/`punch_pose_[ev.player]` from
`seqs_->kick[render_colour(s, ev.player)][dir].steps.size()` /
`...punch[...].steps.size()` instead of the fixed `kActionPoseTicks`
(`dir` = the player's facing at the event tick). `kActionPoseTicks` can be
dropped once both call sites are converted.

---

## Finding 4 — The "picking up a bomb" pose's displayed frame is walk-phase-driven in the original, not elapsed-since-grab

**Original**: `pseudo.c` lines 23396-23411 (read directly off the raw
decompile in `D:\...\BOMBRMAN\pseudo.c`, not just the transliteration, to
rule out a transliteration slip). Described in order, with the player record
addressed by byte offset:

1. The facing byte (`+44`, i.e. byte 2 of the dword at `+42`) is mapped
   through `sub_413AED` to a direction suffix, and `sub_4518D0` formats the
   sequence name `"pickup <dir>"`.
2. `sub_41D957` resolves that name to a sequence handle.
3. **(A)** `sub_41DAA7(handle, elapsed)` computes a frame index from the
   pickup state's ELAPSED counter — the word at `+80`, i.e. the high half of
   the dword at `+78`. *This result is computed and then thrown away; see
   step 6.*
4. The elapsed counter advances by the file's standard per-frame accrual:
   the sub-tick accumulator at `+82` takes `+= dword_464958` (ms since the
   last frame), then while it is still positive it sheds `dword_46494C` (ms
   per tick) per iteration, bumping `+80` once each time round.
5. `sub_41DA5C(handle)` yields this sequence's own last-frame index. If the
   elapsed count (cached earlier in this branch) is strictly GREATER than it,
   both `+78` (state) and `+80` (elapsed) are cleared — the pickup state
   ends. That closes the state-4 (pickup) else-block.
6. **(B)** On the shared tail, outside that block, the handle is resolved
   again and the frame index is recomputed UNCONDITIONALLY from the
   walk-phase word at `+48` divided by 3 — overwriting (A) — after which the
   branch jumps to the shared shadow+body draw site (`LABEL_239`).

Step (A)'s elapsed-based frame index is dead — it is immediately recomputed
at step (B) from the `+48` counter divided by 3, the exact same "leg-cycle"
walk-phase counter that drives the ordinary walk/stand/carry poses elsewhere
in this file (already correctly ported as `walk_phase_[i] / 3`). This shared
tail is reached by states {0 (idle), 3 (unused), 4 (pickup)} — everything
that *isn't* kick/punch/trampoline/spin/cornerhead, which all jump to
`LABEL_239` early with their own elapsed-based frame index intact. Only the
(A) computation's SIDE EFFECTS survive: advancing the `+80` elapsed counter
and the `elapsed >= statecnt` exit check that ends the pickup state. The
actual frame shown on
screen while picking up a bomb is therefore whatever the player's walk-phase
counter happens to read at that moment (frozen at its last value if the
player wasn't mid-step when the grab started), not a clean 0-to-N
playthrough of `PUP*.ANI`.

**Port**: `libs/render/src/renderer.cpp` lines 793-798:

```cpp
const Anim& up = q.pickup[body_colour][dir];
if (pickup_pose_[i] > 0 && !up.steps.empty()) {
    a = &up;
    ph = static_cast<std::size_t>(static_cast<int>(up.steps.size()) - pickup_pose_[i]);  // elapsed-since-grab
}
```

The port uses a clean elapsed-since-grab counter (0..N-1, matching the
already-correct EXIT-timing comment it cites) for the FRAME too, which is
the (A) computation the original actually throws away.

**Visible effect**: subtle — the pickup animation plays as a clean forward
sequence in the port but, per the original, actually starts at whatever
phase the walk leg-cycle was in and advances by walk-phase increments (which
freeze if the player isn't actively walking during the grab). Likely reads
as "pickup animation always plays smoothly" (port) vs. "pickup animation can
start mid-cycle / stutter" (original) — a minor, hard-to-notice difference
in practice, but a genuine one.
**Severity**: Low (subtle, single transitional pose, easy to miss even in a
direct A/B).
**Confidence**: Medium-High (confirmed against the raw Hex-Rays `pseudo.c`
output directly, not just the transliteration, and the surrounding file's
extremely thorough HEXRAYS-FIX annotation practice does not flag this
specific recomputation as a decompiler artifact — but no disassembly
byte-check was done to rule out a Watcom-optimizer quirk Hex-Rays
mis-rendered; recommend a `native/tools/disasm.py sub_41F29B` spot check of
the ~0x4203xx region, pseudo.c ~23409-23410, before changing code on this
one alone).
**Suggested fix**: if disasm confirms, change the pickup-pose frame from
`up.steps.size() - pickup_pose_[i]` to `moving_[i] ? walk_phase_[i] / 3 :
0` (the same formula already used for the walk/stand pose above it),
keeping `pickup_pose_[i]`'s existing countdown only as the state's exit
timer (unchanged).

---

## Verified faithful (no change)

- **Draw order across entity types** — actors → bombs → powerups →
  flame/burn → players, matching `sub_42A191`'s call sequence
  (`sub_4056CA`, `sub_4245B9`, `sub_424F89`, `sub_426D06`, `sub_420F07`
  in that order, confirmed in `native/src/game/batch_0x4293E5.cpp` lines
  802-813). Already fixed/documented in `facts.md` §5; re-verified against
  the native transliteration in this pass, unchanged.
- **Static solid/brick tiles as a background layer** (drawn once per
  cell-change in the original, not per frame) — `draw_cells` composites
  first, matching the "tile layer addendum" fact; not re-derived here but
  no contradiction found in `sub_425D22`'s stamp-vs-live-draw split.
- **Flame arm-piece selection** (`flame_piece`, a 1:1 table lookup off the
  sim-decided `FlameKind`, not a live neighbour scan) — matches `off_45BEA0`
  order; already `facts.md`-confirmed, re-verified.
- **Flame/brick-burn frame pacing** — `s.tuning.flame_frames -
  s.flame[y][x]` / `brick_burn_frames - s.burning[y][x]` exactly reproduces
  `sub_426D06`'s ignition-zeroed, monotonic `+48` per-cell counter
  (`native/src/game/batch_0x426C4C.cpp` lines 185-280); confirmed the
  counter is NOT rescaled to fit sequence length, matching the port's
  `draw_anim`'s plain `% statecnt` wrap.
- **Flame draw dx/dy offset** (the one draw site that applies
  `sub_41DB41`'s per-STAT offset, and the `- tileH/2` centre re-anchor) —
  `sub_426D06`'s real-flame branch (`native/src/game/batch_0x426C4C.cpp`
  lines 245-259) matches `draw_world`'s `dy_centre = sp.dy - kTileH/2`
  exactly, byte-for-byte against the disasm-recovered arithmetic already
  cited in the port's own comment.
- **Walk leg-cycle pacing** (`walk_phase_[i] / 3`, driven by the
  `PlayerWalking` event's disease-scaled px budget rather than position
  delta) — matches `sub_41F29B`'s "walk-phase word at `+48`, divided by 3"
  (pseudo.c 23410) fed by the mover's per-pixel `+48` accrual (`sub_41EC84`,
  `native/src/game/batch_0x41DAA7.cpp` lines 770-982); the "burns the
  budget even when blocked" behaviour (pedal-in-place vs. freeze) is
  correctly modelled.
- **Kick/punch/warp elapsed-frame FORMULA** (not the duration constant,
  Finding 3 covers that) — `ph = kActionPoseTicks - kick_pose_[i]` and
  `ph = kWarpTicks - p.warp` both correctly reproduce the original's
  "elapsed = 0 at entry, increments to statecnt" shape; only the total
  (`kActionPoseTicks`) is wrong, per Finding 3.
- **Warp/spin pose selection and priority** — the sequence-name buffer is set
  to the literal `"spin"` for
  both state 6 (warp-out) and state 7 (warp-in), elapsed-based frame,
  8-tick (`>8`, i.e. `dword_46494C`-quantum) phase-flip from out to in —
  matches `q.spin[body_colour]` + `kWarpTicks - p.warp`, already
  `facts.md`-confirmed.
- **Trampoline flight pose** — the original does NOT switch to a dedicated
  "flying" sprite name for state 5; the sequence-name buffer is left holding
  whatever walk/stand/walkbomb/standbomb name `LABEL_155` put there, and its
  frame is the `+48` walk-phase counter divided by 3 (the same walk-phase
  formula as ordinary standing/walking) — the ONLY original-side
  special-casing is the Y-lift (lift = `elapsed_half_bounce * getvalue(681)`)
  and the shadow suppression.
  The port's `draw_world` correctly leaves `a`/`ph` at whatever walk/stand
  selection was already made and only adds `lift`/suppresses the shadow —
  no dedicated pose override, matching the original exactly.
- **Disease flash gate and scope** — `(p.disease_timer & 8) != 0` matches the
  original's bit-3 test (`& 8`) on the word at `+120` (the low word of the
  same dword used elsewhere as the disease-age accumulator), and `LABEL_239`
  is confirmed to be the SINGLE
  shared shadow+body draw site reached by every pose branch (kick, punch,
  trampoline, spin, cornerhead, and the pickup/idle/state-3 shared tail) —
  so computing `body_colour` once before the pose `if`-chain and reusing it
  everywhere (as the port does) is correct; the strobe is not
  pose-selective in the original either.
- **Bomb-carry arc** (`kCarryArcIds`/VALUELST 500/502/504/506, the
  `carry_ticks_` 0..3 clamp) — already `id-audit.md`-cited and matches
  `sub_42331C`'s carried-state branch; not re-derived here, no
  contradiction found.
- **Gold Bomberman twinkle** (`update_gold_sparkles`, one-attempt-per-
  matching-player-per-tick spawn, 5-in-6 placement odds, fixed-at-spawn
  screen position, age-vs-sequence-length retirement, per-team vs.
  per-slot gating) — matches `sub_420D4E`/`sub_420E39`
  (`native/src/game/batch_0x420D4E.cpp` lines 69-145) line-for-line,
  including the `dword_4621F0 != dword_464994` sim-tick gate the port
  reproduces implicitly by only calling `update_gold_sparkles` from
  `sample_movement` (already tick-gated).
- **Powerup-token reveal gate** (hidden until the CELL reads blank, not at
  ignition) and the bomb-vs-powerup-vs-flame draw-order fix — both already
  `facts.md` §5-documented and independently re-verified against
  `sub_424F89`/`sub_42A191` in this pass.

---

## Not audited (out of scope / needs a different system's pass)

- The general ANI-hotspot bottom-centre-anchor convention itself
  (`docs/formats/ani.md`) — `sub_425D22`'s tile stamp path
  (`native/src/game/batch_0x42583B.cpp`, pending) blits via a DIFFERENT,
  non-hotspot direct-rect path (`sub_41532B`) than the sprite-queue path
  (`sub_415A9F`/`sub_415920`) every other draw in `renderer.cpp` goes
  through; reconciling the two would require auditing the ANI hotspot
  PARSER (`libs/render/src/sprites.cpp`), which is asset-layer, not this
  system.
- `boxed_in`'s exact blocked-tile predicate vs. `sub_41E5C3`
  (`!sub_422E48 && sub_425FB9==0`) — plausible match, not exhaustively
  cross-checked against `sub_422E48`'s flying-bomb exclusion.
