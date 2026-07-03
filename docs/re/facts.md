# BM95.EXE — reverse-engineering facts ledger

Facts distilled from static analysis of the original `BM95.EXE` (SHA of the
1997 build; 424,448 bytes). **This file records only facts — numbers,
addresses, and behavioral observations — never copied code.** The executable
and any raw disassembly stay out of the repository (see clean-room policy
below). Each entry cites its provenance so it can be re-checked.

## Binary profile

| Property | Value | How known |
|---|---|---|
| Format | PE32, i386 | PE header (machine 0x14c, opt magic 0x10b) |
| Image base | 0x00400000 | optional header |
| Entry point | RVA 0x4bdcc | optional header |
| Code section | `BEGTEXT` RVA 0x1000, ~348 KB | section table |
| Data | `DGROUP` RVA 0x58000; `.bss` 0x5e000 | section table |
| Compiler | **Watcom C/C++** | `BEGTEXT`/`DGROUP` section names; register calling convention seen throughout |
| Calling convention | Watcom register: args in **EAX, EDX, EBX, ECX**; callee saves ebx/ecx/edx/esi/edi/ebp | prologue of getvalue @ 0x4124a4 reads arg from EAX |
| Graphics | **DirectDraw** (`DDRAW.dll`) | import table |
| Input | **DirectInput** (`DINPUT.dll`) | import table |
| Audio | **DirectSound** (`DSOUND.dll`) + `WINMM.dll` | import table |
| Netplay | `WSOCK32.dll` | import table |

Implication: the renderer is a DirectDraw blit loop over a 640×480 palettized
surface, matching the PCX/ANI asset formats we already decode.

## VALUELST lookup mechanism — CONFIRMED

`getvalue(id)` lives at **0x4124a4**. Decompiled behavior (paraphrased, not
copied):

- Reads `id` from EAX.
- If `id < 0` or `id >= count` (count at global `[0x46024c]`) → not-found path.
- Otherwise indexes a flat dword table at `[0x460250]` by `id * 4`.
- If `table[id] == 0` → not-found path.
- Not-found path increments a counter at `[0x460555]`, compared against 5 —
  i.e. a small number of missing values aborts the program. This is the code
  behind VALUELST.RES's own header warning: *"DO NOT CHANGE the first number!
  if you do the program will exit to DOS because it cannot find a particular
  value."*

**Consequence for us:** the game stores VALUELST values in an array indexed
directly by resource id and looks them up by id, exactly as our
`Tuning::apply(id, value)` model does. Our id-indexed approach is faithful to
the binary. (Provenance: disassembly window at 0x4124a4–0x4124f6.)

Observed literal-id `getvalue` call sites (28 of them) request only
**non-gameplay** ids: 95, 97, 700–768 (menu layout coords), 900/905 (AI),
1200–1250 (campaign). The core gameplay values in VALUELST are all marked
`; PGT` in the file and are **not** fetched through `getvalue` — strong
evidence they are loaded as a batch into a settings struct through a separate
path. (Provenance: exhaustive scan of `call 0x4124a4` sites.)

## ANI per-step timing — PARTIALLY DECODED

Each `STAT` block's `HEAD` (46 bytes) is zero except a leading u16 that only
ever holds **30 (0x1E)** or **0xFFFF**. It is therefore a two-state flag, not a
free-form duration. Per-sequence counts:

- `walk north`: 15 steps, all 30
- `flame center green`: 6× 30 then 1× 0xFFFF
- `flame tipeast green`: 7× 30
- `bomb regular green`: 1× 30 then 17× 0xFFFF
- `die green 1`: 40× 30 and 43× 0xFFFF

Best current reading: 0x1E marks a real animation step and 0xFFFF marks a
hold/tween/loop marker. Fully resolving it needs the exe's ANI player routine,
not yet located. Tracked as open. (Provenance: `DATA/ANI/*` STAT parse.)

## Function map (from the IDA database + embedded source names)

The exe embeds its source filenames (`bombs.c`, `flame.c`, `ai.c`, `scheme.c`,
`sound.c`, `graf.c`, `misc.c`, `campaign.c`, `search.c`, `net1.c`, `aliens.c`,
`options.c`) and debug format strings. Reading the user-provided IDA database
(`BM95.EXE.idb`) with `python-idb` gives 1519 functions, FLIRT-named Watcom
runtime, and cross-references. String xrefs pin the gameplay functions exactly:

