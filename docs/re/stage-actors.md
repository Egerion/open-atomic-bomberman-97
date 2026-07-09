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

### Warphole one-time knockout  [VERIFIED 2026-07-04, DONE]

`sub_4056CA` case 1, the `if (!*(v23+146))` block (the `+146` latch is set on
first activation, so this fires ONCE per warphole): it clears the warphole's
OWN tile `sub_425E9B(x,y,0)` AND then clears ONE random ADJACENT tile:
```c
do { do { d = rand()%4; nx = dword_45BECC[d]+x; ny = dword_45BEDC[d]+y; }
     while (nx < 0); }
while (nx >= 15 /*W*/ || ny < 0 || ny >= 11 /*H*/);   // retry until in-bounds
sub_42C0C8("knocking out %u,%u");
sub_425E9B(nx, ny, 0);                                 // set that tile to Blank
```
`dword_45BECC={0,1,0,-1}` / `dword_45BEDC={-1,0,1,0}` (the cos/sin dir tables) —
so the target is always a true cardinal neighbour (never the centre; `{0,0}` is
never produced), and whatever sat there (brick OR solid) is set to Blank.
`sub_425E9B(_,_,0)` writes cell type 0 via `sub_425E36` (bounds-checked).

Port: `apply_actors` clears one neighbour off the SETUP-only LCG (same stream as
`-T,H`), never `State::rng` — it only mutates `cfg.cells`, so no per-tick RNG and
no golden impact (golden has no warpholes). Test: `tests/test_match.cpp`.

## 3. CONVEYOR mechanic (type 2) — the player-facing rule  [VERIFIED 2026-07-04]

Confirmed in the character update `sub_41F29B` and the per-pixel stepper
`sub_41EC84` (already cited in facts.md as "Player movement"). The conveyor is
a **move-budget contribution**, expressed in the SAME 1/100-px units as player
speed (`+29` dword = the move-budget accumulator = our `Player::move_budget`;
the stepper `sub_41EC84` reads it at `+116` and spends **100 units per pixel
step** — `for (; +116 > 0; +116 -= 100)`).

### The budget arithmetic (the "too fast" fix)

Both the normal player speed AND the conveyor push are scaled by the SAME
per-frame ratio `dword_464958 / dword_46494C`, where
`dword_46494C = 1000/getvalue(30)` (ms per tick) and `dword_464958` = the
wall-clock ms elapsed since the last frame, clamped to `getvalue(31)`.

- `getvalue(30) = 20` (tick rate)  ⇒  `dword_46494C = 1000/20 = 50` ms/tick.
- `getvalue(31) = 150` (elapsed-ms clamp).
- At the locked tick rate the frame delta ≈ 50 ms ≈ `dword_46494C`, so the
  ratio `dword_464958 / dword_46494C ≈ 1`.

Normal player move (`sub_41F29B` ~23440, else branch):
```
v91 = getvalue(42) + skates*getvalue(90) - collisions*getvalue(91);  // = speed
if (molasses) v91 /= 3;  if (hyper||super) v91 = 3*v91/2;
v91 = dword_464958 * v91 / dword_46494C;      // scale by frame/tick  (≈ v91)
player[+29] += v91;
```
Conveyor contribution (`sub_41F29B` ~23422 forced case; ~23447/23449 bonus/pen):
```
player[+29] += dword_464958 * getvalue(dword_464930+190) / dword_46494C;
```
**Because the SAME `dword_464958/dword_46494C` factor applies to both, at 20 Hz
the per-tick contributions reduce to `getvalue(42)` (=923) for the walk and
`getvalue(190+idx)` for the belt — in the SAME budget units, sharing the same
100-per-pixel threshold.** Our sim already bakes the ≈1 ratio out (fixed step):
`move_budget += p.speed(=923) + extra`, spent 100/px. So the belt's `extra` is
**exactly `getvalue(190+idx)`** with NO extra scaling. The prior code injected
the raw 250 correctly *for index 0*, but defaulted to the WRONG index (see below).

### The conveyor-speed OPTION (`dword_464930`) — NOT a per-board tunable

