# Fidelity audit — `flames.cpp` (system 3, W1)

**Verdict: mostly faithful — one real, previously-undocumented RNG-ORDER
divergence (arm iteration order) and one narrow RNG-draw gap (relocate on a
re-hit-after-reveal brick), both scoped to the RNG stream rather than to
what ignites/spreads/stops. The core arm-walk logic (bomb/powerup/solid/
brick stop order, tip/mid selection, chain-queue transfer, brick crumble
timing, owner-vs-colour split) is already faithful — verified, not
re-fixed — thanks to the five prior flame-system audit passes cited
throughout `flames.cpp`'s own comments (`docs/re/facts.md` "Chain-reaction
timing", "Brick crumble timing", "Overpowered-powerup relocation", "Flame
arm-shape (tip/mid/center) selection", "Bomb/flame colour is not the
owner").**

Scope: `libs/sim/src/systems/flames.{hpp,cpp}` only. Renderer-layer
consumers of this system's data (`libs/game/src/renderer.cpp`) are system
#9 in `docs/re/fidelity-audit.md`'s ledger and out of scope here except
where noted as a data-gap this system would need to close first.

References read in full: `native/src/game/batch_0x422DDD.cpp` (`sub_42331C`
the bomb/flame state machine, `sub_423209` the chain queue), 
`native/src/game/batch_0x426C4C.cpp` (`sub_426D06` flame/burn age+draw,
`sub_426FCC` flame-cell ignite), `native/src/game/batch_0x42459A.cpp`
(`sub_425107` relocate/reveal, `sub_42542D`/`sub_4254F3` powerup query/
clear, `sub_4245DA` bomb-capacity scan), `native/src/game/batch_0x42583B.cpp`
(`sub_425FB9`/`sub_425E36`/`sub_425E9B`/`sub_425EFC` tile-state grid).
`docs/re/facts.md`'s five prior flame-system entries (full text, not
excerpts).

---

## Finding 1 — Explosion arm direction iteration order does not match the original's ascending godir order

**Original**: `sub_42331C`'s explosion block, `native/src/game/batch_0x422DDD.cpp`
lines 815-881 (pseudo.c ~25617-25678):

```c
for ( k = 0; k < 4; ++k )
{
  if ( !*(_BYTE *)(v75 + 56) || k + 1 != *(unsigned __int8 *)(v75 + 56) )
  {
    v54 = sub_42665C(*(_DWORD *)(v75 + 28));   // epicentre x
    v53 = sub_4266A3(*(_DWORD *)(v75 + 32));   // epicentre y
    sub_426FCC(v54, v53, ..., 8, ...);          // epicentre re-ignite
    ...powerup destroy on epicentre...
    for ( m = 0; *(unsigned __int8 *)(v75 + 76) > m; ++m )
    {
      v54 += dword_45BECC[k];
      v53 += dword_45BEDC[k];
      ...bomb/powerup/solid/brick/blank arm-tile logic...
    }
  }
}
```

`k` is used directly as the index into `dword_45BECC`/`dword_45BEDC`
(confirmed values `{0,1,0,-1}`/`{-1,0,1,0}`, `docs/re/facts.md` lines
222-223) — i.e. `k` **is** the godir value, and the loop visits directions
in ascending order **0,1,2,3 = Up, Right, Down, Left**.

**Port**: `libs/sim/src/systems/flames.cpp` lines 240-250 (`FlameSystem::explode`):

```cpp
for (Direction d : {Direction::Up, Direction::Down, Direction::Left, Direction::Right}) {
    if (skip_dir >= 0 && grid::to_godir(d) == skip_dir) continue;
    for (int i = 1; i <= reach; ++i) {
        if (!spread_to(cx + grid::dir_dx(d) * i, cy + grid::dir_dy(d) * i, b.owner, b.colour,
                       d, i == reach))
            break;
    }
}
```

`Direction` (`libs/sim/include/bomber/sim/types.hpp` line 34) is declared
`{Up, Down, Left, Right}` in **enum-declaration order**, unrelated to godir.
`grid::to_godir` (`libs/sim/src/grid.hpp` lines 26-34) maps
Up→0, Right→1, Down→2, Left→3. The braced-init list therefore visits
directions in godir order **0, 2, 3, 1 = Up, Down, Left, Right** — a
different permutation of the same four directions from the original's
0,1,2,3.