| Function | Anchor string | Role |
|---|---|---|
| `0x41f29b` | "player %u, godir is invalid", "cornerhead %u", "walkbomb %s" | player movement / animation-state selection |
| `0x426d06` | "flame %s green" | flame animation build/update |
| `0x42331c` | "bomb %s green" | bomb animation |
| `0x405de3` | "diseases_destroyable=%u" | disease logic |
| `0x40a1c6` | "player %u offscreen at %d,%d" | position/offscreen check |
| `0x4124a4` | (found cold earlier) | getvalue(id) |

Confirmed from disassembly of `0x41f29b`:
- Grid X index is bounds-checked against **14** (`cmp …, 0xe`) → the playfield
  is **15 tiles wide**, matching our `kGridWidth = 15`.
- Animation timing multiplies by **20** (`imul eax, eax, 0x14`) — the VALUELST
  id-25 "standard frame" reference (20). Confirms our 20 Hz base.
- The player struct uses a `word` action-state field at offset `+0x4e`
  (compared against 3–7, 20, 40) and position/flag fields at `+0x30/+0x66/
  +0x74/+0x78/+0x80`. Full layout not yet mapped.

### Flame lifetime — CONFIRMED = 10 frames

Read directly from `0x426d06` (the per-tick flame-grid update): it loops over
the cell grid (cell struct = **152 bytes / 0x98**, dims at `[0x4648b4]` and
`[0x4648ac]`). For each lit cell it advances a fixed-point phase accumulator
(`struct[+0x44] += [0x464958]`) and, at frame `struct[+4] == 9`, kills the
flame (`struct[0] = -1`). So a flame lives through frames 0..9 = **10 frames**,
matching VALUELST id 10 ("frames to cycle the regular flame animation" = 10)
and our `flame_frames = 10`. Upgraded guess → confirmed; the sim already
matches, no change needed.

Also learned here: the engine stores positions as **16.16 fixed point**
(`sar …, 0x10` to get the integer pixel). Our sim uses a 1/100-pixel integer —
a different representation of the same idea; conversions of any raw fixed-point
constant we pull from the binary must account for this.

### In-sandbox decompiler attempt (angr)

`angr` was installed and run in the sandbox to self-serve pseudocode. Its
CFGFast function-boundary detection does not cleanly handle this Watcom binary
(functions at the IDA-known starts aren't recognized; region and full scans
gave inconsistent/empty results). Raw capstone disassembly reading works and is
how the flame constant was confirmed, but it is slow for a large fixed-point
routine. For the remaining movement/collision constants the Hex-Rays decompiler
(pinned functions below) is the efficient tool.

### Fastest way to finish the deep constants

The four functions above are now pinned. Viewing their **Hex-Rays pseudocode**
in IDA (you have the decompiler) and pasting it here lets every remaining
constant — corner-slide threshold, flame lifetime, punch/throw fuse behavior,
disease application — be read cleanly in minutes, versus grinding raw Watcom
disassembly in 45-second sandbox slices. Pasted pseudocode is a facts source;
it is not committed to the repo.

## Player struct + update loop — CONFIRMED (from `sub_420F07` pseudocode)

The per-frame player update `sub_420F07` iterates **10 players**; the player
struct base is `0x461BC4`, **stride 152 bytes (38 dwords)**. Fields seen:

| Offset | Type | Meaning |
|---|---|---|
| +0x00 | dword | active/moving state (truthy while acting) |
| +0x10 | byte | alive / on-screen |
| +0x54 | byte | a flag (team?) |
| +0x68 | dword | position, **16.16 fixed point** (`>>16` = pixel) |

Timing: global timers are decremented by the frame-delta `dword_464958` and
clamped ≥0 — the same frame-delta used everywhere, confirming the fixed-cadence
model. The animation-advance `sub_41F29B` runs for every player; the movement
handler **`sub_420D4E`** runs only for the input-controlled player
(`dword_46492C == i`). The corner-slide threshold lives in `sub_420D4E` (or a
collision helper it calls, e.g. `sub_4105B0`).

## Player movement / collision stepper — CONFIRMED (`sub_41EC84`)

Located via the full Hex-Rays dump (batch decompile, see method.md) and
cross-checked against raw disassembly of `0x41EC84–0x41F262`. `sub_41F29B`
(animation-advance) dispatches the actual per-pixel mover **`sub_41EC84`** each
tick for the player whose input/AI has been resolved. This function is the
movement *feel*; the constants below are read straight from it.

Geometry globals (set in the resolution-init at `0x4148…`):

