# Enclosure — the HURRY wall-closing spiral

RE of the wall-closing ("enclosure" / HURRY) mechanic in BM95.EXE (Watcom,
imagebase 0x400000). Distilled here per the RE workflow. Roadmap item #37.
Re-audited 2026-07-10 (line-by-line arithmetic fidelity pass) — §2 and §4
below correct the 2026-07-04 pass's trigger-boundary and spiral-cadence
arithmetic; the verdict table in facts.md summarizes what changed.

When the match clock runs low, solid wall tiles drop in a clockwise spiral from
the top-left corner inward, crushing whatever they land on. This is the
pressure mechanic that ends drawn-out rounds.

## 1. The stepper — `sub_426818`

`sub_426818` is called once per rendered frame from the in-game main loop
(`sub_42A191`). It owns the whole enclosure: arm/disarm, the preview
animation, and the actual tile drops.

### State (file-scope dwords)

| addr        | as    | meaning                                                   |
|-------------|-------|-----------------------------------------------------------|
| dword_45BE9C | flag | armed (1 = walls closing)                                |
| dword_462230 | int  | current wall tile X                                      |
| dword_462234 | int  | current wall tile Y                                      |
| dword_462238 | int  | current spiral direction, indexes cos/sin below          |
| dword_462240 | int  | current ring depth                                       |
| dword_46223C | DWORD| last-drop timestamp, ms (`timeGetTime()`)                |
| dword_462244 | int  | `rand()%3` — which of 3 drop SOUNDS (presentation)       |
| dword_464974 | int  | `getvalue(27)` = enclosement_depth (loaded once at match init, `sub_41095A`, pseudo.c 14651) |
| dword_4648AC / dword_4648B4 | int | board width 15 / height 11               |

`dword_45BECC[4] = {0,1,0,-1}` (cos) and `dword_45BEDC[4] = {-1,0,1,0}` (sin)
— confirmed against the raw `.data` initializer dump, not just usage sites —
indexed by direction in **GODIR order** (0=Up,1=Right,2=Down,3=Left). So a
step `(x += cos[dir], y += sin[dir])` walks one tile in that godir.

`dword_45BE9C`'s raw `.data` initializer is **1**, not 0 (every other state
dword here is `.bss`, i.e. implicitly 0). That is not a gameplay-visible
quirk: it just means the very first frame of the very first match always
takes the DISARM branch below (since the clock starts well outside the
closing window), which is *also* the only place that seeds `dword_462238 = 1`
(Right) and zeroes position/depth — i.e. the odd initializer is how the spiral
gets its initial state without a dedicated "new match" init call. A real
match clock only counts down, so disarm never fires again after that.

## 2. Trigger time — TWO distinct, non-overlapping windows  [RE-CONFIRMED 2026-07-10]

Per frame, in this order:

1. read `hurry_seconds = getvalue(101)`;
2. read `remaining = sub_410578()` — SECONDS REMAINING (`dword_4601A4`);
3. test `remaining <= hurry_seconds - 5` — **NON-STRICT `<=`**;
4. if that holds AND `dword_45BE9C` is still 0, **ARM**, writing in exactly
   this order:

   | # | write | meaning |
   |--:|-------|---------|
   | a | `dword_462244 ← rand() % 3` | which of the 3 drop SOUNDS, latched once (presentation) |
   | b | `dword_45BE9C ← 1` | armed |
   | c | `dword_46223C ← sub_43ACF8()` | = `timeGetTime()`, seeds the drop clock |
   | d | call `sub_405D0C()` | *** IS an actor-registry sweep: deactivates every WARPHOLE and TRAMPOLINE. RE-CONFIRMED 2026-07-26; the 2026-07-20 "correction" that called this an inert lobby-table cleanup was itself wrong and is retracted. See §5.1. *** |

   (if the predicate holds but `dword_45BE9C` is already 1, nothing at all
   happens — arming is edge-triggered);
5. otherwise (predicate false) and only if `dword_45BE9C` is set, **DISARM**
   — the branch reachable only if time somehow went back up:
   `dword_45BE9C ← 0`, `dword_462230 ← 0`, `dword_462234 ← 0`,
   `dword_462240 ← 0`, `dword_462238 ← 1`.

- `sub_410578()` returns `dword_4601A4`, set in `sub_4105D2` as
  `(dword_4601AC - dword_4601B8) / 1000` = **whole seconds remaining, FLOORED**
  (total match ms minus elapsed ms, over 1000, clamped ≥ 0, C integer
  division truncates toward zero for non-negative operands = floor).
- **The banner is a SEPARATE check, in the HUD routine (~29533,
  `sub_42A191`), with the OPPOSITE strictness on both sides.** It runs, in
  this order:
  1. read `remaining = sub_410578()` and `hurry_s = getvalue(101)`;
  2. test `remaining < hurry_s` — **STRICT `<`**; if false, nothing below
     runs;
  3. inside, re-read both (`getvalue(101)` then `sub_410578()` — the value is
     unchanged within the same frame) and test
     `remaining > hurry_s - 5` — **STRICT `>`**;
  4. inside that: if the latch `dword_464984` is still 0, set it to 1 and
     call `sub_427961(2700)` — the "HURRY!" cue, ONCE;
  5. still inside that: draw the blinking "Hurry" HUD text — every frame this
     branch is taken.

  (In the decompiled listing both comparisons' LEFT operands appear as bare,
  unassigned-looking calls — `sub_410578()` / `sub_412135(101)` with no
  named local receiving the result. That is a decompiler artifact of Watcom's
  REGISTER calling convention (confirmed from `sub_412135`'s own signature:
  argument AND return both travel in EAX), not a discarded value: the
  compiler evaluates the first call, parks its EAX result in EDX to survive
  the second call's own EAX-argument handoff, and the decompiler doesn't
  reconstruct that as an explicit assignment. The SAME pattern appears in
  §1's arm check above, where the `getvalue(101)` result is preserved the
  same way.)
