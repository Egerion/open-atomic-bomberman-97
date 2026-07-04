# Stage actors — dirarrows, warpholes, conveyors, trampolines

RE of the "extra" stage-actor layer in BM95.EXE (Watcom, imagebase 0x400000).
Facts distilled here per the RE workflow; `facts.md` is owned by another agent
this round, so stage-actor facts live in this file. Roadmap item #7.

Covers: the actor registry, the `EXTRA<N>.RES` data source (NOT the .SCH grid),
and the conveyor / trampoline / dirarrow / warphole mechanics.

## 1. Actor registry (`dword_45E0A8`)

- Allocated by `sub_404D16`: `sub_418511(152, 100)` = 100 entries of 152 bytes.
- Cleared by `sub_404D53` (memset each of the 100 slots).
- A free slot is claimed by `sub_404E3C`: returns `152*n + base`, bumps the
  used count `dword_45E0B0`, memsets the slot. Max 100 actors per stage.
- Tile→actor lookup `sub_405654(x, y)`: linear scan, stride 38 dwords (=152
  bytes); returns the first slot whose `[0]` (active) is set and whose
  `[7]==x && [8]==y` (actor+28 = tileX, actor+32 = tileY). Returns 0 if none.

### Actor struct layout (the fields the mechanics read)

| offset | as        | meaning                                             |
|-------:|-----------|-----------------------------------------------------|
| +0     | dword     | active flag (1 = live)                              |
| +4     | dword     | **type: 0=DIRARROW, 1=WARPHOLE, 2=CONVEYOR, 3=TRAMPOLINE** |
| +28    | dword     | tile X (`[7]`)                                      |
| +32    | dword     | tile Y (`[8]`)                                      |
| +42    | word hi   | (conveyor) opposite-direction compare field         |
| +44    | word (`[22]`) | godir / direction (0=N/Up,1=E/Right,2=S/Down,3=W/Left); for warphole = **idno** |
| +46    | word      | (warphole) **linkto** id                            |
| +48    | word (`[24]`) | (trampoline) "triggered" flag → drives its bounce anim |
| +52    | word      | (warphole) parsed arg0 "type" (unused by movement)  |
| +146   | byte      | (warphole) one-shot "already handled" latch         |

Direction letters decode via `sub_404DB8` (tolower): `n→0, e→1, s→2, w→3`,
i.e. exactly **GODIR order (Up=0, Right=1, Down=2, Left=3)**. Unknown → -1.

## 2. DATA SOURCE — `EXTRA<N>.RES`, not the .SCH grid

`sub_404E99` (the parser) opens **`extra%u.res`** (format string `aExtraURes`,
`%u` = `dword_46499C`, the current level/board id) and reads it **line by
line as a text file**. This is the definitive finding: **special-tile actor
placement is NOT encoded in the .SCH scheme grid.** The .SCH grid only carries
wall/brick/blank + player spawns (as our `assets::sch` parser already models).
`sub_40551F` chooses the source: editor mode (`sub_40C06A()==1`) builds actors
interactively; otherwise it calls `sub_404E99()` to load `EXTRA<N>.RES`.

Files present in this install: `DATA/RES/EXTRA2.RES` (dirarrows),
`EXTRA3.RES` (dirarrows), `EXTRA4.RES` (warpholes), `EXTRA9.RES` (trampolines),
`EXTRA10.RES` (conveyors). Missing EXTRA files ⇒ "There is no extra.res file
for this level!" ⇒ that board simply has no actors. So actors are OPTIONAL and
board-indexed.

### Line grammar

Comment lines start with `;`. Blank lines ignored. An actor line starts with
`-` (dash) followed by comma/space-separated fields. Field 0 is the type
letter (case-insensitive first char):

```
-A,<dir>,<x>,<y>                    dirArrow   (needs 4 args: A,dir,X,Y)
-C,<dir>,<x>,<y>                    conveyor   (needs 4 args: C,dir,X,Y)
-T,<x>,<y>                          trampoline (needs 3 args: T,X,Y)
-T,H,H                             trampoline, RANDOM placement (see below)
-W,<type>,<idno>,<x>,<y>,<linkto>  warphole   (needs 6 args)
```

Fields are split on `,` first, then leading spaces are tolerated. Numbers are
parsed by `sub_4516C1` (atoi-like). Arg-count mismatches are fatal errors in
the original ("dirarrow: needs to have 4 args total (A,dir,X,Y)", etc.).