| Global | Value | Meaning |
|---|---|---|
| `[0x4648AC]` | **15** | grid columns (matches `kGridWidth`) |
| `[0x4648B4]` | **11** | grid rows (matches `kGridHeight`) |
| `[0x4648A4]` | **40** | tile width, px |
| `[0x4648A0]` | **36** | tile height, px |
| `[0x464898]` | `(screenW−600)/2` | field origin X (≈20 @ 640) |
| `[0x4648A8]` | `screenH−412` | field origin Y (≈68 @ 480) |
| `dword_45BECC` | `{0, 1, 0, −1}` | dx per godir |
| `dword_45BEDC` | `{−1, 0, 1, 0}` | dy per godir |

godir order is **0=Up, 1=Right, 2=Down, 3=Left**; direction rotations use
`(godir±1)&3`. In the mover, the player position (`+0x1c`, `+0x20`) is a plain
**integer pixel** count (a 1-pixel step adds ±1), and godir is 16.16 at `+0x2c`
(`>>16` = 0..3). (This is distinct from the smooth-render field noted earlier.)

**Speed = a spent budget.** Player `+0x74` holds a movement budget. Each moving
tick `sub_41F29B` adds `baseSpeed(+0x70) × frameDelta[0x464958] / [0x46494C]`
(times disease factors: `÷3` if `+132` set, `×3/2` if `+133`/`+137` set), then
`sub_41EC84` spends it in a `while (budget>0) { …one pixel…; budget −= 100 }`
loop. The remainder is **carried** to the next tick (never reset) → fractional
speed. `baseSpeed = getvalue(42)` (= our `start_speed`); each skate adds
`getvalue(90)`. `[0x46494C] = 1000/getvalue(30)` = 50 ms at the nominal 20 Hz,
and `[0x464958]` is the elapsed-ms frame delta, so at fixed 20 Hz the per-tick
budget is simply `baseSpeed (+ skates)`, i.e. `baseSpeed/100` px per tick.
Passability `sub_41E5C3(tx,ty)` = *(no bomb at tile)* **and** *(wall type 0)*.

**Per-pixel resolution** (offsets from tile centre: `along` on the facing axis,
`perp` on the lane-perpendicular):

- ahead passable, or still behind centre (`along<0`) → step forward 1 px; if
  off-lane (`perp≠0`) also step 1 px toward the centreline (diagonal glide).
- blocked ahead, `perp≠0` → if the tile toward the lean **and** the tile beyond
  it (lean+forward) are both passable, step 1 px perpendicular — round the
  corner. **No distance threshold**: the assist depends only on which side of
  the tile centre you are on (so up to a half-tile).
- blocked ahead, centred (`perp==0`), past centre (`along>0`) → settle back
  toward the tile centre; exactly centred → stop.

Ported faithfully into `move_player` (sim.cpp); the old guessed
`corner_threshold` is deleted. Verified by `tests/test_move.cpp` (speed 93 px /
10 ticks matches the budget rule; 4-px lean rounds a corner the old 9-px gate
would have ignored; centred-into-wall stops on the tile centre).

## Disease system — CONFIRMED (`sub_41DFB6` assign, `sub_41EB13` bomb params, `sub_41F29B` effects)

The skull powerup calls **`sub_41DFB6`**: pick `rand() % 9` → **exactly 9 diseases**
(index 0..8). Each sets a byte flag at player `+132+index` (except index 7,
swap, which is special-cased), sets `+120`=has-disease, `+124`=duration =
`frameScale × getvalue(130+index)`, `+128`=`getvalue(129)` (freshness). On assign
it plays sound `50*index+3000` (per-disease voice) 1/3 of the time, else 2300.

| idx | flag | disease | effect (site) | duration id |
|---|---|---|---|---|
| 0 | +132 | **slow** (molasses) | move budget `÷3` (`sub_41F29B` 23436) | 130 |
| 1 | +133 | **fast** (hyper) | move budget `×3/2` (23438) | 131 |
| 2 | +134 | **constipation** | can't drop bombs (gates +56 at 23310; `return 0` at 10585) | 132 |
| 3 | +135 | **diarrhea** | forces auto-drop every frame (sets +56 at 23279) | 133 |
| 4 | +136 | **short flame** | dropped-bomb flame forced to 1 (`sub_41EB13` v9=1) | 134 |
| 5 | +137 | **super/ebola** | fast **and** auto-drop (grouped with +133 and +135) | 135 |
| 6 | +138 | **short fuse** | dropped-bomb fuse `÷3` (`sub_41EB13` v10/=3) | 136 |
| 7 | — | **swap** | swaps (x,y) with a random other live player; no flag | 137 |
| 8 | +140 | **reversed** | godir `(g+2)&3`, humans only (`+16 != 1`, at 23049) | 138 |

