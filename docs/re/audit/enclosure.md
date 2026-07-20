# Fidelity audit — `enclosure.cpp` (system 7, W3)

**Verdict: faithful.** The wall-arm/disarm trigger arithmetic, the 250 ms/
5-tick cadence, the literal spiral state-machine (including the phantom
corner repeats and the free duplicate start-tile revisit), the deferred
one-tick wall-detonation queue, the player bounce/warp crush exemption, and
the "no RNG in the sim" property are all already correct — every one of
these was independently re-derived here from `native/src/game/batch_0x42583B.cpp`'s
`sub_426818` and cross-checked against `docs/re/enclosure.md` §§1-8 and
`docs/re/facts.md`'s 2026-07-10 "Enclosure/HURRY arithmetic audit" entry,
and both docs match the disassembly-derived transliteration line for line.
No code changes are suggested for anything already covered by those two
prior passes (see "Verified faithful" below).

Two things fell out of a genuinely fresh read that neither prior pass
flagged:

1. **A stale, incorrect RE fact living in `docs/re/enclosure.md` itself**
   (not a port bug — the port never implemented the claimed behaviour, and
   that turns out to be the *correct* choice). Severity: **Documentation**
   (Medium — actively misleading for future work), Confidence: **High**.
2. **A real, narrow arithmetic divergence in the `wall_detonates` = ON
   bomb-crush path**: the original only detonates the *first* grounded bomb
   it finds on a crushed tile per drop event and does not search further;
   the port detonates *every* bomb on that tile. Severity: **Low**
   (requires two grounded bombs sharing one tile, an edge case the normal
   placement/kick rules likely never produce), Confidence: **High** on the
   literal reading, **Medium** on real-game reachability.

Scope: `libs/sim/src/systems/enclosure.{hpp,cpp}` only.

References read in full: `native/src/game/batch_0x42583B.cpp` (`sub_426818`
the enclosure stepper itself — this is the file the ledger's native
reference `batch_0x426C4C.cpp` turned out NOT to contain; see Finding 0
below), `native/src/game/batch_0x426C4C.cpp` (`sub_4278F2` wall-slam SFX,
confirmed as documented), `native/src/game/batch_0x420D4E.cpp` (`sub_421D3F`
player-at-cell finder), `native/src/game/batch_0x41DAA7.cpp` (`sub_41DE63`
shared kill routine), `native/src/game/batch_0x42459A.cpp` (`sub_42542D`/
`sub_4254F3`/`sub_425107` powerup query/destroy/relocate), `native/src/game/
batch_0x405B3A.cpp` (`sub_405D0C`, the function called from the enclosure
arm branch — see Finding 0), `docs/re/enclosure.md` (full), `docs/re/facts.md`
(the enclosure/HURRY sections, `docs/valuelst-map.md` ids 27/28/46/101/30).

---

## Finding 0 — `docs/re/enclosure.md` misidentifies `sub_405D0C`: it does NOT clear warpholes/trampolines, and the port is right to do nothing there

**Original**: `sub_426818`'s arm branch (`native/src/game/batch_0x42583B.cpp`
lines 690-696):

```c
if ( !dword_45BE9C )
{
  dword_462244 = rand_() % 3;
  dword_45BE9C = 1;
  dword_46223C = sub_43ACF8();
  result = sub_405D0C();
}
```

`docs/re/enclosure.md` §2 (line 57) glosses this call as `// clear
warpholes+trampolines from the actor grid`, and its §9 addresses table
(line 445) repeats: `sub_405D0C | clear warpholes(type<=1)+trampolines(type
3) from actor grid on arm`. That gloss traces back to an early cross-batch
forward-declaration guess (`batch_0x42583B.cpp` line 109: `int sub_405D0C();
// enclosure start notify/sound` — note even that guess disagrees with the
later "clear warpholes" gloss already).

**The actual function**, transliterated later and more carefully from the
real disassembly in `native/src/game/batch_0x405B3A.cpp` (lines 298-325),
does something completely different:

```c
int sub_405D0C()
{
  int result;
  unsigned int v1;
  int v2;
  _DWORD *v3;

  v2 = 0;
  result = dword_45E0A8;
  v3 = (_DWORD *)dword_45E0A8;
  while ( v2 < 100 )
  {
    if ( *v3 )
    {
      v1 = v3[1];
      if ( v1 )
      {
        if ( v1 <= 1 || v1 == 3 )
          *v3 = 0;
      }
    }
    ++v2;
    result = (int)v3;
    v3 += 38;
  }
  return result;
}
```