VALUELST (confirmed 2026-07-04):
```
id 189 = 3      ; "here's how many different speeds there are"  (count)
id 190 = 250    ; low     (1/100 px, "100th of a pixel" per the file comment)
id 191 = 350    ; medium
id 192 = 450    ; high
```
`dword_464930` is the **game "Conveyor Speed" OPTION** (Low/Med/High), NOT a
per-board field. It is clamped to `[0, getvalue(189)-1]` and sourced from:
- **hardcoded default `dword_464930 = 1`** (medium) in the game-init routine
  (pseudo.c 14652, alongside the other option defaults);
- the persisted options struct field `a1+14` when a config is loaded
  (pseudo.c 12655), i.e. the `conveyor_speed=` line in `options.ini`;
- editor cycling (pseudo.c 9321/9424, wrap/clamp against getvalue(189)).

This install's `options.ini` carries **`conveyor_speed=2`** (high = 450). The
program default (no options file) is **index 1 (medium = 350)**.

⇒ `conveyor_speed = getvalue(190 + option)`; default option **1 (=350)**, this
install **2 (=450)**. The old `conveyor_speed_index = 0` (=250) default was the
"too fast/too slow" bug: the belt speed itself was in the right units, but the
selector defaulted to the wrong tier. Fixed: default the selector to 1 (the
binary's default) and let MatchConfig override it from the options.

**Options wiring (DONE 2026-07-04, "devam" #39).** The reader is confirmed:
`sub_406238` splits each `options.ini` line on `=`, `stricmp`s the key, `atoi`s
the value into `dword_464930`, then clamps `[0, getvalue(189)-1]` (pseudo.c
7740/7862-7865). Ported as `assets::load_options()` (install.hpp/cpp), which
surfaces `conveyor_speed` as an optional; `game_app` reads
`<game_dir>/options.ini` at init and sets `cfg.tuning.conveyor_speed_index` per
round (absent ⇒ keeps the default 1). Config-only, no golden impact.

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

## 4. TRAMPOLINE mechanic (type 3) — FLY + RANDOM LAND  [CORRECTED 2026-07-04]

**The trampoline is NOT an in-place bounce.** A prior note wrongly said the
player "hops in place"; the binary shows a **flight that teleports the player to
a random nearby tile at the apex**. RE'd byte-for-byte from `sub_41F29B` state 5
(raw disasm, imagebase 0x400000) — see the arithmetic below.

Trigger is unchanged: when the per-pixel stepper `sub_41EC84` centres a player
(`v35 == -1`) on a trampoline actor it sets `actor[+48]=1`, `player[+78]=5`
(BOUNCE state), `player[+80]=0`, and plays SOUNDLST id 350 (boing). While in
state 5 (and warp states 6/7) `sub_41DE63` returns 0, so the player is
**invulnerable to being pushed** and its input is ignored (state-gated).

### The flight — `sub_41F29B` state 5 (`v86==5`), raw disasm 0x420280..0x42053f

`player[+78]` packs `state | (counter << 16)`; the counter word is `player[+80]`
(`= (*(int*)(v111+39)) >> 16`), advanced once per tick by the 20 Hz frame loop:
```
for ( +82 += dword_464958; +82 > 0; +82 -= dword_46494C )  ++[+80];   // ++c, 1/tick
if ( c >= getvalue(680) )   { [+78] = 0; [+80] = 0; }                  // end at 30
```
**Apex relocation — fires the single tick `c == getvalue(680)/2 == 15`**
(disasm 0x420381: `cmp ebx, 15; jne skip`). The exact loop (0x4203a7):
```
cx = pixelToTileX(player+28);  cy = pixelToTileY(player+32);          // sub_42665C/sub_4266A3
for ( m = 0; m < 100; ++m ) {
    nx = cx + rand()%5 - 2;          // FIRST rand draw   (0x4203ef)
    ny = cy + rand()%5 - 2;          // SECOND rand draw  (0x420411)  -- BOTH always drawn
    if ( nx != cx && ny != cy        // must differ on BOTH axes
         && !sub_425FB9(nx, ny)      // not solid (see below)
         && !sub_422E48(nx, ny) ) {  // no grounded bomb (see below)
        player+28 = tileToPixelX(nx);  player+32 = tileToPixelY(ny);   // sub_426524/sub_42655F
        break;
    }
}
++[+80];   // extra increment (0x4204af) — bumps c past 15 so the apex can't re-fire
```
- **`sub_425FB9(x,y)` = SOLID test.** Reads the collision grid `dword_46222C`
  (filled from the board tile type via `sub_425E36`; `0` = walkable floor,
  non-zero = wall/brick) and returns `1` (blocked) out of bounds. So
  `!sub_425FB9` ⟺ in-grid AND floor — exactly our `grid::tile_open`.
- **`sub_422E48(x,y)` = BOMB test.** Scans the 152-byte bomb registry for a
  bomb on `(x,y)` (tile derived from its pixel `+28/+32`) whose sub-mode `+46`
  is neither 2 nor 3 (airborne/thrown) — i.e. a grounded bomb. Maps to our
  `grid::bomb_at` (which already excludes flying bombs).

So at the apex the player LEAVES the trampoline for a random tile up to 2 cells
away on each axis, both axes differing, that is open and bomb-free — the "fly".
If no candidate qualifies in 100 tries (fully boxed in) it stays put. Walls and
bombs ARE relevant here (unlike the old wrong "never leaves the tile" claim).

### The hop arc (presentation) — CONFIRMED linear tent, `35 * min(c, 30-c)`

`sub_41F29B` blits the body at `y - v80` (0x42052e) where, from the raw disasm
0x4204b3..0x420517:
```
v80 = getvalue(681) * (c < getvalue(680)/2 ? c : getvalue(680) - c)
```
i.e. a **linear tent** `35 * min(c, 30-c)`, peaking `35*15 = 525 px` at c=15. The
sprite rockets high off the top of the field and comes down onto the random apex
tile. `getvalue(681) = 35` ("pixels vertically per frame", VALUELST id 681) is a
presentation value — the integer sim omits it; the renderer applies the tent to
the body (the shadow stays on the ground). This arc is EXACT, not an approximation.

### Bounce DURATION — VALUELST id 680 = 30 frames  [VERIFIED 2026-07-04]

id 680 = 30 ("how many frames do you bounce on a trampoline?"), read as
`getvalue(680)` above (bounce ends at c==30, apex at c==15). Now
`tuning.trampoline_bounce_frames = 30`, wired through `Tuning::apply(680)`.
(This replaced a former `= 20` our-tunable and an earlier mistaken 12 — the
decorative belt-frame count of the "extra trampoline" ANI, not the gameplay
timing.) The `"extra trampoline"` ANI (EXTRAS.ANI, 12 art frames) is purely the
tile mat art (`sub_4056CA` case 3); the flight timing is id 680.

Compare: the BOMB mover `sub_42331C` does **not** react to trampolines at all —
`sub_427961(350)` is called from exactly one site (the player stepper), and
`actor.type == 3` is tested nowhere in the bomb mover. A sliding bomb rolls over
a trampoline tile with no bounce (§6).

### Port (libs/sim)

`StageActorSystem::tick_bounce(Player&, int)` mirrors state 5. `Player::bounce`
is the equivalent DOWN countdown (30→0); the elapsed count is `c = 30 - bounce`.
`tick_bounce` decrements first (the original's leading `++c`), then at
`c == 30/2 == 15` runs the relocation loop **on `State::rng`** (two
`random_below(s,5)` per attempt, ALWAYS both, break on the first valid tile). Our
down-counter passes through each value exactly once, so the apex fires once (the
original's extra `++c` is a re-fire guard we don't need). `simulation.cpp`
state-gates the whole flight in `player_turn`: `if (bouncing) { tick_bounce; return; }`.

**Determinism / golden.** The relocation is the ONLY new sim RNG. It runs only
inside the state-5 gate, which requires a trampoline (`start_bounce`). Boards
with no trampolines never enter it → zero draws → the per-tick RNG order/count is
untouched and the golden scenarios (which place no actors) are byte-identical. No
new hashed field: the relocation mutates `Player::x/y` and `State::rng`, both
already hashed, and reuses the already-hashed `Player::bounce` countdown.

### Step-on trigger point (port detail, unchanged)

The original triggers inside the stepper (`v35 == -1`); our port fires it
mid-walk via the `MovementSystem::move` step-on callback (see §5) with a post-walk
safety net for the standing-still case. The one-shot latch `Player::tramp_latch`
(set on launch, cleared on leaving the tile) keeps a stationary centred player to
one hop per entry.

### Warp/teleport animation — sequence name `"spin"`  [CONFIRMED 2026-07-04]

The warp (states 6/7) draws the player with the `strcpy_`'d literal sequence name
at **0x45a213 = `"spin"`** (both state blocks: disasm 0x420544 and 0x4205d8,
`mov edx, 0x45a213; lea eax, [ebp-0x78]; call strcpy_`). `"spin"` is the 5th
sequence in `DATA/ANI/WALK.ANI` (after the four `walk <dir>`). The renderer draws
`SequenceSet::spin[player]` while `Player::warp > 0`, advancing the frame by
`kWarpTicks - warp` (elapsed). There is NO `warp`/`teleport` sequence anywhere in
STAND.ANI or the string table — `"spin"` is the confirmed one.

### Trampoline rendering

Trampoline art is the `"extra trampoline"` sequence in `EXTRAS.ANI` (no
dedicated TRAMP*.ANI, 12 art frames). Its `+48` "triggered" word, once set to 1
by a step-on, plays the bounce frames then clears; **while idle (`+48 == 0`) it
shows only frame 0** (the resting mat). Our renderer therefore gates the
trampoline animation on the hashed bounce state (`Player::bounce` of a player on
the tile, mapped onto the 12 art frames) — animating it every tick (the prior
bug) made an untouched trampoline appear to bounce forever.

## 5. DIRARROW (type 0) + WARPHOLE (type 1)  [IMPLEMENTED 2026-07-04]

### Dirarrow (type 0) — BOMB-ONLY  [VERIFIED 2026-07-04]
An arrow tile re-steers a **sliding bomb** crossing its centre. It does **NOT**
steer walking players.
- **PLAYER — no effect.** The player mover `sub_41F29B` calls `sub_405654` only
  for warpholes (~23354, the auto-drop path) and conveyors (~23417/23443). There
  is NO type-0 (dirarrow) branch anywhere in the player mover or the per-pixel
  stepper's actor reads. So a walking player passes over an arrow tile with no
  forced turn. (The `sub_42542D(...)==2` call at the end of `sub_41EC84` is the
  FLAME-obstacle grid `dword_462214` + the powerup dispatcher `sub_41E21E`, a
  DIFFERENT registry from the actor grid `dword_45E0A8` — not a dirarrow.) We do
  NOT re-steer players; a prior draft that did was removed as unfaithful.
- **BOMB** (`sub_42331C` slide loop, pseudo.c ~25532): a sliding bomb, at a tile
  centre (`!v79 && !v80`, both alignment offsets zero), re-reads the actor grid
  — `if (actor && actor[1]==0 /*dirarrow*/) { bomb[+44] = actor[22]; }` — i.e.
  the bomb turns to the arrow's godir. Ported into `bombs.cpp` `slide()`, reading
  `actor_type`/`actor_dir`, at each tile-centre crossing (kicked + conveyor).
- Dirarrow art: `sub_4056CA` builds sequence `"extra arrow <compass>"`
  (`EXTRAS.ANI`: "extra arrow north/east/south/west", 21×21, hotspot 10,20),
  frame = `+48` (advances 1/animator-call).

### Warphole (type 1)
A linked-teleporter tile. **No RNG is drawn by the warp path** (confirmed).
- Trigger (`sub_41EC84`, `v35 == -1` step-on, `actor[1]==1`):
  `player[+78]=6` (warp state), `player[+80]=0`, `sub_405A81(actor,&dx,&dy)`
  resolves the destination and it is stored in `player[+20]/[+24]`, then
  `sub_427961(1330)`. The player then animates the warp-out/in and is relocated
  to the partner tile; invulnerable during states 6/7 (`sub_41DE63`).
- `sub_405A81`: linear scan of the actor registry for ANOTHER warphole
  (`+4 == 1`) whose **idno (`+44`) equals THIS warphole's linkto (`+46`)**;
  returns that partner's `+28/+32` tile. No match ⇒ destination = own tile.
  **Pure scan, zero `rand_()` calls — the warp draws no RNG.** (Contrast the
  DIRARROW/checkerboard `-T,H` placement, which DOES use `rand_()`, but only at
  level LOAD, on the setup stream — §8.)
- Warphole art: `"extra warp 1"` (`EXTRAS.ANI`, OPENHOLE.TGA, 40×36, a single
  static frame); the actor `+146` latches so the load-time entry effect fires
  once.

### Two-phase warp timing — CONFIRMED (`sub_41F29B` state 6/7 blocks, ~23155/23215) [2026-07-04]

The player warp is a **two-phase animation**, now read directly from the state
machine in `sub_41F29B` (`v86` = `player[+78] >> 16` = the action state):

- **State 6 (warp-out):** advance the frame counter `+40` each tick; when
  `+40 > 8` (i.e. after **9 ticks**), set state 7, reset `+40`, and **relocate**
  the player — `player[+28]/[+32]` (position) `= player[+20]/[+24]` (the exit
  stored on step-on). So the player does NOT move on step-on; it moves at the
  out→in boundary.
- **State 7 (warp-in):** advance `+40`; when `+40 > 8` (another **9 ticks**), set
  state 0 (normal). The player can move again.

So the whole warp is **18 ticks** (9 + 9). Throughout, `sub_41DE63` returns 0 for
states 6/7 (`else if (+78 == 6 || +78 == 7) return 0;`, pseudo.c 22003) — the
player is invulnerable to being pushed, and the mover is not run (both state
blocks `goto LABEL_246/239`, skipping the movement budget), so input is ignored.
No RNG anywhere on the path.

**Port.** `Player::warp` is an 18-tick countdown (hashed — it gates movement
every active tick). The warp STARTS on centring (sets `warp = kWarpTicks(=18)`,
the re-entry latch, captures the exit tile into `Player::warp_to_*`, and emits
`WarpUsed`/sound 1330) but does NOT move the player. `StageActorSystem::tick_warp`
decrements it each tick and, at the midpoint (`warp == kWarpMid = 9`), relocates
the player to the captured exit. `simulation.cpp` state-gates the whole warp
exactly like a trampoline bounce: `if (warping) { tick_warp; return; }`. The
one-shot latch (`Player::warp_latch`, cleared on leaving the warphole tile) stops
a re-warp at the exit (itself a warphole). A warphole with no partner has its
dest == its own tile, so the warp is a harmless in-place hop.

**The step-on MUST fire mid-walk, not after — root cause of "STILL stuck".**
The original triggers the warp INSIDE the per-pixel stepper (`sub_41EC84`, the
`for(+116>0; +116-=100)` loop) at `v35 == -1`, i.e. the exact pixel step that
lands the player on the tile centre. A first port fired it only AFTER the whole
per-tick move budget was spent, requiring the player to END the tick exactly on
the centre pixel. But the mover steps ~9 px/tick (speed 923, 100 units/px) and
tile centres are 40 px apart, so a player WALKING through a warphole in an open
lane steps OVER the centre pixel and almost never lands on it — the warp trigger
never fired and the player just walked across the warphole (the "still stuck /
tıkalı" report). This was NOT a passability problem: the warphole lives in the
actor grid, not `cells`, so the tile is fully walkable (`tile_open` is true);
and NOT an exit-resolution problem (`apply_actors`/`sub_405A81` resolve the
partner correctly). It was purely the trigger point.

Fix: `MovementSystem::move` takes a step-on callback (`StepOnFn`, a plain
function pointer — no heap, deterministic) invoked the instant a per-pixel step
settles the player on a tile centre. `StageActorSystem::move_on_actor` passes it,
so a walking player fires `start_warp`/`start_bounce` mid-walk exactly like
`v35 == -1`. `warphole_after_move`/`trampoline_after_move` remain as a post-walk
safety net for the standing-still case (player already centred, no step). Both
paths call the same latched `start_warp`/`start_bounce`, so a walk across the
centre fires exactly once. Verified equivalent to the original: it fires post-
step (arrival at centre) vs the original's pre-step (`v35 == -1` at centre−1,
about to step to centre) — the same physical event, and both continue the loop
from the centre with identical remaining budget.

**Destination captured at step-on.** The exit tile is stored into
`Player::warp_to_*` when the warp starts (mirrors the original storing the dest
in `+20/+24` right when it sets state 6). `tick_warp` relocates to that stored
tile, NOT a midpoint lookup of `warp_dest` at the current tile — because the
loop that triggered the warp can slide the player a few px OFF the warphole
within the trigger tick, so a midpoint tile lookup could read a non-warp tile.

**CORRECTED 2026-07-10 (facts.md "Bomb/warphole reconciliation"): the bomb path
does NOT warp at all — see §6 item 4, rewritten.** `warp`, `bounce`, and
`warp_to_*` pack into ONE hash word (each fits a byte: bounce ≤ 30,
warp ≤ 18, dest tiles ≤ 14); ALL are 0 on boards with no warpholes/trampolines,
so that word is `mix(0)` there — byte-identical to before, and the golden
scenarios (no actors) are unchanged.

Note the 8-frame per-phase threshold is a HARDCODED constant in the state
machine (`+40 > 8`), not a VALUELST id — like the head-hit stun of 16. Confirmed
9+9=18 ticks: the per-phase frame counter `+40` advances once per tick at 20 Hz
(`for(+41 += dword_464958; +41 > 0; +41 -= dword_46494C) +++40`, with the
elapsed-ms delta ≈ dword_46494C = 50 at the locked rate), and each phase ends at
`+40 > 8` = the 9th increment.

## 6. Bomb ↔ actor reactions (`sub_42331C`)  [IMPLEMENTED 2026-07-04]

The bomb mover `sub_42331C` iterates every bomb (152-byte struct). A moving/
resting bomb is `state +16 == 9`, with a movement sub-mode `switch(+46)`.

1. **Bomb on a conveyor** (pseudo.c ~25365, `case 0`): if the actor under the
   bomb's tile is a conveyor (`actor[1]==2`), set `bomb[+44] = actor.godir`,
   `bomb[+116] += dword_464958 * getvalue(190+idx) / dword_46494C`
   (= `getvalue(190+idx)` sub-px/tick at 20 Hz — the SAME belt speed as the
   player push), a 2-px perpendicular-centering nudge, then `+= 100` and step
   (LABEL_21). So a bomb resting on a belt slides along it. Our port
   (`bombs.cpp` `slide_on_conveyor`): a *non-moving* bomb on a conveyor tile is
   set moving in the belt direction with the belt budget, then handed to the
   normal slide. (The `+100` per-tick kicker in LABEL_21 mirrors the existing
   kicked-bomb `+ 100`; see the units note in bombs.cpp.)
2. **Dirarrow re-steering a sliding bomb** (pseudo.c ~25532, `case 2` slide
   loop): at a tile centre (`!v79 && !v80`), `if (actor && actor[1]==0)
   bomb[+44] = actor[22];` — the sliding bomb turns to the arrow's godir.
   Ported into `bombs.cpp` `slide()` at each tile-centre crossing.
3. **Bomb landing on a trampoline — DOES NOT HAPPEN.** [VERIFIED 2026-07-04]
   `sub_427961(350)` (the trampoline boing) is called from exactly ONE site in
   the whole binary — `sub_41EC84` line 22606, inside the `actor[1]==3` branch
   of the PLAYER stepper — and `actor.type == 3` is tested nowhere in the bomb
   mover `sub_42331C`. So a bomb slides straight over a trampoline tile with no
   bounce, no boing. The prior note speculating a `sub_41EC5B` bomb-bounce path
   was wrong; there is no bomb↔trampoline interaction. Nothing to port.
4. **Bomb entering a warphole — NEVER HAPPENS; blocked like a wall.**
   **CORRECTED 2026-07-10** (facts.md "Bomb/warphole reconciliation";
   supersedes the earlier "teleport via the same `warp_dest` grid" claim,
   which was wrong). The warp resolver `sub_405A81` (idno↔linkto scan, the
   function that actually computes a teleport destination) is called from
   **exactly one call site in the whole binary**: `sub_41EC84` line 22594, the
   PLAYER per-pixel stepper. `sub_42331C` (the bomb mover) never calls it.
   Instead, the sliding-bomb cell-entry probe `sub_4230A5` (pseudo.c
   25155-25179, invoked from the kicked/conveyor slide loop at 25555) ends
   with `return (!v8 || v8[1] != 1) && sub_425FB9(a1,a2) == 0;` where
   `v8 = sub_405654(a1,a2)` — so whenever the probed tile carries ANY actor of
   type 1 (warphole), the whole expression is false **regardless of the
   underlying cell type**: the tile is impassable to a sliding bomb, exactly
   like a wall. A kicked or conveyor-carried bomb therefore halts (or, if
   jelly, reverses/ping-pongs) one tile short of a warphole and can never
   cross onto it — there is no bomb-warp code path anywhere in the binary.
   (The punched/flying-bomb landing check similarly refuses to settle a bomb
   on a warphole tile — pseudo.c 25453 `v62[1] != 1` — it just hops onward,
   same as over a wall; unlike the slide probe it does NOT destroy a powerup
   on the tile as a side effect, since it never reaches the tile-entry probe
   at all.) Ported: `bombs.cpp` `slide()`'s cell-entry probe now includes
   `actor_type[..] == Warphole ⇒ blocked`, matching `sub_4230A5`'s verdict;
   the prior teleport branch (and `Bomb::warp_latch`, entirely vestigial once
   warping is unreachable) were removed as unfaithful.

Three of the four consume the SAME `actor_type`/`actor_dir` grids in `State`;
`warp_dest` remains a player-only input (§5).

## 7. Sounds  [WIRED 2026-07-04]

`SoundDirector` maps the stage-actor events to SOUNDLST ids (confirmed against
`SOUNDLST.RES` and the `sub_427961` calls in `sub_41EC84`, pseudo.c 22599/22606):

| Event::Type        | SOUNDLST id | resolves to | original call     |
|--------------------|-------------|-------------|-------------------|
| `TrampolineBounce` | 350         | "1017"      | `sub_427961(350)` |
| `WarpUsed`         | 1330        | "warp1"     | `sub_427961(1330)`|

Both play a single slot (`audio_.play(id)`), matching `sub_427961`'s single-id
call (not a range pick). Conveyors and dirarrows emit no sound of their own in
the original (the belt is silent; only the ANI animates). No conveyor/arrow
Event/sound needed.

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
| sub_41F29B  | character update: conveyor budget (§3), speed idx +190; state 5 trampoline FLY+random-land relocation (0x4203a7) + tent arc (0x4204b3); states 6/7 warp draw "spin" (§4) |
| sub_41EC84  | per-pixel stepper: step-on warphole/trampoline (§4)       |
| sub_41DE63  | invulnerable while in bounce/warp states 5/6/7            |
| sub_425FB9  | trampoline SOLID test: collision grid dword_46222C, 0=floor, 1 OOB (== grid::tile_open) |
| sub_422E48  | trampoline BOMB test: grounded bomb (mode +46 != 2,3) on tile (== grid::bomb_at) |
| sub_42665C/sub_4266A3 | pixel→tile X/Y; sub_426524/sub_42655F tile→pixel X/Y (relocation) |
| 0x45a213    | warp sequence-name literal **"spin"** (strcpy'd in warp states 6/7); lives in WALK.ANI |
| sub_42331C  | bomb mover: bomb-on-conveyor (~25365), bomb dirarrow re-steer (~25532); no tramp/warp interaction (blocked by `sub_4230A5` before ever reaching a warphole tile, §6 item 4) |
| sub_4230A5  | sliding-bomb cell-entry probe (pseudo.c 25155-25179): blocks entry to a warphole tile (`v8[1] != 1` verdict) — the reason bombs never warp |
| sub_404DB8  | direction letter → godir (n/e/s/w → 0/1/2/3)              |
| sub_4056CA  | actor animator: conveyor frame = +48/3; tramp bounce = 12-frame seq (case 3) |
| sub_41DA5C  | sequence frame count (statecnt at `dword_461B5C+60*seq+52`) |
| sub_41DAA7  | frame picker: `frame % statecnt`                          |
| VALUELST 30 | tick rate = 20  ⇒ dword_46494C = 1000/30 = 50 ms/tick     |
| VALUELST 31 | elapsed-ms clamp = 150 (dword_464958)                     |
| VALUELST 42 | player start speed = 923 (same budget units as belt)      |
| VALUELST 189–192 | conveyor speed count=3 + low/med/high = 250/350/450  |
| dword_464930 | "Conveyor Speed" game option (0/1/2); default 1 (medium); options.ini `conveyor_speed=` |
| VALUELST 680/681 | trampoline flight = 30 frames (apex@15, relocate) / hop arc = 35*min(c,30-c) px (sub_41F29B state 5) |
| EXTRAS.ANI  | "extra trampoline" = 12 art frames (cosmetic); "extra warp 1" = 1; "extra arrow <dir>" |
| CONVEYOR.ANI | "extra conveyor north/east/south/west" (4/5/4/5 frames, 40×36 hotspot 20,35) |
| SOUNDLST 350/1330 | trampoline "1017" / warp "warp1"                    |