### Coordinate normalization (applies to every actor)

```
while (x < 0) x += board_width;     // dword_4648AC = board width in tiles
while (y < 0) y += board_height;    // dword_4648B4 = board height in tiles
if (x >= board_width)  x = board_width  - 1;
if (y >= board_height) y = board_height - 1;
```

So **negative coordinates wrap from the far edge** (e.g. `-3` on a 15-wide
board = column 12). This is why the sample files use `-3`, `-5` etc. The clamp
of over-large positives is a safety net.

### Trampoline `H` (random) placement

`-T,H,H` (arg1 uppercased == 'H'): pick a random open tile on a **checkerboard
parity** and place there:

```
for (i = 0; i < 100; ++i) {
    x = rand() % board_width;
    y = rand() % board_height;
    if ((x & 1) != (y & 1) && !sub_405654(x, y))   // odd-parity, no actor yet
        break;                                       // accept this tile
}
```

i.e. trampolines land only on tiles where `(x + y)` is odd (the "brick lattice"
cells), never overlapping an existing actor. Up to 100 attempts. **This
consumes `rand()` in the ORIGINAL's setup**, but our sim's determinism is
seeded independently; see the determinism note in §7.

### Warphole linking

`sub_405A81(this_actor, &outX, &outY)`: scans all actors for another warphole
(`type==1`) whose **`idno` (+44) equals THIS warphole's `linkto` (+46)**, and
returns that partner's tile as the teleport destination. If no partner matches,
the player stays put (destination = own tile). So `-W` lines form directed
links by id: `-W,1,0,2,2,3` (idno 0 at 2,2, links to id 3) pairs with
`-W,1,3,2,-3,2` (idno 3 at 2, height-3).

## 3. CONVEYOR mechanic (type 2) — the player-facing rule

Confirmed in the character update `sub_41F29B` and the per-pixel stepper
`sub_41EC84` (already cited in facts.md as "Player movement"). The conveyor is
a **move-budget contribution**, expressed in the SAME 1/100-px units as player
speed (`+29` dword = the move-budget accumulator = our `Player::move_budget`).

The conveyor speed comes from VALUELST, indexed by the board's conveyor-speed
selector `dword_464930` (0..2, editor-cyclable):

```
id 189 = 3      ; number of conveyor speeds
id 190 = 250    ; "low"
id 191 = 350    ; "medium"
id 192 = 450    ; "high"
```

So `conveyor_speed = getvalue(190 + dword_464930)`. Per-tick contribution in
the original is `frame_delta * getvalue(190+idx) / dword_46494C`, and since our
sim is fixed 20 Hz with `frame_delta == dword_46494C == 20`, this reduces to
**exactly `getvalue(190+idx)` sub-px per tick** (default 250).

### Two cases (sub_41F29B)

**(a) Player is NOT moving this tick** (`godir == -1`, no direction input):
```
actor = sub_405654(tileX, tileY);
if (actor && actor.type == 2 /*conveyor*/) {
    player.godir = actor.dir(+44);                       // FORCE facing = belt dir
    player.move_budget += frame * getvalue(190+idx) / 20;// = +conveyor_speed/tick
    if (!sub_41EC84(player)) { player.godir = -1; ... }  // step; revert if fully blocked
}
```
=> **A player standing still on a conveyor is pushed along the belt** at
`conveyor_speed` per tick, facing the belt direction. If the stepper cannot
move it (wall ahead), godir reverts to -1 (no visible push, no facing change).

**(b) Player IS moving this tick** (has a direction input):
```
budget = normal_move_budget;               // getvalue(90)*speed etc + disease factors
actor = sub_405654(tileX, tileY);
if (actor && actor.type == 2) {
    if (actor.dir(+44) == player.godir)                  // moving WITH the belt
        budget += frame * getvalue(190+idx) / 20;        //   speed BONUS
    if (actor.dir_hi(+42) == (player.godir + 2) & 3)     // moving AGAINST the belt
        budget -= frame * getvalue(190+idx) / 20;        //   speed PENALTY
}
player.move_budget += budget;
sub_41EC84(player);                        // then step
```
=> Walking along the belt is FASTER by `conveyor_speed`; walking into the belt
is SLOWER by `conveyor_speed`; walking across (perpendicular) is unaffected.
The conveyor NEVER overrides an active input's direction — it only adjusts the
budget. It only forces direction when the player has no input (case a).