`dword_45E0A8` is a **100-slot "broadcast/session record" table** used by the
**LEVEL SELECT / setup-screen** UI (`batch_0x405B3A.cpp`'s own header
comment, lines 1-10, and its manifest note at lines 36-42) — a 152-byte
stride array that superficially resembles the 10-player `dword_461BC4`
record layout (same early-field offsets) but is a **separate heap
allocation entirely unrelated to gameplay actors, warpholes, or
trampolines**. `batch_0x405B3A.cpp` itself flags the discrepancy explicitly
in its own manifest comment (lines 39-42): *"A prior batch's cross-decl
guessed `sub_405D0C` was 'enclosure start notify/sound'; the real body
ported below (iterate 100 slots, clear ones whose state field is 1 or 3)
does not match that guess — flagged in the manifest."*

Net: calling `sub_405D0C()` from inside a live match's enclosure-arm branch
touches a lobby-only data structure that gameplay never reads once the
match starts. It is (at most) an inert leftover call in the original, not a
"clear warpholes+trampolines" gameplay effect. **There is no such side
effect to port.**

**Port**: `libs/sim/src/systems/enclosure.cpp`'s arm branch (`update()`,
lines 273-279) sets `enclose_interval`/`enclose_timer`/`enclose_index` and
returns — it does not touch `s.actor_type`/`s.actor_dir` (the warphole/
trampoline grid) at all. Grepping the whole `libs/` tree for `405D0C` or any
"clear warpholes on arm" logic turns up nothing — this was simply never
implemented.

**Visible effect**: none — the port's omission accidentally matches the
original's real behaviour. The risk is purely to future work: a reader who
trusts `docs/re/enclosure.md`'s current text (as this audit's own reference
list initially did, before tracing the callee) could "fix" this port by
adding a warphole/trampoline-clear-on-arm feature that has no basis in the
real game and would be a wholly new, undocumented gameplay change.

**Severity**: Documentation (would become real severity if acted on).
**Confidence**: High — direct disassembly-derived transliteration with an
explicit self-flagged correction, not an inference.

**Suggested fix**: update `docs/re/enclosure.md` §2 (line 57's inline
comment) and §9 (line 445's table row) to drop the "clear warpholes+
trampolines" claim and cite `sub_405D0C`'s real body (LEVEL-SELECT
broadcast-table cleanup, inert during an active match) instead, citing
`native/src/game/batch_0x405B3A.cpp` lines 298-325 and its own manifest
note. No `libs/sim` change needed.

---

## Finding 1 — `wall_detonates` = ON crushes only the FIRST grounded bomb found per drop event; the port crushes every bomb on that tile

**Original**: `sub_426818`'s bomb-crush loop (`native/src/game/batch_0x42583B.cpp`
lines 769-820):

```c
v19 = 100;
while ( 1 )
{
  v6 = (_DWORD *)sub_422E48(dword_462230, dword_462234);
  if ( v6 )
  {
    if ( dword_464940 )                    // wall_detonates ON
    {
      sub_423209((int)v6, -1);             // queue THIS bomb
LABEL_47:
      v20 = 100;
      do {                                  // flame-cell cleanup (unconditional)
        v5 = (_DWORD *)sub_42708D(dword_462230, dword_462234);
        if ( v5 ) sub_427115(v5);
        --v20;
      } while ( v5 && v20 > 0 );
      ...compute next spiral position...
      goto LABEL_26;                        // <-- next DROP EVENT, not a retry
    }
    sub_424841(v6);                         // wall_detonates OFF: eat this bomb
  }
  --v19;
  if ( !v6 || v19 <= 0 )
    goto LABEL_47;                          // OFF path loops back to re-search
}
```

When `dword_464940` (`wall_detonates`, VALUELST id 46) is **ON**, finding a
bomb immediately queues it via `sub_423209` and `goto LABEL_47` **unconditionally**
— there is no path back to the top of the `while(1)` loop in that branch, so
the search runs **at most once** per drop event. A second bomb occupying the
exact same crushed tile (in principle reachable — `sub_422E48` is a linear
scan over a 100-slot bomb array by cell coordinate, not a 1:1 per-cell grid
like the flame/powerup state, so nothing structurally prevents two entries
matching the same `(x,y)`) is **not found or detonated by this event** —
only the flame-cell cleanup and spiral advance still run.