- Combined, the banner is active exactly while
  **`hurry_seconds - 5 < remaining < hurry_seconds`** (open both ends) — i.e.
  remaining ∈ {hurry_seconds−4, …, hurry_seconds−1}, a 4-whole-second window —
  and the wall-arm (§ above) fires the instant that window closes
  (`remaining <= hurry_seconds - 5`). **No gap, no overlap**: the banner turns
  off on exactly the tick the walls arm.
- The disarm branch resets the spiral to **(x=0, y=0, depth=0, dir=1=Right)**.
  Because a real match clock only counts down, disarm never fires mid-round;
  it only matters as the de-facto "new match" init (§1).

### Our port: floor first, then compare — not "compare against ticks×20"

Our sim only has whole ticks, not a continuous ms clock, so `remaining` has
to be reconstructed from `s.ticks_left`. Since `kTicksPerSecond` ticks make
one second and `ticks_left` is itself already clamped ≥ 0
(`simulation.cpp`), **`s.ticks_left / kTicksPerSecond` (integer division)
reproduces `sub_410578()`'s floor exactly** — there is no rounding
ambiguity, because `ticks_left` advances in lockstep with the same 50 ms/tick
clock the original's `dword_4601B8` elapsed-ms counter does.

The EARLIER (2026-07-04) revision of this port instead compared raw
`ticks_left` directly against `threshold * kTicksPerSecond`. That is **not**
the same predicate:
- `ticks_left <= H*20` is true for `ticks_left ∈ {..., H*20}`, which
  corresponds to `remaining <= H` **only at the exact tick `ticks_left ==
  H*20`** (`remaining == H`, floor-division remainder 0) — for the other 19
  ticks of the `remaining == H` window it is *also* true, so as an
  implementation of `remaining <= H` it's actually fine... but the banner
  needs the STRICT `remaining < H`, and `ticks_left <= H*20` includes the
  entire `remaining == H` window (all 20 ticks), firing the banner a full
  second early on the boundary tick.
- Symmetrically, `ticks_left <= H*20` as an implementation of the (non-strict)
  wall-arm predicate only becomes true at the LAST tick of the `remaining ==
  H` window (`ticks_left == H*20` exactly, remainder 0) — 19 ticks LATER than
  the correct "first tick `remaining` reaches `H`" (`ticks_left == H*20+19`).

Net effect of the old comparison: **the banner fired ~1 tick too early, and
the wall-arm fired ~19 ticks too late.** Flooring `ticks_left/kTicksPerSecond`
once and comparing the resulting whole-second value with the ORIGINAL's exact
strictness (`<` for the banner, `<=` for the arm) fixes both with zero slop —
see `EnclosureSystem::update()`.

- **NO `ticks_left > 0` guard**: `sub_410578`'s remaining-seconds is CLAMPED
  to ≥ 0 (never negative), so once the threshold predicate goes true it stays
  true forever — the original keeps closing walls through and past TimeUp
  (sudden death), it never freezes the spiral. Our `ticks_left` similarly
  floors at 0, so `seconds_left` floors at 0 too and the predicate stays
  monotonic — the direct equivalent.

## 3. Cadence — 250 ms per EVENT = 5 ticks  [CONFIRMED 2026-07-04, refined 2026-07-10]

The drop loop (`LABEL_26`, ~27225) runs, per iteration, in this order — with
`now` = `timeGetTime()` and a per-frame drop budget initialised to 5:

1. if `dword_46223C + 250 >= now`, **RETURN** — fewer than 250 ms since the
   last drop, so wait;
2. decrement the per-frame budget and **RETURN** once it has been exhausted
   — cap 5 drops per frame;
3. `dword_46223C += 250` — advance the drop clock by exactly one 250 ms
   bucket (not "set to now");
4. `sub_4278F2(dword_462244 + 140)` — play the wall-drop sound
   (id 140 + the `rand()%3` variant latched at arm time);
5. `sub_425E9B(dword_462230, dword_462234, 1)` — DROP the wall at the current
   (x, y), **UNCONDITIONALLY**;
6. clear player / flame / powerup / bomb on that tile (§5);
7. advance the spiral to the next (x, y) — §4, and it may or may not actually
   move;
8. loop back to step 1, to catch up any further 250 ms buckets.

- **The interval is a HARDCODED 250 ms** (`+= 250`), gated by `timeGetTime()`.
  It is NOT a VALUELST getvalue — the only enclosure getvalues are id 27
  (depth) and id 101 (threshold). `dword_46494C = 1000/getvalue(30) = 1000/20
  = 50` ms/tick, so 250 ms = **exactly 5 ticks** at the locked 20 Hz rate.
- The per-frame budget of 5 (step 2) caps drops at 5 per frame — a
  wall-clock catch-up for dropped
  frames. In deterministic lockstep every frame is 50 ms, so at most one 250 ms
  bucket elapses per 5 ticks and the cap never engages: a clean **1 EVENT / 5
  ticks**, uniformly (§4 explains why "event" and "newly-solidified tile" are
  not the same count).
- The first EVENT drops 250 ms (5 ticks) AFTER the arm frame: on the arm
  frame `dword_46223C == now`, so `dword_46223C + 250 >= now` is true and the
  loop returns without dropping.
- `dword_462244 + 140` is a SOUND id (three drop-sound variants); `rand()%3`
  is drawn once at arm time, on the presentation stream — the sim draws NO RNG
  for the enclosure. The sound plays on EVERY event, including the phantom
  repeats in §4 (verified: `sound_director.cpp`'s `WallClosed` handler already
  replays the latched roll unconditionally on every event it receives, so no
  presentation-side change was needed for this).