Note both `+44` (dir) and `+42 hi` (opposite compare) are set from the parsed
direction; in practice `+42 hi == +44` for conveyors, so "against" is the exact
reverse of the belt.

### Rendering

Conveyor tile art = `DATA/ANI/CONVEYOR.ANI`, drawn as an animated FLOOR tile
UNDER entities. Original `sub_4056CA` (the actor animator) advances a per-actor
frame counter; the belt art cycles. Task spec: **conveyor anim frame = counter
/ 3** (a slow scroll). Direction picks which CONVEYOR.ANI sequence (4 dirs).

## 4. TRAMPOLINE mechanic (type 3) — the player-facing rule

When the per-pixel stepper `sub_41EC84` centers a player on a tile
(`v35 == -1`, i.e. reached the tile centre) and finds a trampoline actor
there:

```
actor = sub_405654(x, y);
if (actor.type == 3 /*trampoline*/) {
    actor[24] /*+48 word*/ = 1;      // mark trampoline "triggered" (its bounce anim)
    player[+78] = 5;                 // player enters BOUNCE state 5
    player[+80] = 0;                 // reset the state's sub-timer
    sub_427961(350);                 // play SOUNDLST id 350 (boing)
}
```

While in bounce state 5 (and warp states 6/7), `sub_41DE63` returns 0 — the
player is **invulnerable to being pushed / bumped** during the bounce. The
player does NOT change tile from the trampoline (unlike the warphole): the
trampoline bounces the player **in place** — a vertical hop animation (the
`+78==5` state drives the "jump" sequence in the entity animator, sub_41DE63
guards it, `+80` counts the arc). The bounce ends when the state animation
completes and `+78` returns to its normal walking state; motion input during
the bounce is ignored (state-gated). No tiles are traversed, walls/bombs are
irrelevant because the player never leaves the tile — it is a cosmetic-plus-
invulnerability hop, NOT a launch across the board.

Compare: the BOMB path (`sub_41EC5B`, the bomb mover) also reacts to
trampolines: a sliding bomb hitting a type-3 actor sets `actor[24]=1`, bomb
state 5, sound 350 — same "boing", but that is a bombs.cpp concern (§6).

**Port trigger point (our simplification).** The original tests `v35 == -1`
("arrived at tile centre") INSIDE the per-pixel stepper loop, so it fires the
exact pixel step the character lands on the centre. Our port checks the arrival
once per tick, AFTER the stepper has spent the whole budget: it fires when the
player ends the tick exactly on the trampoline tile's centre point (both axes).
This reliably covers the common cases — standing on a trampoline, and walking
down a lane and settling on its centre — and keeps the sim integer-exact. The
one gap is a player crossing the centre pixel mid-tick at a speed that steps
OVER it and lands past it; that tick would be missed and caught only if a later
tick settles on the centre. Making it byte-for-byte would require moving the
centre test into the per-pixel stepper (movement.cpp), a small follow-up.

### Trampoline rendering

Trampoline uses a tile ANI (no dedicated TRAMP*.ANI in this install — it is
part of `EXTRAS.ANI` / a TILES set; see §5). Its `+48` "triggered" word, once
set to 1 by a step-on, plays the compressed→released bounce frames then clears.

## 5. DIRARROW (type 0) + WARPHOLE (type 1) — documented, deferred

### Dirarrow (type 0)
An arrow tile forces the direction of anything crossing it.
- For a PLAYER: dirarrows are consumed inside the stepper's per-tile check; the
  arrow's `+44` dir overrides the player's travel direction at that tile
  (a forced turn). PLAYER dirarrow re-steer is implementable here, but is
  lower priority than conveyor+trampoline and shares the exact same
  `sub_405654`/`+44` plumbing, so it is left as a fast follow-up.
- For a BOMB: a sliding bomb crossing a dirarrow is re-steered — this is the
  `sub_42542D(...)==2` / `sub_41E21E(bomb, actor)` path in the bomb mover.
  **This requires bombs.cpp and is explicitly a bombs.cpp follow-up (§6).**
- Dirarrow art: `sub_4056CA` builds sequence name `"extra arrow %s"` (dir
  suffix) from a base ANI — animated arrow tiles.