Move-budget order (23436-23438): `v91 = base; if(slow) v91/=3; if(fast||super) v91 = 3*v91/2`.
Bomb flame comes from player `+0x57` (flame stat), overridden to 1 by short-flame,
or to `max(cols,rows)` by goldflame `+0x5e`; fuse from `+0x48`, `÷3` by short-fuse.

Config from VALUELST (ids read in the resolution-init):

| id | name | value | meaning |
|---|---|---|---|
| 120 | diseases_can_be_destroyed | 1 | skull token burns like any powerup |
| 121 | diseases_are_time_limited | 1 | wears off after its duration |
| 122 | diseases_will_recycle | 0 | does **not** drop as a powerup when it leaves |
| 123 | diseases_multiply | 1 | on contact both keep it (`dword_464A78`: source cleared only if 0) |
| 124 | diseases_are_curable | 1 | a fresh powerup can cure |
| 125 | diseases_cure_chance | 10 | 1-in-10 cure per fresh powerup |
| 129 | disease_freshness | 10 | ticks a disease must age before it can pass again |
| 130–138 | durations | 300 each | 15 s at 20 Hz (nine values = nine diseases) |

**Contagion** (`sub_41F29B` ~22950): a diseased player overlapping a non-diseased
player (`|dx| ≤ tileW-10 && |dy| ≤ tileH-10`) copies all 9 flags + duration to
them; since multiply=1 the source keeps it. Receiver gets freshness `+128`. No
sound is emitted, so contagion is silent.

**Timer** (`sub_41F29B` ~22928): the disease field `+120` counts **up** by the
frame delta each tick; when it exceeds the duration `+124` the disease is cured
(`sub_41DF4C`). Our sim uses an equivalent per-tick countdown. Freshness `+128`
counts down by 1/tick and gates re-spreading.

**Visual — CONFIRMED (`sub_41F29B` ~23252):** a diseased player's sprite is
drawn with a **random colour each frame** while a disease bit is set:
`if (diseaseWord & 8) draw(x, y, rand() % 10, sprite)` — i.e. the body strobes
through the ten player palettes. (This is why, with no such indicator, only the
speed diseases were noticeable in-game.) Ported to game/main.cpp as an
`SDL_SetTextureColorMod` colour strobe on diseased players; the disease roll
itself is uniform across all nine (verified empirically).

**Pickup dispatch** (`sub_41E21E`, powerup type at `+4`): every pickup first rolls
a cure — if curable, `rand() % cure_chance == 0` clears all diseases (`sub_41DF4C`)
— then applies the powerup. Case 2 (skull) = `sub_41DFB6(p, 1)` → **1** disease
with sound; case 0xB (purple skull, SuperDisease) = `sub_41DFB6(p,1)` then
`sub_41DFB6(p,0)` ×2 → **exactly 3** diseases, only the first plays a voice.
On assign, the voice is `2300` two-thirds of the time, else `50*idx + 3000`.

## Spooger — CONFIRMED (`sub_41F29B` spooge branch)

The Spooger powerup (`+93`) lays a **line of bombs**. The whole bomb-drop block
is **edge-gated** — the frame starts with `+54 = +56; +56 = 0` and the block
runs only on `+56 && !+54` (button down this frame, not last = a rising edge).
So it fires once per press, not while held. Within that single frame the spooge
branch (`+93 && !auto-drop && bomb underfoot`) walks one tile at a time in the
facing direction (`dword_45BECC/45BEDC[godir]`) placing a bomb per tile via
`sub_41EB13`, laying the **entire run in that one frame**. It stops on the first
of: a bomb already on the tile (`sub_421CB5`), a powerup (`sub_42542D`), a
blocked tile (`!sub_41E5C3`), or running out of bombs (`max_bombs ≤ placed`).

Because a bomb must already be underfoot, it takes **two presses**: the first
(empty tile) drops one underfoot via the normal branch; the second (now standing
on it) fires the run ahead. Ported to `spooge_ahead` in sim.cpp with the tick
drop logic edge-gated to match; bombs carry the player's flame/fuse plus disease
overrides. Verified by `tests/test_spooge.cpp` (single-press = underfoot only,
bomb-count limit, wall stop, column direction, short-flame carry, non-spooge).

