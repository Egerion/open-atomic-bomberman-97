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

## ANI per-step timing (STAT HEAD u16) — CONFIRMED INERT (`sub_41CD03` loader, `sub_41DAA7` player)

Each `STAT` block's `HEAD` (46 bytes) is zero except a leading u16 that only
ever holds **30 (0x1E)** or **0xFFFF**. Resolved 2026-07-04 ("devam" #6): **the
engine never reads this field. It is vestigial authoring metadata and has no
effect on animation pacing.** The old "0x1E = step / 0xFFFF = hold-or-loop"
guess is wrong.

**Loader `sub_41CD03` (0x41CD03).** Parses each `SEQ ` into a 60-byte sequence
descriptor in the global array based at `dword_461B5C` (bump-allocated 60 bytes
at a time by `sub_41C7A5`). Descriptor layout: `+52` = statecnt (number of
`STAT` steps), `+56` = pointer to the step array. Each step is **20 bytes**
(`sub_41C707` bump-allocates them): `+0` = the STAT HEAD field0 (the 0x1E /
0xFFFF value), `+4`/`+8` = the next two HEAD u16 (always 0), `+12` = frame
count, `+16` = pointer to the frame array (each frame is 12 bytes: absolute
frame index, dx, dy). So the timing u16 lands at **step record +0**.

**Accessors (the whole "ANI player" surface).**
- `sub_41DA5C(seq)` → statecnt (`descriptor+52`).
- `sub_41DAA7(seq, counter)` → the frame index of step `counter % statecnt`
  (reads only step `+16`, the frame pointer). This auto-looping modulo IS the
  entire frame-selection logic.
- `sub_41DB41(seq, counter, &dx, &dy)` → the same step's blit offsets.

**Proof it is inert.** Across the full 1134-function decompile, the *only*
offset ever dereferenced from a 20-byte step record is `+16` (the frame
pointer). Step `+0` (timing), `+4`, `+8`, and `+12` (frame count) are never
read by any code path — grep of every `... + 56)` step-array access confirms
only `+ 16` and the bare pointer (freed in the ANI teardown at `sub_41D3B5`).

**How pacing actually works.** Each entity owns its own animation counter and
advances it itself; the displayed frame is `counter % statecnt`. The cadence is
hardcoded per entity type, never data-driven:
- Movers advance the counter **once per pixel-step** inside the movement budget
  loop — rover/ghost `sub_401B5C`: `budget += speed*dt/msPerFrame + 100; while
  (budget>0){ ++entity[+48]; budget-=100; ... }`.
- Human players (`sub_41F29B`, builds `"walk %s"`/`"stand %s"`/`"kick %s"`)
  index with a **16.16 fixed-point walk phase shifted `>>16`**, advanced by
  distance travelled.
- Stage extras (`sub_401B5C`-area, `"extra arrow %s"` etc.): arrow every frame,
  **conveyor `counter/3`** (a literal divide-by-three — proof the ANI file does
  not carry the rate), trampoline plays once then holds.

**Data (all 95 ANI, 249 sequences, 3416 steps).** field0 ∈ {`0x1E` ×2761,
`0xFFFF` ×655}. 0xFFFF **never appears as a sequence's first step** (0×), and is
mostly mid-sequence (607 middle, 48 last) — so it is not a terminal/loop marker
either. Sample sequences: `walk north` all 0x1E; `bomb regular green` 1× 0x1E
then 17× 0xFFFF; `die green 1` mixes 40× 0x1E / 43× 0xFFFF. (Provenance:
`DATA/ANI/*` STAT parse; addresses from `pseudo.c`.)

**Port implication.** The renderer must pace animation from a logical counter
and select `counter % statecnt`; `SeqStep::head0` must never influence pacing.
`Renderer::draw_anim` already does exactly `steps[counter % steps.size()]` and
ignores `head0`, so our model matches. Open follow-up (roadmap #13): the human
walk *phase* is distance-driven (fixed-point) in the original vs our
once-per-moved-tick increment — a cadence nuance, independent of this field.

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

## Screen geometry / field placement — CONFIRMED (`sub_42647A`, `sub_426524`/`sub_42655F`)

The play-field metrics are set in `sub_42647A`: grid `15x11` (dword_4648AC/4648B4),
tile `40x36` (dword_4648A4/4648A0), and the pixel origin
`originX = (screenW-600)/2`, `originY = screenH-412` — for 640x480 that is
**(20, 68)** (dword_464898/4648A8; screen dims dword_464A70/464A6C = 640/480).

Tile→screen converters: `sub_426524(tx) = 40*tx + 20 + originX` = tile-**centre**
X; `sub_42655F(ty) = 36*ty + 36 - 1 + originY` = tile-**bottom** Y. So sprites
blit at (tile-centre-x, tile-bottom-y) minus their ANI hotspot — a bottom-centre
anchor. The inverse sub-tile helpers `sub_426599`/`sub_4265EB` return the offset
from the tile centre (the movement stepper's `sx`/`sy`).

Port note: our renderer stores the player/bomb position as the tile CENTRE and
reaches the bottom anchor by adding `kTileH/2`, so the only correction needed was
`kFieldOriginY` 64 → **68** (`docs`/`renderer.hpp`); originX 20 and the 40x36 /
15x11 metrics already matched. (Resolved 2026-07-04 while reviewing Ege's
"graphics shifted down" report — the field was actually 4 px too *high*.)

Per-element blit anchors (all via the draw queue `sub_415920`/`sub_415A9F`,
which subtract the sprite's ANI hotspot): floor + brick/solid tiles
(`sub_4022A1`) and bombs (`sub_42331C`) blit at (tile-centre-x, tile-bottom-y);
**powerups** (`sub_424F89`, seq `"power %s"` via `off_45BE50[type]`) also blit
at (tile-centre-x, tile-bottom-y) and are ANIMATED (frame = `sub_41DAA7(seq,
counter)` where `counter` is the powerup's OWN per-tile word at struct `+48`,
bumped once per draw — NOT a global tick or a fixed +48 offset). The type→name
table `off_45BE50` = {bomb, flame, disease, kicker, skate, punch, grab, spooge,
goldflame, trigger, jelly, disease3, random, clog, ...} — our `kPowerNames[]`
matches it 1:1 (verified 2026-07-04). Floor art is POWERS.ANI (40x36, hotspot
(20,35), CIMG type 4, cyan-tile background — no red frame; the red border on the
POW*.PCX menu icons is that format's transparent key, and red is also used for
interior detail so the PCX must not be raw-blitted on the floor). Our animated
path draws POWERS.ANI; the static POW*.PCX tile-fill is a robustness fallback;
**the shadow** blits at the player's OWN anchor `(v111+28, v111+32)` with no
offset (its (14,16) hotspot centres it). Our renderer had a stray `+8` on the
shadow (removed 2026-07-04) and draws powerups top-left (≈1 px off, animation
aside). (Resolved while reviewing Ege's "shadow/powerups/bombs a bit too high".)

## Player colour remap — CONFIRMED (`sub_414A65`, builds the `.rmp` tables)

The green armour of every pre-rendered player sprite (walk/stand/bombs/flames/
deaths, all authored in "green") is retargeted per player at load time by
**`sub_414A65`** (0x414A65), which bakes a 256-entry remap table (`%u.rmp`,
`dword_460564[player]`). Init loops all ten players building the args from
VALUELST: `getvalue(200+5k)=R%`, `getvalue(201+5k)=G%`, `getvalue(202+5k)=B%`,
then `sub_414A65(k, R%, B%, G%, 0)` (arg order a2=R%, a3=B%, a4=G%). Per source
palette entry `[R,G,B]`:

```
if (G > R && G > B) {                 // green-dominant (strict, no margin)
    lum      = G;
    baseline = (R + B) / 2;           // v33
    excess   = lum - baseline;        // v32 - v33
    outR = R% * excess / 100 + baseline;   // then snap to nearest palette entry
    outG = G% * excess / 100 + baseline;
    outB = B% * excess / 100 + baseline;
} else outC = C;                      // non-green pixels unchanged
```

The **baseline `(R+B)/2` is preserved**, so the sprite's casing/shading survives
the tint — a white player {100,100,100} yields a *shaded* light bomb, not a flat
white one. Ported to `libs/game/src/sprites.cpp recolor_image` (truecolour, so
the nearest-palette snap is dropped). Fixes the trigger-bomb "white blob": the
desaturated TRIGBOMB art (mean 58,150,41, baseline ~49) exposed our earlier
approximation (lum=G, no baseline, fabricated glint) which blew it to pure white
for the white player. Render-only; no golden impact. (Resolved 2026-07-04 from
Ege's white-blob trigger-bomb photo.)

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
  now modeled (see "Trigger allowance" below).
- **Fuse pause — CONFIRMED** (closes the last "still guessed" row): the fuse
  only advances when `state != 2 (dud) && motion != 2 (flying) && motion != 3
  (carried) && kind != 1 (trigger)`. Sliding (kicked) bombs DO tick.
- **Kicked slide, blocked**: per-pixel loop; on blocked-ahead + at/past tile
  centre: snap to centre, budget zeroed, then **jelly (kind 2): direction =
  (dir + 2) & 3 — reverses and KEEPS its moving state** (sound 135
  "bombboun"), so it ping-pongs off obstacles until something stops it;
  non-jelly: motion state cleared (sound 130 "bombstop"). A bomb sliding
  into flame explodes (`sub_42708D` check) — now ported (see "Kick nuances").
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
  equivalent but not literal (now literal — see "Goldflame literalness" below).

Ported: `Tuning::jelly_turn_chance` (id 667), exclusive kind at placement,
jelly reverse-on-block in `BombSystem::slide`, jelly veer in
`BombSystem::fly`, events BombStopped (130) / JellyBounced (135). Tests:
`tests/test_jelly.cpp`. Golden scenario B constants refreshed for this
deliberate behaviour change (this section is the citation).

## Dud bombs — CONFIRMED (`sub_422EDE` roll, `sub_422C13` gate, `sub_42331C` window)

Read 2026-07-03. Bomb state (+0): 0 dead, 1 live, **2 = dud (fizzling)**.

- **Roll site = bomb creation** (`sub_422EDE`): only when the kind is REGULAR
  (`!a4` — trigger and jelly never fizzle) and not a network game. Gated by a
  global timer (`dword_464AF4`): when open, the gate re-arms FIRST
  (`sub_422C13`: gate += getvalue(320) + rand() % getvalue(321), VALUELST
  320 = 180, 321 = 180 — one dud opportunity per ~9–18 s) and then the bomb
  duds on `rand() % max(1, getvalue(322)) == 0` (322 = 3). The gate is also
  armed once at match init (`sub_422C7A`).
- **Fizzle window** (`sub_42331C` tail): the "bomb regular green dud"
  sequence (DUDS.ANI) renders while the always-running anim counter stays
  within getvalue(323) = 120 ticks (6 s); past it the bomb returns to state 1
  with its anim reset. The fuse gate skips state 2 entirely, so the fuse
  RESUMES where it froze — total lifetime = fuse + fizzle.
- The original's gate compares wall-clock-ish time (why network games skip
  duds); our deterministic port measures the same 180-frame values in ticks
  (`State::dud_gate`, hashed) — semantics identical at 20 Hz, and
  determinism holds where the original had to disable the feature.
- Chain explosions still set off a fizzling dud (the explosion path ignores
  the dud state).

Ported: `Bomb::dud_left` (hashed) + `State::dud_gate` (hashed), roll in
`BombSystem::place`, freeze in `tick_fuses`, DUDS.ANI wired through
AssetStore/SequenceSet/Renderer. Tests: `tests/test_dud.cpp`. Golden fully
recaptured (hash layout gained two fields; setup consumes one arm draw).

## Head hit — CONFIRMED (`sub_421F7E`, stun countdown in the player updater)

Read 2026-07-03. When a flying bomb lands on a live player:

- **Stun = hardcoded 16 ticks** (`a1[29] = 16`, the word at +58): the player
  updater decrements it each tick, blocks the whole turn while positive, and
  clears action-state 3 when it reaches zero. NOT a VALUELST id — our old
  `head_stun_frames = 20` guess corrected to 16 and marked confirmed.
- **Drop count** = getvalue(670) + rand() % max(1, getvalue(671)) — the
  modulus is 671's value itself (we previously used %(671+1); fixed).
- **Kind selection**: per drop, up to 200 tries of `kind = rand() % 15`,
  accepted when the player's per-kind count `player[+86+kind]` exceeds the
  VALUELST start-with baseline (getvalue(50+kind)); the kind is decremented
  (`--player[+86+kind]`) and its token scattered. Uniform over KINDS with
  surplus, not over accumulated tokens. The `+86+kind` byte layout maps kind
  directly (86 bombs, 87 flame, 89 kick, 90 skate, 91 punch, 92 grab, 93
  spooger, **94 goldflame**, 95 trigger, 96 jelly) — the SAME order as our
  `PowerupType` enum, so a kind cast is faithful. **Goldflame (kind 8)
  IS included** (start-with id 58 = 0 ⇒ a set flag is surplus) — see 2026-07-04
  update below.
- **Scatter placement** (`sub_4255B2` via `sub_425BED`, which discards the
  position argument): the token lands on a RANDOM tile — `x = rand()%W,
  y = rand()%H`, inner budget 100 rolls (solid/brick just re-roll), outer
  budget 100 attempts (a bomb, ANY powerup record, or a live PLAYER on the
  tile burns an attempt — see "Scatter occupancy test" below for the exact
  predicate; flame does NOT block placement), token LOST if everything
  fails. Our old nearest-free-spiral was a guess; replaced.
- Side find: a fully-boxed-in idle player rolls a panic anim state
  `20 + rand % getvalue(330)` — cosmetic (presentation `panic_lcg_`, not the
  sim). getvalue(330) = 13, CONFIRMED 2026-07-04 (see "Final in-game 1:1 gaps").

Ported in `PowerupSystem::head_hit`/`scatter`; `tests/test_sim.cpp` covers
stun/scatter and (new) the goldflame drop. **UPDATE 2026-07-04:** goldflame
(kind 8) added to the `surplus()` roll — the previous "skipped for now" note is
resolved. This changes the per-hit RNG draw count ONLY when the victim has
goldflame, so **golden B must be recaptured**; A/C/D/E stay byte-identical (see
"Final in-game 1:1 gaps" §4 for the per-scenario reasoning). The earlier
"Golden verified UNCHANGED" claim held only while goldflame was excluded.

## Powerup pickup dispatcher — CONFIRMED (`sub_41E21E`)

Read 2026-07-03. Flow: cure roll first (curable && 1-in-getvalue(125)), then a
kind switch over the token's `+4`, then a common tail. Player bytes +86..+96
are the per-kind counts (86 bombs, 87 flame, 89 kick, 90 skate, 91 punch,
92 grab, 93 spooger, 94 goldflame, 95 trigger, 96 jelly).

- **Random (case 0xC)**: `token.kind = rand() % 12` — Random itself is
  excluded by the modulus — retried up to 200 times while
  `forbidden[kind]` (the scheme -P table), then control jumps BACK into the
  switch and the rolled kind applies normally (a skull or super-skull is a
  legal outcome). Ported into the pickup path in `simulation.cpp`
  (`State::forbidden` now carries the scheme flags; one RNG draw per
  attempt). Tests: `tests/test_random.cpp`.
- **Mutual exclusions** via the remove helper `sub_41E16A`: punch removes
  trigger; grab removes spooger; spooger removes grab; trigger removes punch
  AND jelly; jelly removes trigger. NOT yet in our sim (roadmap item 11).
- **Trigger pickup** also zeroes the live-trigger-bomb counter (+85); bomb
  creation lays trigger kind only while `+85 < +86 (max bombs)`.
- **AWESOME cadence**: pickup counter +101 (not incremented by skulls):
  voice at 7, then every 5th; counter wraps to 7 past 50. (Our SoundDirector
  had "every 3rd" — fixed, with the wrap.)
- **Pickup sounds**: normal kinds play the 400 voice group; **jelly plays
  135 ("bombboun")** instead — fixed in SoundDirector.
- Per-kind limits clamp `player[86+kind]` against getvalue(550+kind) in the
  common tail — matches our `PowerupSystem::apply` clamping.

## Trigger allowance — CONFIRMED (`sub_41EB13` placement, `sub_41E21E` case 9)

Read 2026-07-04 ("devam" #9). A trigger player may only lay a limited number of
live trigger bombs, capped by their bomb count.

- **Placement gate** (`sub_41EB13`, the bomb-params function): kind starts 0;
  jelly (`+96`) sets kind 2; then the trigger branch OVERRIDES only while the
  allowance holds:
  `if (player[+95] /*trigger flag*/ && player[+85] /*live count*/ < player[+86]
  /*max_bombs*/) { kind = 1; ++player[+85]; }`.
  So the allowance VALUE per state is exactly **max_bombs** (`+86`), and each
  trigger placement consumes one (`++[+85]`).
- **Exhaustion behaviour**: when `+85 >= +86` the trigger branch is skipped —
  the bomb is **NOT blocked**, it simply stays kind 0 = a **normal timed bomb**
  (fuse from `getvalue(41)`, subject to the dud roll like any regular bomb).
- **Refill**: the counter `+85` is written in exactly three places across the
  whole 1134-function decompile — `= 0` at player spawn (`sub_...23910`), the
  `<` test + `++` at placement (`sub_41EB13`), and `= 0` on **Trigger pickup**
  (`sub_41E21E` case 9, first statement: `*(_BYTE*)(a1+85) = 0`). There is **NO
  decrement anywhere** — not on detonation. So the budget is a per-pickup
  lifetime allowance: a Trigger pickup refills it to a fresh `max_bombs`
  trigger placements; once spent, further placements are normal bombs until the
  next Trigger token is collected. (Provenance: exhaustive `+ 85)` grep.)
- Trigger pickup (case 9) also `++[+95]` (flag) and evicts punch (`sub_41E16A(_,5)`)
  and jelly (`sub_41E16A(_,10)`) — matches our existing mutual-exclusion port.

Ported: `Player::trigger_placed` (hashed), gated in `BombSystem::place`
(`make_trigger = trigger && trigger_placed < max_bombs`, then `++trigger_placed`;
exhausted ⇒ normal timed bomb), refilled in `PowerupSystem::apply` Trigger case.
No new RNG draws. Tests: `tests/test_trigger_allowance.cpp`. Golden must be
recaptured (new hashed field; a downgraded trigger bomb now also participates in
the dud roll it previously skipped).

## Goldflame literalness — CONFIRMED (`sub_41E21E` case 8, `sub_41EB13` reach)

Read 2026-07-04 ("devam" #10). Goldflame is a **flag**, and the giant blast is
computed at drop time — it is not a stored flame stat.

- **Pickup** (`sub_41E21E` case 8): `++player[+94]` sets the goldflame flag
  (byte +94), plays voice 400, and is otherwise a normal pickup (its `550+8`
  limit clamps the byte in the common tail).
- **Drop-time reach** (`sub_41EB13`): the flame reach `v9` is derived per bomb —
  `v9 = player[+87] /*flame stat*/; if (player[+136] /*short-flame*/) v9 = 1;
  if (player[+94] /*goldflame*/) v9 = (gridW <= gridH) ? gridH : gridW;`. So the
  reach is literally **max(cols, rows)** (`dword_4648AC`/`dword_4648B4` = 15/11 ⇒
  15). **Ordering matters**: short-flame sets 1 FIRST, then goldflame OVERRIDES
  it — goldflame **beats** short-flame.

Ported: `Player::goldflame` (hashed) replaces the old `flame = 99` sentinel; set
in `PowerupSystem::apply` Goldflame case; `BombSystem::place` computes
`b.flame = short_flame ? 1 : flame; if (goldflame) b.flame = max(kGridWidth,
kGridHeight);`. Tests: `tests/test_goldflame.cpp`. Golden must be recaptured
(stored `flame` value 99 → 15, plus the new flag).

RESOLVED 2026-07-04 (the deferred head-hit follow-up, see "Goldflame on a head
hit" below): the original also rolls goldflame as a droppable kind on a head hit
(kind 8 = byte +94, accepted when +94 > start-with). Now wired into
`head_hit`'s `surplus()`; `remove()`/`scatter()` already handled Goldflame. This
shifts the head-hit kind-roll ACCEPTANCE only for a victim that HAS goldflame —
so golden B (players can pick up hidden goldflame and be head-hit) must be
recaptured; A/C/D/E are byte-identical (no goldflame victim on a head hit).

## Kick nuances — AUDIT (`sub_42331C` kicked-slide, `sub_42708D`, `sub_42464B`)

Read 2026-07-04 ("devam" #8). Three gaps audited against the kicked-slide loop.

1. **Slide into flame explodes — CONFIRMED, FIXED.** The per-pixel slide loop
   in `sub_42331C` calls `sub_42708D(x,y)` (flame-at: returns the flame cell
   ptr if `cell[+0] != 0`) at the stepped position; on a hit (and the flame is
   not the "just-placed guard" kind 9) it queues the bomb's detonation
   (`sub_423209(bomb, 0)`) and stops. Our slide previously sailed through fire.
   Ported: `BombSystem::slide` now detonates via `FlameSystem::explode(index)`
   when the bomb occupies a lit tile (`slide` takes the bomb index for this).
2. **Mid-slide "re-steer" is a DIRARROW/conveyor, NOT a player — NO CHANGE.**
   The slide's re-steer (`v70 = sub_405654(tileX,tileY); if (v70 && !v70[1])
   bomb[+44] = v70[+44]`) reads the **level-actor registry** `dword_45E0A8`
   (allocated 152×100 at `sub_404D16`), whose entries are stage objects parsed
   from the level file: **type 0 = DIRARROW**, type 2 = conveyor, type 3 =
   trampoline, type 1 = warphole (registration at `sub_...6970-7086`, strings
   "dirarrow"/"conveyor"/"trampoline"/"warphole"; godir at actor `+44`). It is
   **not** the human-player array (`dword_461BC4`). So the original has no
   player-based bomb re-steer — a sliding bomb adopts a **directional-arrow
   tile's** direction. We do not model dirarrows/conveyors yet, so there is
   nothing faithful to add; this belongs to ROADMAP #7 (conveyors/trampolines).
   (This corrects the task's "resting player re-reads godir" premise.)
   Sidenote: `sub_4230A5` (the slide passability check) tests walls, bombs,
   bricks, powerups and the warphole actor (type 1) — it does **not** test for
   players, so in the original a sliding bomb passes THROUGH players. Our slide
   currently treats a live player on the next tile as a blocker; left as-is
   (pre-existing, out of this audit's scope) and noted for a future pass.
3. **Kicked-bomb speed = fixed VALUELST id 300 — CONFIRMED faithful.** The kick
   handler `sub_42464B` sets bomb `+112 = getvalue(300)` (id 300 = 1000; punch
   `sub_...25943` sets `+112 = getvalue(301)` = 1300). The `getvalue(base+190)`
   seen at pseudo.c ~23422/23447 is the **conveyor** contribution to *player*
   movement (id 190 = 250, "low"), gated on actor type 2 — unrelated to kicked
   bombs. Our `Tuning::kicked_bomb_speed` (id 300) already matches; no change.

Ported: flame-into-explode in `BombSystem::slide` (now index-based). Tests:
`tests/test_kick_nuances.cpp`. Golden: the flame-explode path only triggers when
a kicked bomb meets flame; golden scenarios that never do stay byte-identical,
but recapture after the trigger/goldflame hash-layout change regardless.

## Punch glove feedback — CONFIRMED (`sub_424A50` handler, `sub_41F29B` dispatch)

Read 2026-07-04 ("devam" #23, control/audio fidelity). The punch glove is the
per-player action flag **+91** (set by `sub_41E21E` case 5). Its handler is
`sub_424A50`, dispatched from the player updater `sub_41F29B` at the action
edge-gate `if (+57 && !+55)` (button down this frame, not last = one fire per
press) via `if (+91 && !+56 && sub_424A50(p)) { p[+0x4e] = 2; }` — setting the
punch anim state 2.

- **The glove ALWAYS swings.** `sub_424A50` returns **1 unconditionally**, so
  the caller sets punch anim state 2 on **every** press regardless of whether a
  bomb is in front. This is the key fact: pressing punch with an empty tile
  ahead still plays the full swing animation.
- **Launch + SFX are gated on a bomb being present.** Inside `sub_424A50`:
  `v9 = sub_422E48(tileAheadX, tileAheadY)` (scan the 100-slot bomb array for a
  resting/kicked bomb on the tile ahead — excludes motion states 2 flying / 3
  carried); **`if (v9) { sub_424987(v9, facing); sub_427961(150); }`**. So the
  bomb is thrown (`sub_424987` → `sub_41013F` spawns the flying-bomb actor 0x36)
  and SOUNDLST 150 ("punching a bomb") plays **only when a bomb is actually
  hit**. An empty swing is animated but **silent** and launches nothing.
- `sub_427961(id)` random-picks across the contiguously loaded slots from `id`
  up (it scans forward while the slot ptr is non-zero); with 150/151 both
  loaded, `sub_427961(150)` plays a random of {150,151}. (Provenance:
  `sub_427961` @ 0x427961.)

Ported: `BombSystem::try_punch` now emits `BombPunched` on **every** press to
drive the swing pose, sets `launch()` only when a bomb is ahead, and flags the
hit in the (unhashed) event `data` (1 = bomb launched, 0 = empty swing).
`SoundDirector` plays 150/151 only when `ev.data` is set — mirroring the SFX
being inside `if (bomb ahead)`. Previously `try_punch` returned early when no
bomb was ahead, giving no pose and no sound on an empty swing (the reported
bug). **No golden impact** — this only adds/moves event emissions and reads
`data` in the presentation layer; `State`/`Player` are unchanged and events are
not hashed.

## Throw is silent — CONFIRMED (`sub_41F29B` +37 carried-release block; SOUNDLST scan)

Read 2026-07-04 ("devam" #23). Throwing a carried (grab-glove) bomb plays **no
sound** in the original.

- **Throw path.** A carried bomb rides in the player actor pointer field **+37**
  (dword). Each tick `sub_41F29B` releases it (LABEL_246 block): `if (p[+37]) {
  v73 = p[+37]; if (notBlocked) { reposition actor at player x/y; sub_424987(v73,
  facing); p[+37] = 0; } }`. `sub_424987` is the SAME launch primitive the punch
  uses — but here there is **NO `sub_427961` call anywhere in the block**. The
  throw is silent.
- **The "bmbthrw" sounds are dead assets.** SOUNDLST lists `172,bmbthrw1`,
  `173,bmbthrw3`, `174,bmbthrw4`, `175,bmbthrw5` (right after grab `170,grab1` /
  `171,grab2`). An exhaustive scan of every `sub_427961(N)` literal call site in
  the whole decompile shows the audio player is **never** invoked with any of
  171–175 (nor 172): the only glove-family sound calls are `sub_427961(150)`
  (punch hit, `sub_424A50`), `sub_427961(160)` (flying-bomb hop, "bmdrop3"), and
  `sub_427961(170)` (grab attach, `sub_424AF4`). So grab2 and all four throw
  sounds are loaded but never played. (Provenance: `grep sub_427961(` over
  `pseudo.c`; SOUNDLST.RES text listing.)

Ported: removed the `BombThrown → play_one_of({150,151})` mapping in
`SoundDirector` (throwing now plays nothing). Previously it borrowed the punch
**hit** sound 150/151 — the spurious "hit" the user reported on throw. Grab
(170) and punch-hit (150/151) mappings are unchanged and confirmed correct.
**No golden impact** — sound-mapping change only; the sim and its hash are
untouched.

## Per-match brick fill — CONFIRMED (`sub_4260F5`, pseudo.c ~26928)

Read 2026-07-04. The destructible-brick layout is RANDOMISED every match; the
.SCH grid only marks brick *candidates*.

- **The `:` grid cells are candidates, not final bricks.** The scheme header
  documents the grid as *"# is solid, : is brick, . is blank"* and carries a
  separate *"scheme brick density (0-100 percent)"* line (`-B,<n>`). BASIC.SCH
  ships `-B,90`.
- **Fill routine** (`sub_4260F5`, non-editor branch): walk the board ROW-MAJOR
  (`for y in 0..rows: for x in 0..cols`), read the scheme cell
  `v3 = sub_404852(x,y)` (0=blank, 1=solid, **2=brick candidate**), and
  `if (v3 == 2 && rand_() % 100 >= dword_4647A0) v3 = 0;` then write it. So each
  brick candidate becomes a real brick with **`brick_density`%** probability;
  `>=` density knocks it back to blank. `#`/`.` cells copy verbatim and, thanks
  to the `&&` short-circuit (`v3==2` tested before `rand_()`), draw **no** rand.
  `dword_4647A0` is the density, parsed from `-B` and clamped to [0,100]
  (pseudo.c 5673-5677), default 90 (pseudo.c 6691).
- **RNG source.** The fill runs during the per-board load (`sub_410B6E` →
  `sub_4260F5`), off the program's `rand_()`, which is seeded from the WALL CLOCK
  (`time_(); srand_();` in the init `sub_41095A`, and again right after). So the
  original's layout is genuinely non-reproducible run-to-run. It is NOT tied to
  any per-match seed.

**Port + regression.** Our `build_match_config` (the pre-sim match layer) was
copying every `:` to `Cell::Brick` unconditionally — the fill was skipped, so
every match on a scheme got the SAME fixed brick layout (the reported
regression). Fixed: `build_match_config` now performs the exact row-major,
per-candidate `rand()%100 >= density` fill, driven by a SETUP-ONLY LCG seeded off
the match seed — **never the sim's per-tick `State::rng`** (mirroring how
`apply_actors` resolves `-T,H` trampolines). This is a pre-sim randomisation
producing the static `cells` grid the sim treats as a fixed input, so the
per-tick RNG draw contract is untouched, while identical seeds reproduce
identical boards (the game advances the seed every round, so successive matches
now vary). **No golden impact**: the golden scenarios build `MatchConfig.cells`
by hand and never call `build_match_config`; `build_state`'s setup RNG
(powerup-hide, dud-gate arm) is unchanged. Tests: `tests/test_match.cpp`.

## Throw/punch flight lands with a sound — CORRECTED (`sub_42331C` case 2 ~25441)

Read 2026-07-04. A previous note said the THROW is silent. That is true of the
throw *instant* (no `sub_427961` in the `+37` release block, see "Throw is
silent") — but the resulting FLIGHT is not. A thrown or punched bomb is a flying
actor (motion state 2, `sub_424987` → `sub_41013F`); its per-tile flight block
`sub_42331C case 2` calls **`sub_427961(160)`** ("bmdrop3") at EVERY tile
boundary once it has travelled `>= 3` tiles — UNCONDITIONALLY, *before* the
head-hit / settle / re-hop branch. So the bomb plays 160 on each hop AND on its
final landing.

- Our `BombSystem::fly` previously emitted `BombBounced` (→ 160) only when the
  landing tile was occupied (a re-hop), and settled silently on a clear tile —
  so the throw arc was missing its landing sound (the "more sounds" the user
  remembered). Fixed: `fly` now emits `BombBounced` once at every landing
  boundary, before the occupancy branch, matching the single `sub_427961(160)`
  call site. **No golden impact** — events are not hashed and no RNG draw
  changed (the jelly-veer roll is untouched). Tests: existing punch/throw and
  jelly suites still hold; `test_golden` E's `bounces`/`rng` count `JellyBounced`
  and are unaffected.

## Diarrhea/super auto-drop × grab-glove = serial throw — CONFIRMED (`sub_41F29B` LABEL_246)

Read 2026-07-04. The auto-drop diseases and the grab/throw glove interact through
three INDEPENDENT blocks that all run in one pass (LABEL_246), which the earlier
port had collapsed into a carrying-vs-not if/else.

- **(1) Auto-drop flag.** `if (+135 /*diarrhea*/ || +137 /*super*/) { +56 = 1;
  +54 = 0; v112 = 1; }` — forces the bomb-key edge (`+56` down, `+54` not-last)
  every frame so the drop block fires each tick, and raises `v112`.
- **(2) Throw block.** `if (+37 /*carried bomb*/) { if (v112 || !+56) { launch it
  (sub_424987); +37 = 0; } }`. Not gated by constipation. So a carried bomb is
  released on key-up normally, but **`v112` (auto-drop) forces the throw EVERY
  frame**.
- **(3) Action2 block** (`+57 && !+55`): punch (+91), trigger (+95). Unchanged.
- **(4) Drop block.** `if (+56 && !+54 && !+134 /*constipation*/)`: GRAB your own
  resting bomb underfoot (`+92`), else SPOOGER line (`+93 && !v112` — suppressed
  during auto-drop), else normal DROP (`sub_41EB13`). Only THIS block is gated by
  constipation.

Net effect with **diarrhea + grab**: each tick the drop block grabs the bomb
underfoot, the next eligible tick the throw block force-throws it (v112), the
drop block then drops a fresh bomb, which is grabbed again — a grab→throw→drop
loop = the **serial throwing** the user observed. It is the ORIGINAL's behaviour,
not a bug to suppress. Constipation blocks the DROP but a carried bomb can still
be thrown (block 2 has no `+134` gate).

Ported into `player_turn` (simulation.cpp) as the same four blocks with the
forced-edge semantics (`a1_now/a1_last/drop_edge` override under auto-drop; throw
outside the constipation gate; spooger `!auto_drop`). Tests:
`tests/test_diarrhea_throw.cpp`. **Golden: scenario B must be recaptured** — its
players have the grab glove and can pick up skulls, so the grab-during-auto-drop
path (new: grab instead of a plain drop) and the every-frame carried-throw now
run, which changes both the hash and the RNG consumption (a grab draws no dud
RNG where the old plain drop did). Scenarios A/C/D/E are unaffected: A has no
players; C/D players have no grab/spooger so the drop block still just drops
(identical calls + RNG); E hides no skulls so no player is ever diseased.

## "HURRY!" callout plays SOUNDLST 2700 — CONFIRMED (`sub_42A191` ~0x42A2C4)

Read 2026-07-04. When the round timer runs low and the walls begin closing, the
original flashes a "hurry" banner AND plays a one-shot voice callout. Both live
in the per-frame game loop `sub_42A191` (~line 29487; the hurry block is around
0x42A2C4):

```c
if (v5 < sub_412135(101)) {          // timer past the hurry threshold
    ... if (sub_410578() > v7 - 5) {
        if (!dword_464984) {         // one-shot latch
            dword_464984 = 1;
            sub_427961(2700);        // <-- the "HURRY!" voice, fires ONCE
        }
        v10 = sub_41D957((int)aHurry);   // then draws the "hurry" banner
        ... sub_415920(..., v11);        // flashes it (every 4th frame: &4)
    }
}
```

- **The sound is SOUNDLST 2700.** SOUNDLST.RES labels `2700,hurry` with the
  comments *"plays when \"hurry\" flashes across the screen."* and *"2799 is last
  \"hurry up!\" sound"* — so 2700..2799 is a contiguous "hurry up!" voice block.
  `sub_427961(2700)` random-picks across the contiguously loaded slots from 2700
  (it scans forward while the slot ptr is non-zero), so the callout varies across
  the whole 2700–2799 block. `sub_427961(2700)` is the ONLY call with id ≥ 2700
  except `sub_427BFB(2800)` (a different block). (Provenance: `sub_42A191`,
  `sub_427961` @ 0x427961, SOUNDLST.RES text listing.)
- **One-shot.** `dword_464984` latches the sound to the first hurry frame; it is
  reset (`dword_464984 = 0`) when a new round starts (line 14792). Our
  `EnclosureSystem` emits `Event::Type::Hurry` once behind the `s.hurry` latch,
  which mirrors this exactly.

Ported: `SoundDirector` maps `Event::Type::Hurry → play_random_in_range(2700,
2799)` (previously the event drove only the visual banner and was unmapped).
**No golden impact** — SoundDirector reads unhashed events; no sim state, RNG
draw, or hash field changed.

## Wall-slam SFX (SOUNDLST 140–146) — CONFIRMED (2026-07-09, `sub_426818`/`sub_4278F2`)

Read 2026-07-04, **corrected 2026-07-09**. SOUNDLST.RES loads `140,clikplat` /
`141,sqrdrop2` / `142,sqrdrop4` … `146,sqrdrop8`, commented *"a solid tile
slamming in place (after \"hurry\" is displayed)"* — the per-tile wall-drop
SFX. SOUNDLST.RES's own author comment right above the block (`DATA/RES/
SOUNDLST.RES`) settles the intended playback shape directly:

```
; a solid tile slamming in place (after "hurry" is displayed)
; NOTE! the code is HARD-CODED to play one of the three below randomly.
; if you add more sounds below 142, they will not be used!!!
140,clikplat
141,sqrdrop2
142,sqrdrop4
143,sqrdrop5   <- unused (loaded but never selectable per the note above)
144,sqrdrop6
145,sqrdrop7
146,sqrdrop8
```

**2026-07-04's dismissal of the one literal `140` site was a misread — it IS
the call site.** `sub_4278F2` (pseudo.c 27879-27896) checks
`result < dword_463080` and indexes `dword_463094`/`dword_463088` before
calling `sub_411D17` (the actual sample-play primitive) — the SAME three
globals `sub_427961`'s sound-play path (pseudo.c 27902-27952) uses for its own
`result < dword_463080` / `dword_463094[]` lookup. `sub_4278F2` is therefore a
sound-play function over the SOUNDLST table, not "pointer arithmetic on an
unrelated base" as 2026-07-04 concluded — that base (`dword_462244 + 140`) IS
a SOUNDLST id, exactly as `docs/re/enclosure.md` §3/§7 (a separate, earlier RE
pass on the enclosure stepper) already documented independently:

- The enclosure stepper `sub_426818`'s ARM branch (pseudo.c 27177, `docs/
  re/enclosure.md` §2) draws `dword_462244 = rand() % 3` **once**, when the
  walls arm (`sub_410578() <= getvalue(101) - 5`) — a presentation-only pick,
  no `State::rng` draw.
- Every 250 ms drop thereafter (the cadence loop, `docs/re/enclosure.md` §3)
  calls `sub_4278F2(dword_462244 + 140)` (pseudo.c 27234) **before**
  `sub_425E9B(...)` solidifies the tile — i.e. it plays exactly ONE of
  {140,141,142}, decided once at arm time, and REPLAYS THE SAME id for every
  wall drop in that enclosure sequence (not a fresh pick per drop).

So the earlier "no call site found" conclusion was wrong: the call site is
`sub_426818` (the enclosure stepper), the callee is `sub_4278F2`, and the
mapping — SOUNDLST 140/141/142 only, 143-146 unreachable dead assets — is
independently confirmed by BOTH the disassembly path (dword_463080/
dword_463094 shared with `sub_427961`) AND the SOUNDLST.RES author's own
"hard-coded to play one of the three below" comment.

**Port fix (this pass):** the previous `WallClosed → play_one_of({140,141,
142})` mapping picked a (round-robin) id on EVERY `WallClosed` event, i.e. a
fresh pick per dropped tile — unfaithful to the "roll once per enclosure arm,
replay for the whole sequence" behaviour above. `SoundDirector` now latches
one of {140,141,142} (via a new `AudioEngine::roll` presentation-side draw,
never `State::rng`) on the FIRST `WallClosed` event since the last
`SoundDirector::reset()` (a per-round reset, matching the original's
per-arm/per-round `dword_462244` draw — `game_app.cpp`'s `sounds_.reset()`
runs once per round load, the same cadence the enclosure's own arm-once
gating uses) and replays that SAME id for every subsequent `WallClosed` event
until the next reset. See `libs/game/src/sound_director.cpp`/`.hpp`,
`libs/game/src/audio_engine.cpp`/`.hpp`. No sim/golden-hash impact —
`SoundDirector` reads unhashed events only.

## Final in-game 1:1 gaps — CONFIRMED (2026-07-04, "devam" #39)

Four small fidelity gaps that finish the in-game layer at 100% 1:1.

### 1. Warphole knocks out one random adjacent tile (`sub_4056CA` case 1)

The actor updater's warphole branch runs a ONE-TIME block gated by a per-actor
latch byte `+146` (`if (!*(v23+146)) { *(v23+146)=1; … }`). On first activation,
outside the editor (`sub_40C06A() != 1`), it:

1. Clears the warphole's OWN tile: `sub_425E9B(x, y, 0)` (write cell type 0 =
   Blank via `sub_425E36`, bounds-checked).
2. Picks ONE random adjacent tile and clears it too:
   ```c
   do { do { v21 = rand()%4;
             nx = dword_45BECC[v21] + x;
             ny = dword_45BEDC[v21] + y; }
        while (nx < 0); }
   while (nx >= dword_4648AC /*W=15*/ || ny < 0 || ny >= dword_4648B4 /*H=11*/);
   sub_42C0C8("knocking out %u,%u");   // debug print
   sub_425E9B(nx, ny, 0);              // clear the neighbour
   ```
   `dword_45BECC={0,1,0,-1}` (dx), `dword_45BEDC={-1,0,1,0}` (dy) — the cos/sin
   dir tables. Both are never 0 together, so the centre is NEVER a candidate (no
   explicit skip needed). The nested `do/while` just retries a fresh cardinal
   direction until the neighbour is in-bounds, then sets that tile to Blank
   UNCONDITIONALLY (brick OR solid, whatever sat there). One knockout per
   warphole (the `+146` latch).

Port: `match::apply_actors` (match_factory.hpp) now calls a `knockout_neighbour`
lambda when it places a warphole, driven by the SAME setup-only LCG the `-T,H`
trampoline placement uses (`roll()`), NEVER `State::rng`. It only mutates the
static `cfg.cells` grid (→ Blank), so the per-tick RNG contract is untouched.
GOLDEN: no golden scenario has warpholes (they build `cells` by hand and never
call `apply_actors`) ⇒ UNCHANGED. Tests: `tests/test_match.cpp` (own tile + one
cardinal neighbour cleared, per-seed determinism, edge warphole never writes out
of bounds).

### 2. options.ini `conveyor_speed=` parsing (`sub_406238` reader)

The Conveyor Speed game option is `dword_464930`. The binary hardcodes its
default to **1 (medium)** at init (pseudo.c 14652: `dword_464930 = 1`), and the
install-root `options.ini` overrides it. The reader `sub_406238` (called from
`sub_406086` when the file opens) parses `key=value` lines: split on the first
`=`, `stricmp` the key, `sub_4516C1(value)` (atoi). The `conveyor_speed` key
stores into `dword_464930`, then clamps it: `if (<0) =0; if (getvalue(189) <= it)
= getvalue(189)-1` (pseudo.c 7862-7865). The write side (`sub_405DE3`, format
`"conveyor_speed=%u\n"`, string @0x…1375) confirms the key order = 4th
(levelno, num_to_win_match, enclosement_depth, conveyor_speed).

Downstream: `getvalue(dword_464930 + 190)` is the conveyor's player-move budget
contribution (pseudo.c 23422/23447/23449) and `getvalue(dword_464930 + 295)` its
sprite variant (9129); the options menu cycles the index `[0, getvalue(189)-1]`
(9321-9425). VALUELST **189 = 3** (speed count), **190/191/192 = 250/350/450**
(low/med/high, 1/100 px). THIS INSTALL's options.ini carries **`conveyor_speed=2`
⇒ high = 450**.

Port: new `assets::Options` + `assets::load_options(path)` (install.hpp/cpp) — a
faithful skim of the line parser, surfacing `conveyor_speed` as
`std::optional<int>` (empty when the key/file is absent so the caller keeps the
default). `game_app::init` reads `<game_dir>/options.ini` and
`game_app::start_match` sets `cfg.tuning.conveyor_speed_index` from it (empty ⇒
keeps the confirmed default 1). GOLDEN: config-only, no per-tick RNG ⇒
UNCHANGED. Tests: `tests/test_options.cpp` (present/absent key, missing file,
case-insensitive, comments ignored).

### 3. getvalue(330) idle-fidget spread = 13 (`sub_41F29B` ~23011)

The boxed-in "cornerhead" fidget: when a standing player has `<4` walkable
neighbours (`v99 >= 4` blocked) and its fidget counter `+39` is idle, it rolls
`v111[39] = rand() % v95 + 20; v111[40] = 0;` where `v95 = getvalue(330)` guarded
to be ≥1 (`if (getvalue(330) <= 1) v95 = 1; else v95 = getvalue(330)`). VALUELST
**330 = 13** — the file labels it "how many cornerhead animations there are", so
id 330 is BOTH the number of cornerhead sequences and the fidget-duration
spread; it equals our `kCornerheadVariants = 13` by construction. Port:
`renderer.cpp` `kPanicSpread` 40-stub → **13**. Presentation-only (the roll comes
off `panic_lcg_`, never `State::rng`) ⇒ no GOLDEN impact.

### 4. Goldflame dropped on a head hit — see "Goldflame literalness" RESOLVED note

`sub_421F7E` rolls `rand()%15` UNIFORMLY over all kinds and accepts any whose
per-kind count `player[+86+kind]` exceeds getvalue(50+kind); goldflame (kind 8 =
byte +94, start-with id 58 = 0) is a valid droppable kind when the flag is set.
Added to `PowerupSystem::head_hit`'s `surplus()` (`have = p.goldflame ? 1 : 0`);
`remove()`/`scatter()` already handled Goldflame. The RNG draw count per head hit
changes ONLY for a victim that HAS goldflame — so **golden B must be recaptured**
(its players can pick up hidden goldflame tokens and be head-hit); **A/C/D/E are
byte-identical** (A no players; C no punch/grab ⇒ no flying bombs; D forces no
actions; E hides no powerups ⇒ no player ever has goldflame). Test:
`tests/test_sim.cpp` "a head hit can drop goldflame (kind 8)".

## Options toggles: stomped_bombs_detonate / diseases_destroyable / random_start — CONFIRMED (2026-07-08)

The two Options-screen toggles that had no sim consumer, plus the Random
Start shuffle, pinned end to end. **Ground truth for the key↔global mapping
is the Options screen itself** (`sub_4080DC`): its draw pass pairs each row's
message id with the global it renders (`getstring(<global>+25)`), and its
row switch toggles the same global — no elided strings involved. This
CORRECTS the `docs/re/results-and-options.md` §3 bottom key table for two
rows (see below); the §3 per-row table was right all along.

- msg **251** ("Random Start") renders/toggles **`dword_464AE8`** (draw
  ~9107, switch case 1 @ 9316/9417);
- msg **254** ("Stomped Bombs Detonate") renders/toggles **`dword_464940`**
  (draw ~9140, switch case 4 @ 9326/9428);
- msg **261** ("Diseases Can Be Destroyed") renders/toggles
  **`dword_464990`** (draw ~9216, switch case 11 @ 9351/9454).

So in the §3 22-key list, `random_start=` ↔ `dword_464AE8` and
`stomped_bombs_detonate=` ↔ `dword_464940` (the doc had them swapped: the
options.ini reader's `stricmp` chain — whose literal keys Hex-Rays elided —
does NOT bind in the writer's fprintf order for these two; positional
matching was a wrong assumption there, and the corresponding gameplay reads
below confirm the UI mapping).

**Defaults (all three): VALUELST-seeded at init, THEN options.ini
overrides.** `sub_41095A` @ 0x41095A (pseudo.c 14641-14658) runs
`dword_464990 = getvalue(120); … dword_464940 = getvalue(46);
dword_464AE8 = getvalue(40); …` and only then calls `sub_406A2A` → the
options.ini reader. Shipped VALUELST: **40 = 1** ("default value of 'do we
randomize player starting positions?'"), **46 = 1** ("when a wall segment
closes in on a bomb, does it set the bomb off? 0 - destroy the bomb, 1 -
detonate the bomb (this is a default; otherwise the settings override
it)"), **120 = 1** ("can diseases be blown up like all other powerups?
gbl_diseases_can_be_destroyed"). The shipped options.ini also carries all
three keys as `1`. So every default is ON.

**stomped_bombs_detonate (`dword_464940`) — the closing wall IS the
"stomp".** Its ONLY gameplay read is the enclosure stepper `sub_426818`
(pseudo.c 27257-27300): each dropped border wall probes its tile — any
player is crushed (`sub_421D3F`→`sub_41DE63`), the floor/hidden powerup
record is destroyed unconditionally (`sub_42542D`→`sub_4254F3`, no skull
relocation here), and a GROUNDED bomb (`sub_422E48`, which skips motion
states 2/3 = flying/carried, so airborne bombs sail over) hits the flag:
**ON → `sub_423209(bomb, -1)` queues a proper detonation** (the 100-slot
pending queue `dword_4621F8/FC`, drained as a real explosion that chains);
**OFF → `sub_424841(bomb)` zeroes it in place** — no explosion, no effect.
Port: `Tuning::wall_detonates` (id 46) consumed by
`EnclosureSystem::drop_wall` already matched; the flying-bomb exemption
and the Options-screen override (`GameApp::start_match` sets it from the
merged option, seeded from getvalue(46) exactly like the original's
global) are new. No RNG on either branch.

**diseases_destroyable (`dword_464990`) — OFF means a destroyed skull
RELOCATES, the destruction itself is unconditional.** Gameplay reads:
1. the explosion flame walk (`sub_42331C` tail, epicentre arm @ 25626-25635
   and per-tile arm @ 25653-25661): a VISIBLE floor powerup on the flame
   tile (`sub_42542D`, state +0 == 2 = on-floor; 1 = still hidden under a
   brick) is destroyed (`sub_4254F3`), then `if (kind == 2 /* disease,
   off_45BE50 order */ && !dword_464990) sub_4255B2(2)` — respawn a fresh
   skull at a random free tile;
2. the sliding-bomb cell-entry probe `sub_4230A5` @ 0x4230A5 (pseudo.c
   25155-25179, called from the kicked/conveyor slide @ 25555): probe order
   is grounded bomb → player (both block first), then a visible powerup on
   the probed cell is destroyed AS A SIDE EFFECT (kicked bombs plow through
   powerups) with the SAME skull-relocation compensation, then the
   tile-type verdict (`sub_425FB9` == 0 blank) decides enterability.
`sub_4255B2` is the SAME random-free-tile scatter the head-hit drop uses
(already ported 1:1 as `PowerupSystem::scatter`, RNG pairs `rand()%W`,
`rand()%H`). Port: `Tuning::diseases_destroyable` (id 120, default true),
consumed in `FlameSystem::spread_to` and the new powerup-squash in
`BombSystem::slide`; both call `scatter(Disease)` only when the flag is
off. With the flag ON (default) no draw happens.

**random_start (`dword_464AE8`) — the shuffle is 200 random pair-swaps.**
Round init (`sub_421793` @ 0x421793, pseudo.c 23996-24012; repeated
verbatim in the demo-replay stepper `sub_40133F` @ 4472-4488): when the
flag is set, `for i in 0..199 { a = rand()%10; b = rand()%10; if (a != b)
swap(startX[a], startX[b]), swap(startY[a], startY[b]) }` over the 10-slot
spawn-coordinate arrays `dword_46460C/dword_46465C`. Port:
`match::build_match_config` now mirrors this loop (was a clean-room
Fisher-Yates, flagged as unpinned) on a SETUP-ONLY LCG — the original's
rand() is wall-clock seeded, so the deterministic seed substitution is the
same policy the brick fill uses. The absent-key default getvalue(40) = 1
(ON) is now honoured by `GameApp::init`.

**GOLDEN: byte-identical.** wall_detonates=1 was already the sim default
and the enclosure consumer predates this entry (no default-path change);
diseases_destroyable=true (the original default) makes both new relocation
calls dead on the default path, and the new slide-probe powerup squash
only fires when a moving bomb's next cell holds a floor powerup — no
golden scenario produces that (proved by running the suite before/after:
all 32 tests, including `golden`, pass unchanged). The flying-bomb
exemption in drop_wall likewise touched no scenario. Three fidelity gaps
were deliberately NOT taken here (each changes default-path behaviour and
needs its own golden treatment) and are now RESOLVED in dedicated entries
below: **"Flame-arm stops"** (the arm STOPS at the powerup it burns and at
the bomb it chain-detonates instead of burning through), **"Flying-bomb
landing on powerups"** (a flying bomb treats a floor powerup as an occupied
landing tile instead of landing on it), and **"Scatter occupancy test"**
(the head-hit/dud scatter's occupancy predicate: player blocks placement,
flame does not — this entry's own "flame/powerup/bomb tiles burn an
attempt" phrasing above was imprecise and is corrected there).

(Provenance: `sub_4080DC` draw/switch pseudo.c 9097-9454; `sub_41095A`
14641-14658; `sub_426818` 27166-27310; `sub_422E48` 25031-25052;
`sub_423209` 25195-25204; `sub_424841` 25899-25904; `sub_42331C` flame walk
25586-25682; `sub_4230A5` 25155-25179; `sub_4255B2` 26443-26483;
`sub_425383` 26353-26375; `sub_421793` 23978-24013; `sub_40133F`
4443-4499; VALUELST.RES lines "40,1" / "46,1" / "120,1" with the quoted
authored comments; shipped options.ini `random_start=1`,
`stomped_bombs_detonate=1`, `diseases_destroyable=1`.)

## Flame-arm stops — CONFIRMED (`sub_42331C` per-direction arm loop, pseudo.c 25637-25678)

Read 2026-07-08, resolving the "Options toggles" flagged gap. The exploding
bomb's own tile (the epicentre, pseudo.c 25619-25636) is unconditionally
ignited, then any powerup on it destroyed — no stop/occupancy test applies
there (placement rules mean no second bomb can ever share that tile). The
EXTENDING ARM is a different, gated loop: for each of the 4 directions, for
`m` in `0 .. bomb.flame-1`, the arm advances one tile and runs THREE checks
**in this order, each BEFORE the ignite call**:

1. **A grounded bomb here** (`sub_422E48` @ 25641; excludes motion states 2
   flying / 3 carried, same as everywhere else) → queue its detonation
   (`sub_423209(bomb, dir_byte)`), **`break`** — the arm stops. The tile is
   **never ignited by this arm at all** (no `sub_426FCC` call on that
   branch); the chained bomb's OWN explosion (same tick) flames it via ITS
   epicentre instead.
2. **A visible floor powerup here** (`sub_42542D` @ 25653, state == 2 — a
   hidden/state-1 record under a still-standing brick can't occur on a
   reachable arm tile, since a brick tile fails the cell-type check below
   before this point could even be reached along that arm in a later step)
   → destroy it (`sub_4254F3`), relocate a fresh skull when
   `diseases_destroyable` is off (`sub_4255B2(2)`, same as the epicentre and
   the sliding-bomb squash), **`break`**. Also never ignited.
3. Only past both: the cell-type verdict (`sub_425FB9`). **1 (solid)** →
   `break`, never ignited. **2 (brick)** → ignite as "brick burning"
   (orientation 9), increment the brick-destroyed counter, run the
   brick-destroy/reveal path (`sub_425EFC`, `sub_425107`), `break`. **0
   (blank)** → ignite normally (`sub_426FCC`, orientation `k`/`k+4` by
   whether this is the arm's last tile) and the arm CONTINUES to the next
   tile.

**The bug this corrects:** our previous `FlameSystem::spread_to` ignited
every non-solid/non-brick tile unconditionally (destroying any powerup or
chain-detonating any bomb found there) and then continued the arm past it —
so a flame arm burned straight through bombs and powerups instead of
stopping at them, extending its visible reach one tile further than the
original on that ray. Fixed: `FlameSystem::ignite_epicentre` (new, the
epicentre-only always-ignite path) is now distinct from `FlameSystem::
spread_to` (the arm), which checks bomb-then-powerup-then-cell-type BEFORE
igniting and returns `false` (stop, no ignite) on the first hit — mirroring
the original's per-tile order and its "no `sub_426FCC` call on that branch"
detail. Tests: `tests/test_sim.cpp` ("flame arm stops at a floor powerup,
without igniting its tile", "flame arm stops at a bomb it chain-detonates,
without igniting past it").

**GOLDEN IMPACT: scenario D only.** Proved by running the full suite before
and after: golden A/B/C/E are byte-identical (no scenario in them ever has a
flame arm reach a bomb or powerup tile before this fix). Golden D (floor
seeded with Disease/SuperDisease/Skate/Flame tokens) diverges starting
between tick 400 and 600 — its ticks-200/400 checkpoint hashes are
UNCHANGED (the arm hasn't reached a powerup yet at that point), and its
pinned RNG stream (`kExpectedRng`) is completely unaffected at every
checkpoint (the fix adds no RNG draws — it only changes which tile the
blank-tile ignite loop reaches next, and whether/when the existing
`scatter()` skull-relocation call fires, which was already in the RNG
stream). Recaptured: `tests/test_golden.cpp` "golden D" ticks 600/800.

(Provenance: `sub_42331C` epicentre block pseudo.c 25601-25636, arm loop
25637-25682; `sub_422E48` 25031-25052; `sub_42542D` 26380-26389; `sub_4254F3`
26406-26417; `sub_425FB9` 26863-26871; `sub_426FCC` 27478-27504 (writes
`dword_46224C`, the same flame array `sub_42708D` reads).)

## Flying-bomb landing on powerups — CONFIRMED (`sub_42331C` flight-landing check, pseudo.c ~25443-25469)

Read 2026-07-08, resolving the "Options toggles" flagged gap. A thrown or
punched bomb (motion state 2, `sub_42331C case 2`) resolves each landing
boundary with `if (!sub_425FB9(x,y) && !sub_422E48(x,y) && !sub_42542D(x,y))`
— the tile must be blank (not solid/brick), have no grounded bomb, AND have
**no powerup record at all** (`sub_42542D` returns the record regardless of
state — hidden or visible; both count). Only when ALL three hold does the
original even test for a player (`sub_421CB5`) at that tile — the player
(head-hit) check is NESTED INSIDE the "clear" branch, not evaluated
independently. If the tile fails the three-way test, the bomb just falls
through to the un-settled tail (`continue`), i.e. it hops onward exactly as
it does off a wall or another bomb — the powerup is left completely
untouched (no destruction, no pickup, no compensation draw).

This differs from the SIBLING mechanic, the sliding/kicked-bomb cell-entry
probe `sub_4230A5` (see "Kick nuances"): a sliding bomb PLOWS THROUGH a
visible powerup, destroying it as a side effect (with the same
diseases_destroyable skull-relocation compensation) and continuing to enter
the tile. A flying bomb does neither — it cannot land there and does not
touch the powerup. The two mechanics are deliberately inconsistent with
each other because the binary itself is: `sub_42331C`'s flight branch and
slide branch call different helpers (`sub_42542D` raw vs. `sub_4230A5`'s
destroy-then-continue) with different outcomes, and the port must match the
binary, not internal consistency.

**The bug this corrects:** `BombSystem::fly`'s landing-occupancy check
(`grid::tile_open` + `grid::bomb_at`) never consulted `State::floor`, so a
flying bomb would land on (and coexist with) a powerup tile instead of
hopping past it. The player check also ran unconditionally instead of being
nested inside the "clear" verdict — unobservable in practice (a player can't
normally coexist with an unclaimed powerup on the same tile, since walking
onto one picks it up the same tick) but not the literal control flow.

Fixed: `BombSystem::fly` now computes `clear = tile_open && !bomb_at &&
floor[ty][tx] == None` first, and only checks for a player (head-hit) inside
`clear`; the hop-vs-settle branch checks `!clear || victim >= 0`. Tests:
`tests/test_punch_throw.cpp` "a punched bomb hops over a floor powerup
instead of landing on it" (asserts the powerup survives the whole flight
untouched and the bomb settles elsewhere).

**GOLDEN IMPACT: none.** Proved by running the full suite before and after:
all golden scenarios A-E are byte-identical. No golden scenario has a flying
bomb's landing tile carrying a floor powerup, so the new `floor[ty][tx] ==
None` branch of `clear` never evaluates false on any existing path — the RNG
stream and every hashed field are untouched.

(Provenance: `sub_42331C` flight-landing check pseudo.c 25443-25469;
`sub_422E48` 25031-25052; `sub_42542D` 26380-26389; `sub_421CB5`
24196-24212; `sub_4230A5` 25155-25179 (the sliding-bomb sibling, contrast).)

## Scatter occupancy test — CONFIRMED (`sub_4255B2` re-roll predicate, pseudo.c 26458-26479)

Read 2026-07-08, resolving the "Options toggles" flagged gap and correcting
this file's own earlier "flame/powerup/bomb tiles burn an attempt" phrasing
(under "Head hit" and "Options toggles" above). The head-hit/dud scatter
(`sub_4255B2`, used 1:1 by `PowerupSystem::scatter`) draws `x = rand()%W, y
= rand()%H` per roll (inner budget 100 rolls/outer attempt, 100 outer
attempts, token lost if both are exhausted). Per roll:

1. `sub_425FB9(x,y)` must be **0 (blank)** — solid (1) or brick (2) tiles are
   silently re-rolled, burning NEITHER an inner-guard-relevant attempt nor an
   outer attempt (the `&&` short-circuits before the outer-budget branch).
2. Past that, the tile is REJECTED — burning ONE outer attempt — when
   **`sub_422E48(x,y)` (a grounded bomb) OR `sub_42542D(x,y)` (ANY powerup
   record, hidden or visible — same raw check as the flying-bomb landing
   test above) OR `sub_421CB5(x,y)` (a live player)** is true. Passing all
   three places the token there immediately.

**Flame is NEVER checked.** `sub_42708D`'s flame array (`dword_46224C`) is
entirely separate storage from the cell-type grid (`dword_46222C`,
`sub_425FB9`) and the powerup array (`dword_462214`, `sub_42542D`) — the
scatter predicate has no call to `sub_42708D` anywhere. A scattered token
CAN land on a tile that is actively burning.

**The bug this corrects:** `PowerupSystem::scatter` rejected a candidate
tile when `s.flame[y][x] > 0` (checking something the original never
checks) and never checked for a live player on the tile (something the
original DOES check) — an exact swap of what should and shouldn't block.
The missing player check is easy to miss end-to-end: when the scatter lands
a token on a tile a player is standing on, powerup pickup runs later in the
same tick and the player immediately reclaims it, so the token never
visibly rests on the floor either way — inspecting `State::floor` after a
full tick cannot distinguish "rejected at scatter" from "placed then
instantly picked up". The distinguishing signal is whether a
`Event::Type::PowerupPicked` fires at all.

Fixed: `PowerupSystem::scatter`'s reject condition is now `grid::bomb_at ||
floor != None || grid::player_at` (flame dropped, player added). Added
`grid::player_at` (`libs/sim/src/grid.hpp`) mirroring `sub_421CB5`. A hidden
powerup (state 1, under a standing brick) needs no separate check: `cells !=
Blank` already excludes Brick tiles before the occupancy branch runs, so
`floor != None` alone is equivalent to "any record" on a reachable (blank)
candidate. Tests: `tests/test_sim.cpp` "scattered token CAN land on a
burning (flamed) tile" and "scattered token NEVER lands on a tile a live
player occupies" (the latter asserts no `PowerupPicked` event fires, per the
masking behaviour above — checking `floor` alone would not have caught the
missing player check).

**GOLDEN IMPACT: none.** Proved by running the full suite before and after:
all golden scenarios A-E are byte-identical. No golden scenario's scatter
draws ever land on a currently-flaming tile (so removing that reject never
flips a reject to an accept) or on a player-occupied tile before this fix
(so adding that reject never flips an accept to a reject) — every draw's
accept/reject verdict is unchanged, so the RNG stream and hashed state are
untouched.

(Provenance: `sub_4255B2` pseudo.c 26443-26483; `sub_425FB9` 26863-26871;
`sub_422E48` 25031-25052; `sub_42542D` 26380-26389; `sub_421CB5`
24196-24212; `sub_42708D` 27511-27519 (separate array, never called from
`sub_4255B2`).)

## `LEVELS.DAT` — NOT READ by the shipped game (2026-07-09)

Read for coverage-audit.md §3 / "Top open items" #1: is `LEVELS.DAT` a
level-unlock table or campaign-progress file the port is missing?

**Finding: `BM95.EXE` never opens this file.** Exhaustive checks against the
full `pseudo.c` decompile:

- `grep -in "levels" pseudo.c` and `grep -in "\.dat" pseudo.c` — zero matches.
  No string literal `"LEVELS.DAT"` (or any casing/substring of "level")
  exists anywhere in the decompiled text.
- Every wildcard file-scan literal in the binary is enumerated at
  `aCam = "*.cam"` (pseudo.c:1311), `aSch_0 = "*.SCH"` (pseudo.c:1404), and
  `aSnd_1 = "*.snd"` (pseudo.c:1576) — no `"*.dat"` or `"*.*"` scan exists
  that could pick the file up incidentally.
- `strings -a BM95.EXE` (raw PE, not just the decompile) also has zero
  occurrences of "level" in any case, narrow or wide-char — ruling out a
  string built at runtime that Hex-Rays failed to surface as a literal.
- The same check against every other shipped executable in the install tree
  (`MAKECFG.EXE`, `TOOLS/EXTPSS.EXE`, `TOOLS/FREDIT.EXE`,
  `TOOLS/FREDSPIT.EXE`, `TOOLS/NUMBER.EXE`, `TOOLS/PLAYSH.EXE`,
  `TOOLS/PSS.EXE`) — including `FREDIT.EXE`, the stage/scheme editor, the
  most plausible consumer of a "levels" file — also finds zero "level"
  strings. No shipped binary reads or writes it.

**What the file actually is:** 4 bytes, contents `81 FB BF 33`. Too small to
be a level table (no per-level struct, no count prefix that resolves to a
sane array). `uninst.log` lists it only as `Copied
C:\INTRPLAY\BOMBRMAN\LEVELS.DAT` — i.e. it was part of the original install
payload, not something the game wrote at runtime (its file timestamp matches
the other installed binaries, not `bmstats.dat`'s later, clearly
runtime-rewritten timestamp). Best explanation: a leftover installer/build
artifact (version stamp, checksum, or an internal Interplay tool's marker
not present in this retail tree) that ships with the product but has no
consumer in it.

**Conclusion: not load-bearing for any feature.** It does not gate level
unlocks, campaign progress, or anything else — `.CAM` campaign progression
(`docs/re/campaign.md`) and stage rotation (`.SCH` via `MatchConfig`) are
fully accounted for by other, confirmed-read files. No parser was added;
writing one would invent structure for 4 bytes nothing in the shipped game
interprets, which is exactly the kind of unfounded content CLAUDE.md's
faithful-port rule forbids. Closing this as **dead/tooling data**, not a
gap in the port.

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