### Warphole (type 1)
A linked-teleporter tile.
- When the stepper centers a player on a warphole (`v35==-1`, actor.type==1):
  `player[+78]=6` (warp state), destination resolved by `sub_405A81` (partner
  warphole by idno/linkto, §2), stored in `player[+20]/[+24]`, sound 1330. The
  player then animates the warp-out/in and is relocated to the partner tile.
  Invulnerable during states 6/7 (`sub_41DE63`).
- Warphole art: the warp actor draws a warp sprite; `+146` latches so the
  entry effect fires once.
- **Deferred**: warphole needs the two-phase (out→in) player state machine and
  the relocation; scoped as a follow-up after conveyor+trampoline land. It
  needs NO bombs.cpp for the player path.

## 6. Follow-ups that need bombs.cpp (owned by another agent this round)

Do NOT implement here; documented for wiring later:

1. **Bomb sliding on a conveyor** — `sub_42331C` (bomb slide/fly updater,
   state 9): a bomb on a conveyor tile gets `bomb.godir = actor.dir` and
   `bomb.fly_budget += frame*getvalue(190+idx)/20`, then steps one tile in the
   belt direction. Mirrors the player conveyor push, for bombs.
2. **Dirarrow re-steering a sliding bomb** — the `sub_42542D(...)==2` /
   `sub_41E21E(bomb, actor)` call in the bomb mover: a kicked/sliding bomb
   crossing a dirarrow turns to the arrow's direction.
3. **Bomb landing on a trampoline** — sets `actor[+48]=1`, bomb state 5, sound
   350 (the bomb bounces).
4. **Bomb entering a warphole** — bomb state 6, teleport via `sub_405A81`,
   sound 1330.

All four live in `bombs.cpp` (owned) and consume the SAME actor grid we add to
`State` here, so no new plumbing is needed when they are wired.

## 7. Sounds to wire (SoundDirector is owned by another agent this round)

New `Event::Type` values are added by this change (we own events.hpp); map them
to SOUNDLST ids in `SoundDirector`:

| Event::Type              | SOUNDLST id | original call            |
|--------------------------|-------------|--------------------------|
| `TrampolineBounce`       | 350         | `sub_427961(350)`        |
| `WarpUsed` (when wired)  | 1330        | `sub_427961(1330)`       |

Conveyors emit no sound of their own in the original (the belt is silent; only
the CONVEYOR.ANI animates). No conveyor Event/sound needed.

## 8. Determinism note (for the sim port)

- The original's `-T,H` random trampoline placement consumes `rand()` DURING
  level load, on a stream separate from gameplay. Our sim is seeded and its
  RNG draw order/count per tick is the contract. To avoid perturbing the
  gameplay RNG stream, actor placement (including `H` resolution) is done at
  match SETUP from the parsed EXTRA layout carried in `MatchConfig`, BEFORE the
  first tick, and the actor grid is a STATIC per-match input (like `cells`).
  If `H` placement is ever performed sim-side it must use a setup-only RNG or a
  pre-resolved coordinate list, never `State::rng`, so the per-tick draw
  contract is untouched.
- The actor layout (per-tile type + direction) IS gameplay-affecting (it
  changes trajectories), so it is mixed into `state_hash()` exactly like
  `cells`. It never changes after setup, so it is a constant contribution to
  the hash — but it MUST be hashed so two matches with different belts don't
  collide.

## 9. Addresses touched (evidence)

| addr        | role                                                      |
|-------------|-----------------------------------------------------------|
| sub_404D16  | allocate actor registry (152×100)                         |
| sub_404E3C  | claim a free actor slot                                   |
| sub_404E99  | **parse `EXTRA<N>.RES`** into actors (the data source)    |
| sub_40551F  | choose editor-build vs `EXTRA<N>.RES` load                |
| sub_405654  | tile→actor lookup (stride 152)                            |
| sub_405A81  | warphole partner/exit resolver (idno↔linkto)              |
| sub_4056CA  | actor animator (arrow/warp/… sequence names)              |
| sub_41F29B  | character update: conveyor budget (§3), speed idx +190    |
| sub_41EC84  | per-pixel stepper: step-on warphole/trampoline (§4)       |
| sub_41DE63  | invulnerable while in bounce/warp states 5/6/7            |
| sub_42331C  | bomb slide updater: bomb-on-conveyor (bombs.cpp follow-up)|
| sub_404DB8  | direction letter → godir (n/e/s/w → 0/1/2/3)              |
| VALUELST 189–192 | conveyor speed count + low/med/high (250/350/450)    |