When `wall_detonates` is **OFF**, by contrast, `sub_424841` (silent eat)
does *not* jump to `LABEL_47`; the loop falls through to `--v19` and
re-enters the `while(1)`, re-querying `sub_422E48` at the same `(x,y)` — so
the OFF path *does* keep eating bombs at that tile until none remain (up to
100 tries). This asymmetry — ON stops after one hit, OFF exhausts all hits
— is intentional-looking in the disassembly (the `goto LABEL_47` is baked
into the ON branch's very next instruction after `sub_423209`), not an
artifact of the transliteration.

**Port**: `libs/sim/src/systems/enclosure.cpp`, `EnclosureSystem::drop_wall`
(lines 136-163):

```cpp
for (std::size_t bi = 0; bi < s.bombs.size(); ++bi) {
    Bomb& b = s.bombs[bi];
    if (!b.active || b.flying || b.tile_x() != wx || b.tile_y() != wy) continue;
    if (s.tuning.wall_detonates) {
        b.fuse = 1;                         // queues EVERY matching bomb
    } else {
        b.active = false;
        if (s.players[b.owner].bombs_placed > 0) --s.players[b.owner].bombs_placed;
    }
}
```

This `for` loop has no `break`, so with `wall_detonates` ON it sets
`fuse = 1` on **every** bomb at `(wx, wy)`, not just the first. The OFF
branch's "eat every bomb at the tile" behaviour is correctly unconditional
already (matches the original's own retry-to-exhaustion for that case) —
only the ON branch's "stop after the first" quirk is missing.

**Visible effect**: only observable when two or more grounded (non-flying)
bombs occupy the exact same tile at the instant a closing wall lands on it,
with "Stomped Bombs Detonate" ON. Under normal placement rules a tile can't
receive a second bomb while one is already there, so this needs some other
mechanism (e.g. a kicked/slid bomb coming to rest on a tile another bomb
already occupies) to produce the state at all — not verified reachable in
this port's current `bombs.cpp` collision rules, hence the Medium
confidence on real-game impact despite the High confidence on the literal
disassembly reading.

**Severity**: Low. **Confidence**: High (arithmetic/control-flow), Medium
(reachability).

**Suggested fix**: if confirmed reachable (or out of caution), change the
`wall_detonates` ON branch to `b.fuse = 1; break;` — detonate the first
matching bomb found (by `s.bombs` iteration order) and stop scanning that
tile for the rest of this drop event, matching `LABEL_47`'s unconditional
jump. Leave the OFF branch's loop-to-exhaustion as is. Add a doctest that
places two grounded bombs on one tile (however that state is constructed)
and asserts only one detonates when a wall crushes it with the option ON,
both are eaten when OFF.

---

## Verified faithful (no change) — re-confirmed from the native transliteration, not just from re-reading the prior docs

- **Top-level gate** (`sub_421969() > 1`) is a general match-active check
  with no enclosure-specific round-end logic — `docs/re/enclosure.md` §8,
  re-confirmed directly in `sub_426818`'s first lines
  (`batch_0x42583B.cpp` 678-679). This port has no "screens" concept in
  `libs/sim`, so nothing to gate — already correct by construction.
- **Arm/disarm trigger arithmetic** (`v1 = sub_412135(101); result =
  sub_410578(); if (result <= v1 - 5) {...} else if (dword_45BE9C) {...}`,
  `batch_0x42583B.cpp` 686-705) matches `EnclosureSystem::update()`'s
  `closing = seconds_left <= s.tuning.hurry_seconds - 5` exactly, including
  the non-strict `<=`. The disarm branch's reset values (`x=0,y=0,depth=0,
  dir=1`) are unreachable in a real match (clock only counts down) and the
  port has no disarm path either — consistent, not a gap (both never
  exercise it).
- **The arm ALWAYS fires regardless of the depth setting** (`dword_45BE9C=1`
  etc. is unconditional on the time predicate, independent of
  `dword_464974`/depth) — the port's arm branch (`enclose_interval==0`) is
  likewise unconditional on `depth`, only the subsequent per-event drop
  gate checks `depth > 0` / `total(depth)`. Matches.
- **250 ms / 5-tick cadence, drop-before-advance ordering, sound-before-
  solidify ordering** (`dword_46223C += 250; sub_4278F2(...); sub_425E9B(...)`,
  `batch_0x42583B.cpp` 748-750) — `docs/re/enclosure.md` §3, re-confirmed
  line for line against `kEncloseIntervalTicks = 5` and `drop_wall`'s own
  SFX-then-solidify event ordering (the port emits one `WallClosed` event
  per drop, consumed unconditionally by `SoundDirector`, per
  `docs/re/facts.md`'s "Wall-slam SFX" entry).
- **Spiral advance state machine** (accept-vs-turn-vs-ring-complete,
  `batch_0x42583B.cpp` 786-813, LABEL_47/60/61) matches
  `enclosure.cpp`'s `spiral()` generator lambda field-for-field: same
  bounds check (`width-depth > nx && height-depth > ny && nx>=depth &&
  ny>=depth`), same clockwise turn (`dir=(dir+1)&3`), same ring-complete
  stop check position (evaluated only when `dir` wraps back to 1). The
  phantom-corner-repeat and free-duplicate-start-tile-revisit behaviours
  both fall out of this shared logic, as `docs/re/enclosure.md` §4 already
  derives.
- **Ring-count ambiguity** (`2*getvalue(27) <= depth` literal reading vs.
  VALUELST 27's authored "2× depth" comment) — re-derived independently
  from the same disassembly line (`batch_0x42583B.cpp` 725-726/801-804);
  agrees with `docs/re/enclosure.md` §4's conflict writeup and the port's
  documented choice to keep the comment-and-golden-corroborated rule. Not
  re-litigated here.
- **Player-crush order and exemptions**: `sub_421D3F`'s own search
  predicate (`present && !dead && type != 4 && tile match`,
  `batch_0x420D4E.cpp` lines 682-697) already excludes type-4
  (network-spectator) internally — the caller's redundant `*((_BYTE*)v8+16)
  != 4` re-check in `sub_426818` (line 755) is dead code given the finder's
  own filter, not a second, different check; no double-standard to port.
  `sub_41DE63`'s bounce/warp early-out (movement-state 5/6/7,
  `batch_0x41DAA7.cpp` lines 248-255) is correctly mirrored by
  `drop_wall`'s `p.bounce == 0 && p.warp == 0` guard.
- **Powerup destroy**: `sub_4254F3` (`batch_0x42459A.cpp` lines 693-703) is
  confirmed to be an unconditional `*a1 = 0` with no skull-relocation
  compensation (unlike `sub_425107`'s reveal-time relocation logic, a
  different function entirely) — matches `docs/re/enclosure.md` §5 point 2
  and the port's unconditional `s.hidden[...] = s.floor[...] =
  PowerupType::None`. The powerup-cell grid (`dword_462214`) is a single
  combined hidden/floor record (state 1 = hidden-under-brick, state 2 =
  revealed/floor, per `sub_425107` lines 638-642), so one destroy call
  covers both — consistent with the port clearing both `hidden` and
  `floor` arrays in one step.
- **Grounded-bomb definition**: `sub_422E48` excludes motion states 2/3
  (flying/carried). This port has no "carried" bomb representation in
  `s.bombs` at all — a caught/carried bomb lives entirely in
  `Player::carrying`/`carried_owner`, never as a `Bomb` entry — so
  `!b.flying` alone is the complete equivalent of excluding both states 2
  and 3 (state 3 structurally can't appear in `s.bombs`). Architecture
  note, not a gap.
- **Deferred one-tick wall-detonation** (`sub_423209` queue,
  `sub_42331C`'s once-per-frame drain gated before `sub_426818`) —
  `docs/re/enclosure.md` §6 and `docs/re/facts.md`'s 2026-07-10 entry #3,
  re-confirmed against `simulation.cpp`'s tick order (step 6 `tick_fuses()`
  runs before step 8 `enclosure.update()`, reproducing "drain runs before
  enclosure this frame, so a wall-queued bomb detonates next tick") — the
  known dud-bomb gap noted in both docs is unchanged and not re-litigated
  here.
- **Flame-cell cleanup runs once per drop event, unconditionally** (the
  `sub_42708D`/`sub_427115` retry loop naturally terminates after one hit
  because the flame-cell grid is 1:1 per-cell, same as the powerup grid) —
  matches `drop_wall`'s unconditional `s.flame[wy][wx] = s.burning[wy][wx]
  = 0`.
- **Zero RNG draws in the sim**: the only `rand_()` in `sub_426818` is the
  presentation-only drop-sound variant pick at arm time
  (`dword_462244 = rand_() % 3`, line 692) — `enclosure.cpp` draws no
  `State::rng` anywhere, confirmed by inspection (no `random_below`/`rng`
  reference in the file). Matches `docs/re/enclosure.md` §7 and
  `docs/re/facts.md`'s "Confirmed unchanged" note.
- **Cosmetic preview overlay** (the `10*getvalue(910)+100` fading-highlight
  walk, `batch_0x42583B.cpp` lines 706-738) mutates no gameplay state and
  draws no RNG — confirmed absent from the port, matching
  `docs/re/enclosure.md` §7's explicit "not ported... none is added here"
  note. Not re-flagged.
- **Random powerup drop** (`sub_426704`, called from the very top of
  `sub_426818` at line 682) is a wholly separate mechanic (periodic random
  powerup spawn on brick tiles, gated on a different VALUELST id per
  scheme/tileset) sharing the same per-frame call site as the enclosure
  stepper in the original, not part of the enclosure system itself — out
  of scope here, not evaluated.