## 4. Spiral geometry — THE CRUX, reconstructed as a literal state machine

**This is the part worth being exact about, and a from-scratch "ring
perimeter formula" gets it subtly wrong.** The advance logic
(`LABEL_47`/`LABEL_60`/`LABEL_61`, ~27267-27298), reached after EVERY drop
(phantom or not):

State: current tile `(x, y)`, current direction `dir`, current ring `depth`.
Per advance, in this order:

1. Compute the tile AHEAD in the current direction:
   `ahead = (x + cos[dir], y + sin[dir])`.
2. **ACCEPT** it — `(x, y) ← ahead` — if and only if **all four** of these
   hold (i.e. `ahead` is still inside the ring box):
   `ahead.x < width − depth`, `ahead.y < height − depth`,
   `ahead.x ≥ depth`, `ahead.y ≥ depth`.
3. Otherwise **TURN CLOCKWISE**: `dir ← (dir + 1) & 3`, and then:
   - if `dir` has wrapped a full turn back to **1 (Right)**:
     - if `2 × getvalue(27) ≤ depth`, **RETURN** — the target ring has been
       reached, and the spiral STOPS for good;
     - otherwise step inward one ring, diagonally:
       `depth += 1; x += 1; y += 1`.
   - else `(x, y)` is left **UNCHANGED** — the NEXT `LABEL_26` iteration
     re-drops this same tile (same sound, same crush/detonate checks) before
     trying the new direction.

Two consequences fall directly out of this that a "one event per unique
tile" model misses:

1. **Three of a ring's four corners cost an EXTRA 250 ms/5-tick event with NO
   new tile.** Turning a corner (any turn that doesn't ALSO complete the
   ring, i.e. the top-right, bottom-right, and bottom-left corners of a
   clockwise-from-top-left ring) leaves `(x, y)` unchanged for this call. But
   `LABEL_26`'s top ALWAYS drops+plays-sound+runs-crush-checks on whatever
   `(x, y)` currently is, unconditionally, on every event — it does not know
   or care whether the position actually moved. So the corner tile gets
   dropped a SECOND time, 5 ticks later, before the walk continues in the new
   direction. Cosmetically a no-op (the tile is already solid) — but it DOES
   replay the wall-slam sound, and it DOES give a second chance to crush a
   player or detonate a bomb that has since moved onto that exact tile.
2. **Every ring's own start tile is naturally revisited a second time — NOT
   via a phantom repeat, via an entirely ordinary ACCEPTED step.** The bounds
   check only excludes tiles OUTSIDE the current ring box (`x/y` vs.
   `depth`/`width-depth`/`height-depth`); it has no memory of which tiles in
   that box have already been visited. Walking up the left edge, the ring's
   own start corner `(depth, depth)` satisfies the bounds check exactly like
   every other tile on that edge, so the up-walk runs all the way back to it
   before finally failing (one step further up, `y < depth`) and wrapping the
   direction back to 1 — AT WHICH POINT the ring-complete check fires and
   (assuming more rings remain) the walk steps diagonally inward with no
   phantom pause. So the fourth corner is "free" (no extra cadence slot) but
   still costs a duplicate DROP of that tile.

Net: **each ring costs 4 extra events beyond its unique-tile count** (3
phantom corner repeats + 1 ordinary-but-duplicate start-tile revisit) — e.g.
the outer ring of a 15×11 board has 48 unique tiles but **52 events**. A full,
literal reconstruction of ring 0's exact 52-event sequence — including
exactly where the three phantoms and the one ordinary duplicate land — is
pinned in `tests/test_sim.cpp`'s `"the enclosure spiral's full ring-0 event
order, phantoms and all"` test case.

- Starts at **(0,0)** with **dir = 1 (Right)**, walks the top edge, turns
  clockwise (Right→Down→Left→Up), steps one ring inward each full loop.
- `sub_425E9B(x,y,1)` sets the tile solid; the surrounding cleanup kills any
  player standing there, detonates/eats a bomb on it (per id 46), and clears
  flame/powerups (§5). Solidifying an already-solid tile (the phantom/
  duplicate case) is a harmless no-op.

### Ring count — flagged DEVIATION-reported, NOT changed

VALUELST 27's own AUTHORED comment (`DATA/RES/VALUELST.RES`, verbatim):

```
; default enclosement depth (how far the playfield will close in)
; 0 is none, 1 is 2 rows, 2 is 4 rows, 3 is all the way
27,1
28,4        ; the possible different enclosement depths (0, 1, 2, 3 right now)
```

i.e. **rings closed = 2 × depth setting** (0/2/4/"all"). This port's
`EnclosureSystem::total`/`position` close exactly that many rings, matching
this comment and the pre-existing golden-tested behaviour (`cells[2][2]`
stays open at depth 1 in `tests/test_sim.cpp`).

A LITERAL transcription of the stop check above (`if (2*getvalue(27) <=
depth) return;`, evaluated once a ring's own traversal has fully wrapped back
to dir 1) reads as "stop once the ring that JUST closed is ring number
`2*depth`" — i.e. rings `0..2*depth` INCLUSIVE, **one ring more** than the
comment says (re-implemented and cross-checked against a 15×11 board: depth 1
→ 3 rings/120 unique tiles, depth 2 → 5 rings/144 unique tiles, under that
literal reading). Depth 3 ("all the way") cannot discriminate between the two
readings — a 15×11 board only has 6 valid ring depths (0..5) either way, so
both readings close the whole board there.