**Visible effect**: `explode`'s four arms are otherwise independent (each
walks its own tiles, no cross-arm interaction), so the different visiting
order is inert **except** where a tile visited mid-arm triggers an RNG
draw: `FlameSystem::relocate_overpowered_here` (the 200-try swap/move search
on an overpowered hidden powerup under a freshly-ignited brick) and
`FlameSystem::burn_powerup_here` → `PowerupSystem::scatter` (the
replacement-skull placement search when a Disease token burns with
`diseases_destroyable` off) both draw from `State::rng`. Whenever a single
explosion's reach causes **two or more** such RNG-drawing events across
**different directions** (e.g. an explosion with reach ≥2 that clips an
overpowered-hidden brick to its north and a floor Disease token to its
east), the original draws those RNGs in the order Up→Right→Down→Left while
the port draws them in the order Up→Down→Left→Right — the SAME draws
happen, but interleaved differently, so every RNG value from that tick
onward diverges from the original's stream for the rest of the match
(placement rolls, AI decisions, future scatters, everything downstream of
`State::rng`). This is silent: no assertion catches it, no golden scenario
currently triggers it (per `tests/test_golden.cpp`'s existing coverage),
and the visible symptom would only surface as "the game feels like it
rolled different powerups/AI moves than a from-scratch replay would" in a
scenario with dense bricks/disease tokens around a high-reach bomb.

**Severity**: High for the determinism contract (CLAUDE.md rule 2: "The
ORDER and COUNT of RNG draws per tick is part of the contract") — this is
exactly the class of bug that contract exists to prevent — but narrow in
practical trigger frequency (needs ≥2 RNG-drawing arm events in one
explosion).

**Confidence**: High. `dword_45BECC`/`dword_45BEDC`'s godir-ascending
indexing is independently confirmed in `docs/re/facts.md` (lines 222-223,
and reused identically by `movement.cpp`, `rovers.cpp`, `ai.cpp` — see
their own "godir order 0=Up,1=Right,2=Down,3=Left" comments); the port's
`Direction` enum order and `explode`'s braced list are read directly from
source, not inferred.

**Suggested fix**: Iterate in godir order explicitly, e.g.
`for (int g = 0; g < 4; ++g) { Direction d = grid::from_godir(g); ... }`
(mirrors the ascending-`k` loop exactly) instead of the enum-declaration-
order braced list. A regression test would need a scenario with a
reach-2+ bomb positioned to hit an overpowered-hidden brick in one
direction and a Disease token in another within the same explosion, then
assert the post-explosion RNG state / subsequent scatter placement matches
a hand-computed Up→Right→Down→Left draw order.

---

## Finding 2 — RESOLVED 2026-07-20: NOT A BUG (audit false positive — port is already faithful)

**Verdict: the fix suggested below was NOT applied. On closer reading of the
flame ARM (which this finding analysed only through `sub_425107` in isolation),
a re-hit of an already-revealed over-powerful token NEVER reaches `sub_425107`
a second time, so the original does not re-relocate it — it destroys it —
exactly as the port already does.**

The gap in the original analysis: the arm's per-tile check order in
`sub_42331C` (`batch_0x422DDD.cpp:837-872`) is bomb → **VISIBLE powerup** →
solid → brick. The visible-powerup branch at lines 849-861 —
`v49 = sub_42542D(x,y); if (v49 && *v49 == 2) { …destroy…; break; }` — fires on
a token whose state byte is 2 (revealed) REGARDLESS of the cell still being a
crumbling brick (`sub_42542D` returns the record for any non-empty state and
does not consult the cell type). It `break`s the arm BEFORE the brick branch at
line 865 that calls `sub_425107`. So once the FIRST hit has revealed the token
(`sub_425107`'s `LABEL_33` flips state 1→2), a SECOND arm reaching the same
still-crumbling tile hits line 850, burns the now-visible token, and stops —
`sub_425107` is never entered again, and its kind-based relocate gate never
gets the chance to re-fire. (This is exactly the "visible floor powerup
(destroy, no ignite)" arm-stop this same audit lists under "Verified faithful"
— Finding 2 contradicted it.)

The port mirrors this precisely: `FlameSystem::spread_to` checks
`s.floor[ty][tx] != None` (flames.cpp:103) — the array the first reveal moved
the token into — and `burn_powerup_here` + `return false` BEFORE the brick
branch that calls `relocate_overpowered_here`. So a re-hit of a revealed token
burns it and stops in the port too. Both sides agree: no relocate on re-hit,
no RNG drawn, token destroyed. **No code change; no golden move.**

Separately noted while confirming the draw picture (OUT OF SCOPE, pre-existing,
not introduced here): `sub_425107` opens with an UNCONDITIONAL
`if (!(rand_() % 30)) sub_42BE0B();` cure roll (`batch_0x42459A.cpp:587`,
pseudo.c 26290) on EVERY brick reveal. The port's brick-reveal path draws no
such value — a systematic RNG-count omission on every brick ignite, orthogonal
to this finding and to batch 2. Flagged for a future batch (fixing it re-shifts
every golden with bricks).

---

## Finding 2 (ORIGINAL, superseded by the resolution above) — `relocate_overpowered_here` cannot re-trigger on a brick re-hit after its powerup has already revealed

**Original**: `sub_425107`, `native/src/game/batch_0x42459A.cpp` lines
571-644 (pseudo.c 26274-26343). The relocate gate reads only the powerup
record's **kind** field, never its **state** byte:

```c
v12 = (_DWORD *)(...);              // this cell's powerup record (state @+0, kind @+4)
if ( !sub_40C06A() )
{
  v3 = sub_4105B0(); v2 = sub_412135(102);
  if ( v3 < v2 )                    // within overpowered_relocate_seconds
  {
    v5 = v12[1];                    // KIND — read unconditionally, regardless of v12[0] (state)
    if ( v5 >= 5 && (v5 <= 6 || v5 == 11) )
    { /* pass 1: 200-try swap; pass 2: 200-try move */ }
  }
}
LABEL_33:
if ( *v12 == 1 )                    // reveal only gates on STATE == 1 (hidden)
{ v12[16] = dword_464994; *v12 = 2; }
```

Because `sub_425EFC`'s cell-type flip is a same-call no-op (already
established, "Brick crumble timing"), a brick stays `Cell::Brick` for the
whole `brick_burn_frames` window — so a **second** flame arm reaching the
SAME tile before the crumble finishes re-enters this same brick branch and
calls `sub_425107` again. If the FIRST hit already revealed the token
(`*v12` flipped 1→2), this SECOND call's relocate gate still fires purely
off `v12[1]` (the kind, untouched by the reveal) — it does not check
`*v12` at all. Pass 1's swap is a raw 152-byte struct copy
(`qmemcpy(v4,v12,152); qmemcpy(v12,v9,0x98); qmemcpy(v9,v4,0x98)`) that
carries the STATE byte along with the kind, so an already-visible
overpowered token can be swapped away (potentially replaced by whatever
the candidate held, in whatever state — visible or hidden — the candidate
was in) even after it has already been shown to players.

**Port**: `libs/sim/src/systems/flames.cpp` lines 171-226
(`FlameSystem::relocate_overpowered_here`):

```cpp
void FlameSystem::relocate_overpowered_here(int tx, int ty) {
    State& s = s_;
    const PowerupType kind = s.hidden[ty][tx];
    if (kind != PowerupType::Punch && kind != PowerupType::Grab &&
        kind != PowerupType::SuperDisease)
        return;
    ...
}
```

The port reads `s.hidden[ty][tx]` — which the FIRST reveal already cleared
to `PowerupType::None` (`spread_to`, `s.hidden[ty][tx] = PowerupType::None;`
right after copying it into `s.floor[ty][tx]`, `flames.cpp` line ~135).
Since the port has no single "record" that carries kind+state together
(kind lives in `s.hidden` OR `s.floor`, mutually exclusively, rather than
one struct with a state byte), a re-hit after the reveal sees
`s.hidden[ty][tx] == None` and returns immediately — the relocate gate can
only ever fire on the FIRST hit.

**Visible effect**: on a re-hit of a still-crumbling brick (within the
same ~`brick_burn_frames`-tick / 0.5s window at 20 Hz, and within
`overpowered_relocate_seconds` of match start) whose overpowered token
already revealed on the first hit, the original still attempts (and, given
a board with standing bricks, usually succeeds at) a 200-try relocation
search — drawing RNG and potentially relocating/hiding the already-visible
token elsewhere or swapping it for a different kind. The port silently
no-ops: the token stays put, unchanged, and no RNG is drawn. Both the
draw COUNT (zero vs. up to 200 pairs) and the eventual world state
(token stays vs. potentially moves) diverge whenever this narrow window is
hit — most plausible with two overlapping/near-simultaneous explosions (or
a chain reaction) both reaching the same overpowered-hidden brick within
the crumble window.

**Severity**: Medium. Narrow trigger window (requires a specific overlap
of two explosions within ~0.5s on the same brick, inside the opening
relocate-seconds of the match) and the original's own resulting behaviour
in this corner (silently un-hiding then re-hiding/moving an already-visible
"over-powerful" token) is already an obscure, likely-unintended interaction
in the source game — but it is a genuine RNG-count/order divergence when it
does occur, same class of bug as Finding 1.

**Confidence**: Medium-high. The pseudo.c-level gate logic is
unambiguous (kind-only gate, state-only reveal, independently confirmed by
the already-existing "Overpowered-powerup relocation" facts.md entry's own
full quote of this function); the practical reachability assessment (two
explosions overlapping one brick within the crumble window) is a reasonable
inference from the already-confirmed "Brick crumble timing" entry rather
than a directly observed repro.

**Suggested fix**: Track the powerup "kind regardless of reveal state" per
tile (e.g. read `s.hidden[ty][tx] != None ? s.hidden[ty][tx] :
s.floor[ty][tx]`, since the original's shared-struct semantics mean a
tile can only ever hold one powerup record whether hidden or floor) and,
if that kind is overpowered and within the deadline, run the same
swap/move search — swapping between `hidden`/`floor` state boundaries
faithfully (candidate's hidden-vs-floor state moves to this tile, this
tile's PRIOR hidden-vs-floor state moves to the candidate) rather than
assuming both sides are always "hidden". Given the narrowness, this is
lower priority than Finding 1.

---

## Informational (out of scope here, flagged for the renderer audit, ledger item #9)

**Brick-burn cells carry no colour in `State`.** `sub_426FCC`'s brick-branch
call (`native/src/game/batch_0x422DDD.cpp` line 867,
`sub_426FCC(v54, v53, *(_BYTE*)(v75+60), 9, -1)`) still passes the
exploding bomb's **colour** byte (owner is explicitly `-1`/unowned, which
is inert — nothing ever reads a kind-9 cell's owner, since a player can
never physically stand on a still-`Brick` tile and the two bomb-related
kind-9 exemptions already skip it, "Chain-reaction timing"). `sub_426D06`'s
kind-9 draw branch (`native/src/game/batch_0x426C4C.cpp` line 267,
`v11 = *(unsigned __int8*)(v16+60)`) reads that colour byte for the blit —
so brick-burn animations ARE tinted with the exploding bomb's colour in the
original, same as real flame. `State::burning` (`state.hpp` line 104) is a
bare `uint8_t` countdown with no paired colour array (unlike `flame`'s
`flame_owner`/`flame_colour` pair), so this system currently has nowhere to
store that colour for a future renderer fix to read. Not a `flames.cpp`
bug per se (nothing observable changes within this system), but the data
gap lives here — noted for whoever picks up ledger item #9 (`renderer.cpp`).

Also informational: `dword_4642B8` ("bricks-destroyed-by-flame counter",
incremented at `native/src/game/batch_0x422DDD.cpp` line 868 right beside
the brick ignite) feeds the persistent cross-match `bmstats.dat` "Bricks
Destroyed" counter (`docs/re/facts.md` lines 3396-3416,
`sub_40200C`/`0x46429C`) — an entirely separate, currently-unported
meta-system (lifetime stats file, not per-match sim state), not a
`flames.cpp` gap.

---

## Verified faithful (no change) — re-confirmed by this pass

- **Arm-stop order and effect**: bomb (transfer owner + chain-queue, no
  ignite) → visible floor powerup (destroy, no ignite) → solid (stop, no
  ignite) → brick (ignite kind 9, stop) → blank (ignite tip/mid, continue).
  Matches `sub_42331C`'s per-tile checks in `spread_to`
  (`flames.cpp` lines 67-156) exactly, including that a HIDDEN powerup
  never appears on open floor (only `s.floor`/state==2 stops the arm, never
  `s.hidden`/state==1) and that players never stop or are checked by the
  arm at all (death is a separate per-tick lookup, not an arm-stop).
- **Epicentre re-ignite is called once per non-skipped direction in the
  original (up to 4x per explosion) but is a fully idempotent overwrite**
  (same tile, same final `flame`/`flame_owner`/`flame_colour`/kind=Center
  values every time, and the powerup-destroy is a no-op after the first
  successful destroy) — confirmed by direct read of
  `native/src/game/batch_0x422DDD.cpp` lines 819-832 (inside the `for
  (k=0;k<4;++k)` guard). The port's single unconditional
  `ignite_epicentre` call before the direction loop (`flames.cpp` line 239)
  produces the identical end state; not a bug despite looking structurally
  different from the original's per-iteration call site.
- **Chain-queue transfer & skip-dir encoding** (owner-transfer-before-queue,
  `bombs_placed` slot migration, opposite-direction skip on the chained
  bomb's own blast, deferred one-tick detonation): re-read against
  `sub_423209`/`sub_42331C`'s drain guard and the arm's bomb-hit branch —
  matches the existing "Chain-reaction timing" and "Bomb capacity is a
  derived live-bomb count" facts.md entries exactly; the queue-drain's
  ASCENDING-BOMB-VECTOR-INDEX order (not push order, not bit-identical to
  the original's slot-reuse array) remains a documented, accepted deviation
  — cited in `flames.cpp`'s own `drain_chain_queue` comment.
- **Tip vs. mid selection**: `is_last_of_reach` (`i == reach`, 1-based) is
  the same test as the original's `m == reach-1` (0-based) — re-verified
  against `native/src/game/batch_0x422DDD.cpp` lines 873-877.
- **Brick crumble timing**: cell stays `Cell::Brick` for the whole
  `brick_burn_frames` window (`sub_425EFC`'s revert-in-the-same-call is a
  true no-op, confirmed again directly in
  `native/src/game/batch_0x42583B.cpp` lines 385-407); the hidden powerup
  reveals immediately at ignition, well before the tile opens
  (`sub_425107`'s unconditional `LABEL_33` tail); a re-hit mid-crumble
  re-enters the brick branch and resets the timer (re-confirmed: cell type
  is still `Brick` at the re-hit, so `spread_to` naturally re-enters that
  branch — no separate "already burning" guard exists in either the
  original or the port).
- **`relocate_overpowered_here` pass 1 / pass 2 mechanics** (kind-only gate
  vs. `getvalue(102)`-second deadline, x-then-y draw order even on a
  rejected candidate, 200-try caps per pass, full-record swap on pass 1 vs.
  plain move on pass 2, "both exhausted → leave as-is" fallthrough): all
  re-verified line-by-line against `sub_425107`
  (`native/src/game/batch_0x42459A.cpp` lines 585-643) and match — see
  Finding 2 above for the one gap found (re-hit after reveal).
- **Flame owner vs. colour split**: `flame_owner` transfers on chain,
  `flame_colour` never does — re-verified against `sub_426FCC`'s two
  separate parameters (`a3`=colour byte, `a5`=owner hiword,
  `native/src/game/batch_0x426C4C.cpp` lines 284-310) and the arm-hit
  transfer's single `+62`-word write (`native/src/game/batch_0x422DDD.cpp`
  line 840, `*(_WORD*)(v48+62) = *(_WORD*)(v75+62)` — the colour byte at
  `+60` is untouched). Matches the existing "Bomb/flame colour is not the
  owner" facts.md entry and `flames.cpp`'s `Bomb::colour`/`flame_colour`
  fields.
- **`age_flames_and_bricks`**: per-cell independent decrement (no RNG, no
  cross-cell interaction), cell-type flip guarded on still reading `Brick`
  — matches `sub_426D06`'s `if (sub_425FB9(j,i)==2) sub_425E9B(j,i,0)`
  guard (`native/src/game/batch_0x426C4C.cpp` line 210-211) and the
  already-confirmed "Flame/burn frame pacing" timing (both real-flame and
  brick-burn expire on `elapsed_ms > tuning_frames * 50ms`, read via the
  original's `*(int*)(cell+66)>>16` idiom — a decompiler-visible but
  behaviourally-identical way of reading the WORD at `cell+68`, the same
  field the top-of-loop `+= dword_464958` accumulates into; not a distinct
  field, not a bug).
- **`burn_powerup_here`'s Disease-relocation compensation** (skull
  destruction is unconditional; the replacement scatter only fires when
  `diseases_destroyable` is off, reusing `PowerupSystem::scatter` =
  `sub_4255B2`): re-verified against both call sites
  (`native/src/game/batch_0x422DDD.cpp` lines 822-831 epicentre,
  849-861 arm) — matches `flames.cpp` lines 11-27 exactly, including that
  the RNG draw only happens on the actual destroy, not on a redundant
  re-check.