## Bomb machine — CONFIRMED (`sub_42331C`, the per-tick bomb updater)

Read 2026-07-03 from the batch pseudocode. One 100-slot bomb array
(`dword_46220C`); the relevant struct fields: +0 state (0 dead, 1 live,
2 **dud**), +4 **kind** (0 regular, 1 trigger, 2 jelly — the name table
`off_45BE44` = {"regular","trigger","jelly",...} drives the "bomb %s green"
sequence pick), +28/+32 x/y, word +44 direction (godir), word +46 motion
state (0 rest, 1 sliding/kicked, 2 flying, 3 carried), +68/+74 fuse
elapsed/duration, +72 tiles-travelled counter, +112 speed, +116 movement
budget (same +=speed / spend-100-per-px scheme as the player stepper).

- **Kind is EXCLUSIVE and set at creation** (`sub_41EB13`): jelly flag
  (player +96) sets kind 2, then the trigger branch (player +95, gated by a
  per-player trigger allowance +85 < +86) OVERRIDES with kind 1. So a
  trigger+jelly player lays trigger (non-bouncy) bombs. The trigger allowance
  counter also means trigger bombs are a limited supply in the original —
  NOT yet modeled in our sim (roadmap).
- **Fuse pause — CONFIRMED** (closes the last "still guessed" row): the fuse
  only advances when `state != 2 (dud) && motion != 2 (flying) && motion != 3
  (carried) && kind != 1 (trigger)`. Sliding (kicked) bombs DO tick.
- **Kicked slide, blocked**: per-pixel loop; on blocked-ahead + at/past tile
  centre: snap to centre, budget zeroed, then **jelly (kind 2): direction =
  (dir + 2) & 3 — reverses and KEEPS its moving state** (sound 135
  "bombboun"), so it ping-pongs off obstacles until something stops it;
  non-jelly: motion state cleared (sound 130 "bombstop"). A bomb sliding
  into flame explodes (`sub_42708D` check) — not yet in our sim (kick audit).
- **Flight (punched/thrown)**: per-tile hops; landing attempts begin once
  +72 >= 3 tiles (matches our 3-tile launch + 1-tile hops). At each landing
  boundary, **a jelly bomb first rolls a random ±90° veer: chance
  1-in-getvalue(667) (VALUELST 667 = 3), side = rand()%2** — i.e.
  `dir = (dir - 1 + 2*(rand%2)) & 3`, rolled only when the unwrapped tile is
  in-bounds; then the normal landing checks run (hop again if occupied).
  Hop sound = 160 ("bmdrop3"), which we already map to BombBounced.
- **Dud pointers for roadmap item 3**: state 2 renders the "... dud" anim
  suffix and lasts getvalue(323) (VALUELST 323 = 120 ticks = 6 s), then the
  bomb returns to state 1 with its anim reset; VALUELST 322 = 3 sits next to
  it (dud chance denominator candidate — verify at the placement site).
- Goldflame detail: bomb creation reads a goldflame FLAG (player +94) and
  uses max(gridW, gridH) as the reach — our `flame = 99` is observably
  equivalent but not literal (audit later).

Ported: `Tuning::jelly_turn_chance` (id 667), exclusive kind at placement,
jelly reverse-on-block in `BombSystem::slide`, jelly veer in
`BombSystem::fly`, events BombStopped (130) / JellyBounced (135). Tests:
`tests/test_jelly.cpp`. Golden scenario B constants refreshed for this
deliberate behaviour change (this section is the citation).

## Still guessed — not yet extracted from the binary

| Constant | Current value | Status |
|---|---|---|
| (none — fuse pause confirmed via `sub_42331C`, 2026-07-03) | | |

## Getting exactness where it matters (recommended path)

Deep per-function decompilation is best done on a persistent machine. Run
Ghidra headless on `BM95.EXE` locally (minutes are fine there), export the
decompiler output for the handful of functions of interest (movement,
explosion/flame, disease), and hand the C-like output over as a **facts
source**. We then translate the observed constants and behavior into the
clean-room sim — we do not paste decompiler output into the repo.

## Clean-room policy

- The repo contains no Interplay/Konami code or assets, and no disassembly.
- We extract *facts* (numbers, addresses, algorithm descriptions) — facts are
  not copyrightable — and implement independently in our own code.
- `BM95.EXE`, raw disassembly, and decompiler output are working material kept
  outside the repository.