This is a genuine, unresolved conflict between two credible sources (a
carefully re-verified direct disassembly reading vs. the developer's own
authored comment + the pre-existing golden/test-pinned behaviour), and static
analysis alone could not resolve which one is a decompiler/transcription
artifact and which is the real shipped behaviour. Per the audit's own
ground rules, this is reported rather than changed: the port keeps the
comment-and-golden-corroborated **"2 × depth"** rule. If this is ever
resolved (e.g. by running the real EXE with a custom scheme, `enclosement_
depth=1`, on a board wide enough to make the two readings' 3rd/5th ring
visibly distinguishable, and counting rings on screen), update
`rings_for()`'s comment in `enclosure.cpp` and this section together.

## 5. Wall vs. contents — order, and what each check actually does

The per-tile cleanup, in the original's exact order (all confirmed, all
already correctly ordered in this port; §"Wall vs. contents ordering" note
below explains why the port's internal order differs textually but not
observably):

1. **Player** (`sub_421D3F` finder → `sub_41DE63` kill, up to 100 retries):
   `sub_421D3F`'s own search predicate is `present && !dead && type-byte(+16)
   != 4 && tile == (x,y)` — type 4 is "network-spectator", a category this
   port has no equivalent slot for (N/A, never reachable). **`sub_41DE63`
   itself — the SAME shared kill routine the ordinary flame-death and the
   campaign rover/ghost landing-tile kill also funnel through
   (`docs/re/campaign.md` clause 4) — additionally early-outs (returns 0, no
   death, no RNG) while the victim's movement-state word (+78) is 5
   (trampoline hop) or 6/7 (warp out/in).** A player mid-bounce or mid-warp
   when the wall drops on their tile is untouched. No attributable killer
   (crush, not a flame): `PlayerDied.data == -1`.
2. **Powerup** (`sub_42542D` finder → `sub_4254F3`, up to 100 retries):
   unconditional destruction, NO skull-relocation compensation here (that only
   happens on the flame-walk's OWN powerup-burn call site, which additionally
   checks `diseases_destroyable` and calls `sub_4255B2`; `sub_4254F3` itself
   has no such logic — confirmed by reading its body: it zeroes the powerup
   record's first dword and sets an optional redraw hint, nothing else).
3. **Grounded bomb** (`sub_422E48` finder, motion states 2/3 = flying/carried
   excluded, up to 100 retries — so an airborne bomb sails over, and a
   sliding-but-grounded bomb IS a valid target):
   - **`wall_detonates` ON (id 46)**: `sub_423209(bomb, -1)` — this does
     **NOT** explode synchronously. It only APPENDS to a 100-slot pending
     queue (`dword_4621F8`/`4621FC`/`462200`). See §6.
   - **`wall_detonates` OFF**: `sub_424841(bomb)` zeroes the bomb record in
     place — no explosion, no effect, and (per the pre-existing "Options
     toggles" facts.md entry, unaffected by this audit) the owner's bomb
     count is freed the same way an explosion would.

The original runs these in player → powerup → bomb order; this port's
`drop_wall()` runs bomb → (solidify + clear flame/powerup state) → player,
textually reordered. This is safe because each cleanup step touches
disjoint state (`players[i].alive` vs. `floor/hidden` vs. `bombs[]`) with no
observable cross-dependency between them at this call site — a carried
bomb (which the player branch releases) is never independently found by the
grounded-bomb scan (motion 3 = carried is excluded from `sub_422E48`
either way), and a bomb sharing a tile with a floor powerup is not a
reachable state under normal placement rules. See `drop_wall`'s comments for
the full per-branch citation.

### 5.1 Warpholes and trampolines are SWITCHED OFF when the walls arm — `sub_405D0C`  [CONFIRMED 2026-07-26 from the binary + live play]

**This section replaces a wrong conclusion.** Two earlier passes got it
backwards, in opposite directions, and both are retracted here:

- 2026-07-04 glossed `sub_405D0C` correctly ("clear warpholes+trampolines")
  but on a *guess* — it was an unverified cross-batch forward declaration.
- 2026-07-20 (`audit/enclosure.md` Finding 0) "corrected" that gloss to
  "inert level-select lobby-table cleanup, port is right to do nothing". That
  correction is the **wrong** one: it trusted `batch_0x405B3A.cpp`'s own stale
  file header, which mislabels `dword_45E0A8` as a broadcast table. There is
  exactly ONE `dword_45E0A8` in the program (`native/src/globals.cpp` line 653;
  `globals.h` line 678 labels it *"the 100-slot × 152-byte 'extra object'
  pool"*) — the same pool `sub_404D16` allocates, `sub_405654` looks tiles up
  in and `sub_4056CA` draws. `batch_0x405B3A.cpp` is the actor table's
  **network-sync** code (`sub_405B3A` writes actor fields +28/+32/+0/+4/+44/
  +46/+52; `sub_405BBA` broadcasts them ten at a time), which is why its header
  calls it a "broadcast table".
- 2026-07-24 (`audit/enclosure-warphole-close.md`) correctly falsified the
  per-tile hypothesis on geometry and predicted a global arm-time trigger, but
  could not name the mechanism. This is it.

**The body** (`native/src/game/batch_0x405B3A.cpp` lines 298-325, called from
`sub_426818`'s arm branch at `batch_0x42583B.cpp` line 695):

`sub_405D0C` walks the whole actor pool at `dword_45E0A8` — **100 slots,
stride 152 bytes (38 dwords)** — start to finish, no early exit. Per slot, in
this order:

| step | test / write | notes |
|-----:|--------------|-------|
| 1 | slot's ACTIVE dword (+0) non-zero? | if not, skip this slot entirely |
| 2 | read the actor TYPE dword at **+4**, as UNSIGNED | |
| 3 | type == 0 → skip | 0 = DirArrow, left alone |
| 4 | type ≤ 1 **OR** type == 3 → write **0** to the slot's ACTIVE dword (+0) | 1 = Warphole, 3 = Trampoline → DEACTIVATE the slot |
| 5 | anything else → untouched | 2 = Conveyor |

Nothing else in the record is written, and every one of the 100 slots is
visited.

The type field is read UNSIGNED and step 3 has already established it is
non-zero, so the `≤ 1` test in step 4 is exactly `== 1`.
Net: **every warphole and every trampoline is switched off, globally, on the
single frame the walls arm; dirarrows and conveyors are left alone.**

**It is a GAMEPLAY change, not a render hide.** Clearing the slot's active
dword removes the actor from both of the registry's consumers at once:

- `sub_405654(x,y)` (the tile→actor lookup) skips inactive slots, so the
  step-on trigger in `sub_41EC84` never sets movement state 5 (bounce) or 6
  (warp) again — **warpholes and trampolines stop WORKING**. The same lookup
  backs `sub_4230A5`'s sliding-bomb entry probe, so a kicked bomb that used to
  be blocked by a warphole tile (§stage-actors.md §6 item 4) now rolls onto it.
- `sub_4056CA` (the animator) iterates the same 100 slots and only draws a
  slot whose active dword (+0) is non-zero — **so the art disappears too**,
  all four at once, independent of where the spiral is.

**Why those two types and not the other two.** `sub_41DE63`, the shared
player-kill routine the wall crush itself calls (§5 item 1), early-outs while
the victim's movement-state word (+78) is 5 (trampoline hop) or 6/7 (warp
out/in). Leaving warpholes and trampolines live would let a player ride a
bounce or a warp straight through a closing wall, indefinitely. Conveyors and
dirarrows create no invulnerable state, so they survive the sweep.

**Live confirmation (Ege, COAL MINE).** COAL is level index 4 → `EXTRA4.RES`,
four warpholes at (2,2),(12,2),(12,8),(2,8) — all on **ring 2**, which the
spiral closes THIRD (drop events 96-124, ~24-31 s in) and, at the default
`enclosement_depth = 1`, never reaches at all. They were observed vanishing
**the moment the walls started closing**. Only a global arm-time trigger can
produce that; the geometry is worked out in
`docs/re/audit/enclosure-warphole-close.md`, which this finding resolves.

(The `-W` lines really are `-W,1,<idno>,<x>,<y>,<linkto>` with arg0 = 1, and
`-3` normalizes to `15-3 = 12` / `11-3 = 8`, which is where those ring-2
coordinates come from. `EXTRA4.RES` is the only warphole file shipped.)

**NOT the HURRY banner — the WALLS.** The two moments are 5 s apart (§2): the
banner/voice latches on `dword_464984` at `remaining < getvalue(101)`, and the
walls arm at `remaining <= getvalue(101) - 5`. `sub_405D0C` hangs off the
SECOND one. `dword_464984` is a HUD-only latch and gates nothing in
`sub_405654`, `sub_4056CA` or `sub_41EC84`. So the actors survive the whole
banner window and die exactly when the first wall is scheduled — not when
"HURRY!" appears.

**Falsifiable predictions from the same code, not yet play-tested.** Three
things this reading commits to, each cheap to check in the original:
1. On INNER CITY TRASH (`EXTRA10.RES`, conveyors) and the dirarrow maps
   (HOCKEY RINK / ANCIENT EGYPT, `EXTRA2/3.RES`) the belts and arrows must
   KEEP working and KEEP drawing after the walls arm — only the ring-2 tiles
   they sit on go quiet, and only once a wall physically lands on them.
2. On COAL the warpholes must survive the "HURRY!" banner and vanish 5 s
   later, on the first wall drop — not at the banner.
3. With `enclosement_depth = 0` (walls never actually close) the warpholes
   and trampolines must STILL vanish at the arm moment, because the sweep is
   above the depth gate. This one is the sharpest test of the whole reading.

**Port.** `EnclosureSystem::update()`'s arm branch calls
`clear_hurry_disabled_actors(s)`, which sets `State::actor_type` to `None` on
every `Warphole`/`Trampoline` cell (the port's equivalent of zeroing the
slot's active flag) and deliberately leaves `actor_dir` / `warp_dest_*` alone,
because the original leaves every other field of the record intact. NOT
depth-gated: the original's `sub_405D0C()` sits inside the `if (!dword_45BE9C)`
arm block, which is ABOVE the `2*enclosement_depth > ring` drop gate, so even
`enclosement_depth = 0` kills them. An in-flight hop or warp COMPLETES —
`sub_41F29B` states 5/6/7 never re-consult the registry, and neither do
`tick_bounce`/`tick_warp`. `actor_type` is hashed, so this MOVES GOLDENS (see
`tests/sim/test_golden.cpp`); it is a deliberate behaviour change.

### 5.2 A wall landing on an actor tile does NOT clear that tile's actor

Separately from §5.1: `sub_426818` solidifies the tile with
`sub_425E9B(x,y,1)` but its per-drop cleanup touches ONLY the player/powerup/
bomb/flame state listed in §5 — it never writes the stage-actor registry
(`dword_45E0A8`). Verified by the 2026-07-20 audit's full `sub_426818` trace
AND by grepping every write to `dword_45E0A8` in the native transliteration:
the only ones reachable at all are `sub_404D53` (level load), `sub_404E3C`
(slot claim), `sub_405B3A` (editor/net set), `sub_405D7E` (editor-only
truncate, gated on `sub_40C06A() == 1`) and `sub_405D0C` (§5.1). So a
CONVEYOR or DIRARROW whose tile the closing wall lands on **stays live in the
registry** — it is just never drawn again (§5.3) and never reachable (a solid
tile is impassable). The port matches: `drop_wall()` does not touch
`actor_type`.

### 5.3 The animator's draw gate is PER TYPE — `sub_4056CA`  [CONFIRMED 2026-07-26]

Read off `sub_4056CA`'s switch (`native/src/game/batch_0x404852.cpp` lines
736-894). The gate is **not** uniform:

| case | type       | draw gate                                                   |
|-----:|------------|-------------------------------------------------------------|
| 0    | dirarrow   | `if (!sub_425FB9(x,y))` — skip unless the cell is walkable floor |
| 1    | warphole   | **no solid test at all**; only `if (+52)` (the parsed arg0, which every `-W` line sets non-zero) |
| 2    | conveyor   | frame counter `+48` advances unconditionally; the DRAW is `if (!sub_425FB9(x,y))` |
| 3    | trampoline | `if (!sub_425FB9(x,y))`                                      |

There is **no** global early-out on `dword_45BE9C` or the hurry clock anywhere
in `sub_4056CA`, and `sub_42A191` calls it unconditionally
(`batch_0x4293E5.cpp` line 807). So the per-tile hypothesis was right *as a
draw gate* — it was just the wrong explanation for the COAL report, which
§5.1 now accounts for.

Port: `Renderer::draw_actors` keeps the `cells != Cell::Blank` skip for
dirarrow/conveyor/trampoline and exempts `Warphole`, matching the table. That
exemption is only reachable via level 7's brick regen landing a brick on a
warphole tile; the HURRY walls can no longer cover a live warphole, because
§5.1 removed it first.

### 5.4 Retracted text (kept as a signpost)

Everything that used to stand between here and §6 — the "render-only hide"
reconciliation inferred from the sibling powerup drawer `sub_424F89`, the
2026-07-24 GEOMETRY UPDATE's open question, and the "unresolved warp-onto-a-
covered-destination edge" — is superseded by §5.1/§5.3 above. The warp-onto-a-
covered-tile edge in particular is now **unreachable via the enclosure**: the
walls cannot cover a live warphole, because arming removes every warphole
first. (It remains theoretically reachable via level 7's brick regen; still
un-RE'd, still not patched, but no longer on the enclosure's critical path.)

## 6. Wall-triggered bomb detonation is deferred ONE TICK, not synchronous [CONFIRMED 2026-07-10]

Traced `sub_423209`'s queue end-to-end:

- `sub_423209(bomb_ptr, reason)` appends `bomb_ptr` to `dword_4621F8[]` and
  `reason` to `dword_4621FC[]`, bumping the count `dword_462200` (cap 100).
  Nothing else — no explosion, no fuse write, here.
- The queue is drained inside `sub_42331C` (the bomb/fuse tick function),
  gated `if (dword_462210 != dword_464994) { ...drain...; dword_462210 =
  dword_464994; }` — i.e. **the drain runs at most ONCE per rendered frame**
  (`dword_464994` is the frame counter, incremented once per frame in
  `sub_42A191`). The drain force-sets each queued bomb's elapsed-fuse word
  (+68) to its OWN threshold — a straight word copy, **+68 ← +74** — and
  stamps its "incoming direction" byte (+56) from
  the queued reason. `sub_42331C`'s own per-bomb loop, LATER IN THE SAME
  CALL, unconditionally checks whether fuse (+68) ≥ threshold (+74) and, if
  so, explodes (this check is NOT gated by the dud/kind exclusion that guards the
  fuse-INCREMENT a few lines above it) — so a freshly force-set bomb detonates
  within that SAME `sub_42331C` call.
- `sub_42A191`'s per-frame order calls `sub_42331C` **TWICE**: once via
  `sub_4245B9` (mode 0), BEFORE `sub_426818` (enclosure); once via
  `sub_42459A` (mode 1), AFTER `sub_426818`. The drain only fires on
  whichever of the two runs FIRST each frame — `sub_4245B9`, since it
  precedes `sub_426818` in the call order. So: a bomb queued by THIS frame's
  wall drop sits undrained (the frame-stamp gate already fired earlier this
  same frame) until the FOLLOWING frame's `sub_4245B9` call.

**Net: a wall-detonated bomb's flame appears exactly ONE TICK (one frame)
after the wall itself solidifies, not on the same tick.** Ported as: instead
of exploding synchronously, `drop_wall()` sets `b.fuse = 1`, so the sim's own
NEXT `tick_fuses()` pass (which runs before `enclosure.update()` in our own
tick order — the same relative order as the original's `sub_4245B9`-before-
`sub_426818`) detonates it on schedule. `tests/test_sim.cpp`'s "hurry walls…"
test and `tests/test_stomped_diseases.cpp`'s "stomped_bombs_detonate ON" test
both pin the one-tick gap directly (bomb still present immediately after the
drop tick, gone one tick later).

One known, narrow gap NOT closed by this fix: a bomb that is CURRENTLY
fizzling as a dud (`Bomb::dud_left > 0`) has its fuse frozen by
`BombSystem::tick_fuses`'s own dud branch (`continue`s past the fuse check
entirely), so forcing `fuse = 1` on a dud-at-the-moment-of-crush bomb is
silently absorbed — it keeps fizzling on its own schedule instead of being
force-detonated. In the original, the drain's force-set bypasses the
kind-based fuse-increment gate (the explode check itself isn't kind-gated),
so a dud WOULD be forced to detonate. This needs a wall-crush ↔ dud
interaction this narrow (both conditions simultaneously) to matter at all;
flagged here, not fixed, given how deep into `tick_fuses`'s existing
dud-state structure a faithful fix would have to reach for a corner this
small.

### A much bigger, EXPLICITLY OUT-OF-SCOPE discovery made while tracing this

The SAME `sub_423209` queue-and-drain-next-frame mechanism is also used by
ORDINARY bomb chain reactions: the flame-arm walk's grounded-bomb hit
(`sub_42331C` ~25645, inside the just-exploded bomb's own arm-walk) queues
the hit bomb via `sub_423209(hit_bomb, direction_reason)` — the SAME function,
the SAME once-per-frame drain. If that reading is right, **ordinary chain
reactions in the original take one extra frame PER LINK to cascade** (bomb A
explodes frame N; a chained bomb B doesn't actually detonate until frame
N+1), not the same-frame/same-tick cascade this port's
`FlameSystem::spread_to` currently performs (`explode()` called synchronously,
recursively, the instant the arm walk reaches a grounded bomb). This is
**far** outside "enclosure" — it would be a fundamental, codebase-wide change
to bomb-chain pacing with enormous golden impact across nearly every existing
multi-bomb scenario, and deserves its own dedicated audit rather than a
same-commit change riding in on this one. **Not touched here.** See the audit
report for the full trace (`sub_42331C` pseudo.c ~25611-25652).

## 7. RNG — none in the enclosure itself  [CONFIRMED, unchanged]

The ONLY `rand()` call anywhere in `sub_426818` is `dword_462244 = rand() %
3` at ARM time (§2/§3) — the drop-SOUND variant pick, drawn once per arm, on
the presentation stream. The sim's `EnclosureSystem` draws ZERO
`State::rng` — golden B/C's hash constants move (see `tests/test_golden.cpp`'s
UPDATE note) purely from *when* and *where* walls solidify, never from any
new/reordered RNG draw; every existing RNG-stream assertion in the golden
suite (D's `kExpectedRng`, E's final `rng`/bounce count) is byte-identical
before and after this audit's fixes.

(A cosmetic, PRESENTATION-ONLY detail found but not ported: `sub_426818` also
runs a per-frame "preview" pass — `10 * getvalue(910) + 100` down to
`getvalue(910)` steps, `sub_424DFE(x, y, alpha)` — that walks the SAME
advance logic as §4 on LOCAL copies of the position state, purely to draw a
fading highlight over the next few tiles about to close. It mutates no
gameplay state and draws no RNG; this port has no such preview overlay and
none is added here.)

## 8. Round end STOPS the spiral — `sub_421969() > 1`  [CORRECTED 2026-07-26]

**This section previously said the opposite and was wrong.** It read
`sub_426818`'s top-level gate `sub_421969() > 1` as a static "are we in a
match" flag set once at round start, and concluded the walls keep closing
through the round-decided window. `sub_421969` is not static: it is
**recomputed every single frame**.

The ENTIRE stepper sits inside that gate (`native/src/game/
batch_0x42583B.cpp` 678-682). Per frame, in order:

1. call `sub_421969()` and compare its result with 1 — if it is **not > 1**,
   return immediately, doing nothing at all;
2. inside the gate, first: if `sub_40C06A() != 1` (not the editor) AND
   `sub_412135(dword_46499C + 340)` (= `getvalue(dword_46499C + 340)`) is
   non-zero, call `sub_426704()` — the
   per-level tile regen (level 7), so that is *also* gated by the same check;
3. then the arm/disarm check (§2), the preview pass (§7) and the 250 ms drop
   loop (§3) — all of them inside the same `> 1` body.

`sub_421969` itself (`native/src/game/batch_0x420D4E.cpp` 525-532) is three
tests, evaluated in this order — the first that hits wins:

| # | test | returns | meaning |
|--:|------|---------|---------|
| 1 | `dword_46489C` (campaign active) non-zero | **2**, hardcoded | campaign is forced to 2 ⇒ it can never freeze |
| 2 | else `dword_464964` (team mode) non-zero | `dword_4621DC` | TEAMS still in play |
| 3 | else | `dword_4621D4` | free-for-all: PLAYERS still in play |

`dword_4621D4` is latched from the accumulator `dword_4621D0` at the tail of
the per-frame player pass `sub_420F07` (`batch_0x420D4E.cpp` 170-222), and
`sub_41F29B` bumps that accumulator once per player that is still in play —
line 238 for a live player (`+8 == 0`), line 862 for one still inside its
death animation (`+48 < getvalue(25)`). Teams work the same way via
`dword_4621BC`/`dword_4621C0` → `dword_4621DC`. So the count falls the moment
the last opponent's death animation finishes, and `sub_426818` returns at its
first line from then on: **the walls stop closing when the round is decided,
and never restart** (the count only falls further; the outer match loop also
pauses the match clock right there — `if (sub_421969() <= 1) sub_410522();`,
`batch_0x4293E5.cpp` 1060-1061, and `sub_410522` clears the clock-running flag
`dword_4601B4`).

### The three systems have DIFFERENT gates — this is the useful part

| system | function | round-end gate | behaviour once ≤1 side remains |
|--------|----------|----------------|--------------------------------|
| bomb fuses + detonation | `sub_42331C` | `if (sub_421969() > 1)` wraps BOTH the fuse-elapsed accrual (`+68 += dword_464958`) and the `+68 >= +74` explode branch — `batch_0x422DDD.cpp` 807-817 | **FROZEN** — no fuse advances, nothing detonates, no chain link fires |
| bomb MOVEMENT | `sub_42331C` | none (the `switch(+46)` slide/fly cases run BEFORE the gate) | keeps sliding/flying |
| flames + brick burn-away | `sub_426D06` | **none at all** — `batch_0x426C4C.cpp` 160-281 has no such check | keeps running: live flames age out normally and bricks finish crumbling |
| enclosure + level-7 tile regen | `sub_426818` | `if (sub_421969() > 1)` wraps the whole body | **FROZEN** — spiral stops mid-ring |

So the user-visible "bombs stop going off when the round is decided" and "do
the walls stop too?" are the *same* gate, evaluated in two functions off the
same latched value within one frame — they can never disagree. Flames are the
odd one out and deliberately keep burning.

**NOT the same question as TimeUp.** §2's "NO `ticks_left > 0` guard" note
still stands: the match CLOCK hitting zero does not stop the spiral
(`sub_410578` clamps remaining-seconds at 0, so the arm predicate stays true
forever). Round END stops it. Two different events, two different answers.

**Port.** `simulation.cpp` already had `bombs_frozen = sides_remaining(s) <= 1
&& !s.campaign_hazards_active` (from `audit/bombs.md` finding 1 — the same
`sub_421969() > 1` predicate). It is renamed `round_frozen` and now also gates
`tile_regen.update()` and `enclosure.update()`, which the original covers with
ONE gate. `flames.age_flames_and_bricks()` and `bombs.advance_bombs()` stay
ungated, per the table. `sides_remaining()` counts only `alive` players, so it
drops one death-animation length earlier than `dword_4621D4` does — a
pre-existing approximation shared with the bomb freeze, noted rather than
changed so both systems keep freezing on the same edge as each other.
This MOVES GOLDENS (goldens B and C reach the hurry phase and get decided).

### What this section's evidence is — and what it is NOT

Be precise about which leg each claim stands on, because the previous version
of this section was confidently wrong:

- **Static, first-hand, and decisive**: the gate itself (`sub_426818`'s first
  two lines), `sub_421969`'s body, the per-frame relatch in `sub_420F07`'s
  tail, the two accumulator bumps in `sub_41F29B`, the match loop's
  `if (sub_421969() <= 1) sub_410522();`, and the *absence* of any such gate
  in `sub_426D06`. Each was read directly, not inferred from a sibling or
  from a doc. The earlier error was not a misreading of these lines — it was
  the unchecked assumption that `dword_4621D4` is written once at round
  start. It is written every frame.
- **NOT verified live.** Unlike §5.1 (which Ege observed in play on COAL),
  the round-end freeze has NOT been watched in the running original. The
  in-play check is easy and worth doing: in a 2-player round, arm the walls,
  then let one player die and watch whether the next tile lands 250 ms later.
- **The `native/` oracle harness does NOT cover this path**, and cannot
  without work. `native/src/main.cpp`'s per-tick oracle loop deliberately
  omits `sub_426818` (its own comment: driven by the real wall clock through
  `sub_43ACF8`, which the shim maps straight to `SDL_GetTicks()`, so it is
  non-deterministic, and `dword_464978` is stale at match start so it would
  fire the walls on frame 1). Making the oracle able to arbitrate anything in
  this file needs a faked millisecond clock in the shim plus a scenario that
  reaches the hurry window — neither exists today.

## 9. Addresses (evidence)

| addr        | role                                                       |
|-------------|------------------------------------------------------------|
| sub_426818  | enclosure stepper: arm/disarm (§2), 250 ms cadence (§3), spiral (§4) |
| sub_410578  | seconds-remaining accessor (`dword_4601A4`)                |
| sub_4105D2  | clock update: `dword_4601A4 = (total_ms - elapsed_ms)/1000`|
| sub_412135  | `getvalue(id)` — Watcom register convention, argument AND return both in EAX (confirms the edx-preserved-across-a-call reading in §2) |
| sub_43ACF8  | `timeGetTime()` — the ms drop clock                        |
| sub_425E9B  | set a tile solid (the wall drop)                           |
| sub_405D0C  | **NOT actor-grid clear (corrected 2026-07-20)** — a level-select lobby broadcast-table cleanup (batch_0x405B3A.cpp ~298-325); no gameplay-actor effect. Port correctly does nothing here. audit/enclosure.md F0 |
| sub_421D3F  | player finder: present && !dead && type != 4 (network-spectator) && tile match |
| sub_41DE63  | shared player-kill routine (crush/flame-death/rover-kill); early-out on type 4 or movement-state 5/6/7 |
| sub_42542D / sub_4254F3 | powerup finder / unconditional destroy (no relocation) |
| sub_422E48  | grounded-bomb finder: present && tile match && motion != 2/3 (flying/carried) |
| sub_423209  | detonation QUEUE append (dword_4621F8/FC/462200), NOT a synchronous explode |
| sub_424841  | zero a bomb record in place (the "eat", `wall_detonates` OFF) |
| sub_42331C  | bomb/fuse tick: drains the queue (once/frame, frame-stamped) and runs the normal fuse check that detonates a freshly-drained bomb |
| sub_4245B9 / sub_42459A | the frame's two `sub_42331C` calls (modes 0/1); mode 0 runs BEFORE sub_426818, mode 1 AFTER — only mode 0 ever drains |
| sub_421969  | top-level gate, re-latched EVERY frame from the alive-side tally (`dword_4621D4`, or `dword_4621DC` in team mode; forced 2 in campaign) — the round-decided freeze, §8 |
| sub_405D0C  | arm-time actor sweep: deactivates every warphole (type 1) and trampoline (type 3), leaves dirarrows (0) and conveyors (2), §5.1 |
| sub_405654  | tile→actor lookup; skips slots whose active dword is 0, which is what makes §5.1's sweep a gameplay change |
| sub_4056CA  | per-frame actor animator; per-TYPE draw gate, warphole exempt from the solid test, §5.3 |
| VALUELST 27 | enclosement_depth (rings = 2× per its own authored comment; §4's "Ring count" flags an unresolved literal-disassembly conflict) |
| VALUELST 28 | the depth setting's own valid range (0..3) — "the possible different enclosement depths" |
| VALUELST 101| hurry_seconds — banner strictly below this; walls arm at this − 5, non-strict |
| VALUELST 30 | tick rate 20 ⇒ 50 ms/tick ⇒ 250 ms = 5 ticks              |
| dword_45BECC/45BEDC | cos {0,1,0,-1} / sin {-1,0,1,0}, godir-indexed, confirmed against the `.data` dump |
| dword_462244 + 140 | wall-drop sound (3 variants, rand%3 at arm)         |
