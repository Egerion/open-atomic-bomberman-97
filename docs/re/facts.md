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
| Calling convention | Watcom register: args in **EAX, EDX, EBX, ECX**; callee saves ebx/ecx/edx/esi/edi/ebp | prologue of getstring @ 0x4124a4 reads arg from EAX |
| Graphics | **DirectDraw** (`DDRAW.dll`) | import table |
| Input | **DirectInput** (`DINPUT.dll`) | import table |
| Audio | **DirectSound** (`DSOUND.dll`) + `WINMM.dll` | import table |
| Netplay | `WSOCK32.dll` | import table |

Implication: the renderer is a DirectDraw blit loop over a 640×480 palettized
surface, matching the PCX/ANI asset formats we already decode.

## VALUELST lookup mechanism — CONFIRMED

CORRECTION 2026-07-09 (docs/formats/valuelst.md consistency pass): the
routine examined below at **0x4124a4** is `getstring(id)` (the MESSAGES.TXT
string lookup), NOT `getvalue`. `getvalue(id)` — the numeric VALUELST lookup
every other doc cites — is **`sub_412135` @ 0x412135**. Evidence:
`sub_412135(1007)`'s return is used directly in arithmetic (pseudo.c
~5907-5912), while `sub_4124A4(730)`/`(i+731)`'s return is passed as a
string pointer to the text drawer `sub_41696C` (pseudo.c ~5780/5791). Both
are id-indexed table lookups of the same shape, which is how the early cold
scan mislabelled this one. The mechanism below was observed on 0x4124a4 and
so describes the string table; `sub_412135`'s call shape matches the same
id-indexed model for the value table:

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

Observed literal-id call sites of **0x4124a4 = getstring** (28 of them)
request ids 95, 97, 700–768, 900/905, 1200–1250 — MESSAGES.TXT string ids.
(The old inference here — "gameplay VALUELST values are batch-loaded, not
fetched through getvalue" — rested on mislabelling this scan as `getvalue`;
the real `getvalue` = `sub_412135` IS called with gameplay ids throughout
the decompile, e.g. `sub_412135(915)`/`(680)` in mechanics already ported.
Provenance: exhaustive scan of `call 0x4124a4` sites + the correction note
above.)

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
| `0x4124a4` | (found cold earlier) | getstring(id) — mislabelled getvalue until 2026-07-09, see the VALUELST-lookup correction |

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
| +0x3C (+60) | byte | **CONFIRMED draw-colour index** (0-9, selects the `.RMP`/`dword_460564[10]` set): non-team play = the player's own slot index; Team Play = `sub_4214BC`'s round-init override, `0` (white) or `2` (red) per the team byte below — every colour-keyed draw (body blit, bomb spawn, and by inheritance flame/carried-bomb/death-anim) reads THIS byte, `docs/re/player-colour.md` "Team Play colour override" |
| +0x54 | byte | **CONFIRMED team byte** (== decimal +84, same field `docs/re/setup-screens.md` cites as "+84", `sub_4223E7`/`sub_422437` accessors; read by `sub_4214BC`'s round-init colour override, `docs/re/player-colour.md` "Team Play colour override") |
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
**the shadow** blits at the player's OWN anchor — the X at player offset +28
and the Y at +32 — with no
offset (its (14,16) hotspot centres it). Our renderer had a stray `+8` on the
shadow (removed 2026-07-04) and draws powerups top-left (≈1 px off, animation
aside). (Resolved while reviewing Ege's "shadow/powerups/bombs a bit too high".)

## Player colour remap — CONFIRMED (`sub_414A65`, builds the `.rmp` tables)

The green armour of every pre-rendered player sprite (walk/stand/bombs/flames/
deaths, all authored in "green") is retargeted per player at load time by
**`sub_414A65`** (0x414A65), which bakes a 256-entry remap table (`%u.rmp`,
`dword_460564[player]`). Init loops all ten players building the args from
VALUELST: `getvalue(200+5k)=R%`, `getvalue(201+5k)=G%`, `getvalue(202+5k)=B%`,
then `sub_414A65(k, R%, B%, G%, 0)` — note the argument ORDER: 2nd = R%,
3rd = B%, 4th = G% (blue before green). Per source
palette entry `[R,G,B]`:

```
if (G > R && G > B) {                 // green-dominant (strict, no margin)
    lum      = G;
    baseline = (R + B) / 2;
    excess   = lum - baseline;
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
| 4 | +136 | **short flame** | dropped-bomb flame forced to 1 (`sub_41EB13` forces its flame-length argument to 1) | 134 |
| 5 | +137 | **super/ebola** | fast **and** auto-drop (grouped with +133 and +135) | 135 |
| 6 | +138 | **short fuse** | dropped-bomb fuse `÷3` (`sub_41EB13` divides its fuse-length argument by 3) | 136 |
| 7 | — | **swap** | swaps (x,y) with a random other live player; no flag | 137 |
| 8 | +140 | **reversed** | godir `(g+2)&3`, humans only (`+16 != 1`, at 23049) | 138 |

Move-budget order (23436-23438), applied in exactly this sequence to one
running value: start from the base budget; if slow, divide it by 3; then if
fast or super, replace it with 3× the current value divided by 2.
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
is a DIFFERENT kind of counter: a RAW per-FRAME decrement (`--` once every
displayed frame, `sub_41F29B` ~22927 / batch_0x41F29B.cpp:279), NOT a delta/tick
countdown — the same cadence as the head-stun `+58`. It gates re-spreading, so
the port burns it kSubFrames/tick (CORRECTED 2026-07-22 — the earlier "counts
down by 1/tick" note here was stale/pre-ADR-0006 and made contagion re-spread
~kSubFrames× too slow; see `diseases.cpp` `spread_and_age`).

**Visual — CONFIRMED, re-traced exactly 2026-07-09 (`sub_41F29B` ~23252):**
after the shadow blit, the body sprite's FRAME argument (normally the
player's own draw-colour byte, `+0x3C`/+60 — one frame per player colour,
0-9, within whatever pose sequence the animation state machine already
picked) is replaced by `rand() % 10` whenever bit 3 of the player's disease
counter is set. That counter is a **WORD** at struct byte offset **+120**
(the routine indexes the player through a 16-bit-wide pointer, so its
element 60 is byte offset 120 — NOT the same access width as the `+0x3C`
byte field), the SAME offset this section's "Timer" paragraph already names as
the disease-age counter that "counts up by the frame delta each tick" — so
this is bit 3 of the elapsed-disease-duration counter, not an independent
flag: `if (diseaseAge & 8) draw(x, y, rand() % 10, sprite)`. Net effect: the
body is redrawn each tick in a **random one of the ten real player-colour
sprite sets** (not an arbitrary tint), and — because it keys off a counter
bit rather than a plain boolean — it flashes in blocks as the counter's bit
3 toggles, rather than being constantly randomized for the whole disease
duration. (This is why, with no such indicator, only the speed diseases were
noticeable in-game.) **Ported (tightened 2026-07-09, `libs/game/src/
renderer.cpp` `draw_world`):** previously an `SDL_SetTextureColorMod`
arbitrary-RGB tint; now `Renderer::disease_flash_colour()` (a presentation
LCG, never `State::rng`) picks a real `0..9` colour index that substitutes
for the player's own draw-colour in every pose branch (walk/stand/
cornerhead/kick/punch/carry/spin all key off the same `body_colour`), so the
flash genuinely shows the player briefly wearing one of the ten shipped
player recolors, matching the original's actual mechanism instead of
approximating it with a tint. **Cadence corrected 2026-07-10** (bomb-placement
investigation): the gate now reproduces the counter-bit pulse exactly —
`(disease_timer & 8) != 0`, mirroring the confirmed bit-3 (value 8) test on the
player's disease counter at +120 (word index 60) — replacing the prior
alternating-tick simplification (`s.tick & 1`). Our `disease_timer` counts down
where `+120` counts up, but `& 8` yields the identical 8-tick-on / 8-tick-off
pulse (~0.4 s buzz, 0.4 s calm at 20 Hz); only the phase differs (imperceptible
in a strobe). The prior `s.tick & 1` produced a uniform ~10 Hz shimmer easily
dismissed as a render artifact — the clustered pulse reads far better as a
distinct "I am diseased" state. This matters because the strobe is the SOLE
ongoing cue for the no-bomb **Constipation** disease (`+134`, the drop gate
inside `sub_41F29B`'s bomb-action block), whose faithful placement block is
exactly the "sometimes I
can't place bombs, for no reason" report — the sim gate is correct; the cue was
the weak link. Presentation-only (reads hashed `disease_timer`, never
`State::rng`); golden unaffected. NOTE: `renderer.cpp` is not built by the
`headless` preset, so verify this one-line change with an SDL build.

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
  (its kind argument is 0 — trigger and jelly never fizzle) and not a network
  game. Gated by a
  global timer (`dword_464AF4`): when open, the gate re-arms FIRST
  (`sub_422C13`: gate += getvalue(320) + rand() % getvalue(321)) and then the
  bomb duds on `rand() % max(1, getvalue(322)) == 0` (322 = 3). The gate is
  also armed once at match init (`sub_422C7A`).
- **UNITS CORRECTED 2026-07-10 (core-feel audit):** VALUELST 320/321 are
  **SECONDS**, per the file's own legend — *"minimum number of seconds
  between potential dud bombs"* (320 = 180) / *"additional random number of
  seconds"* (321 = 180). One dud opportunity per **3–6 MINUTES**, not the
  ~9–18 s this entry previously claimed by reading the raw values as ticks —
  that misreading made our duds ~20× too frequent (a major feel deviation:
  roughly every third bomb after each 9–18 s window fizzled). The gate
  compares against the program's ms clock (`time_()`); the exact
  seconds→clock scaling inside `sub_422C13` is register-garbled in the
  decompile, but the legend's intent is unambiguous. Note the re-arm is
  literally `+=` on the PREVIOUS deadline (not anchored on "now"), so a
  long-idle gate can bank consecutive openings — ported literally.
- **Fizzle window** (`sub_42331C` tail): the "bomb regular green dud"
  sequence (DUDS.ANI) renders while the always-running anim counter stays
  within getvalue(323) = 120 ticks (6 s); past it the bomb returns to state 1
  with its anim reset. The fuse gate skips state 2 entirely, so the fuse
  RESUMES where it froze — total lifetime = fuse + fizzle.
- The original's gate compares wall-clock-ish time (why network games skip
  duds); our deterministic port measures the same values in ticks
  (`State::dud_gate`, hashed, seconds × `kTicksPerSecond`) — semantics
  identical at 20 Hz, and determinism holds where the original had to
  disable the feature.
- Chain explosions still set off a fizzling dud (the explosion path ignores
  the dud state).

Ported: `Bomb::dud_left` (hashed) + `State::dud_gate` (hashed), roll in
`BombSystem::place`, freeze in `tick_fuses`, DUDS.ANI wired through
AssetStore/SequenceSet/Renderer. Tests: `tests/test_dud.cpp`. Golden fully
recaptured (hash layout gained two fields; setup consumes one arm draw), and
again 2026-07-10 for the seconds correction.

## Head hit — CONFIRMED (`sub_421F7E`, stun countdown in the player updater)

Read 2026-07-03. When a flying bomb lands on a live player:

- **Stun = hardcoded 16 ticks** (the literal 16 is stored straight into the
  player's word at +58, i.e. word index 29 of the struct): the player
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

**NOT conflated with DEATH (verified 2026-07-11).** `sub_421F7E` is ONLY the
bomb-on-head handler — it stuns and drops a rand-LIMITED count of rand%15-rolled
kinds. It is a distinct code path from a player's DEATH, which drops the
player's WHOLE surplus via a different function (`sub_41DBFE`) at the end of the
death animation — see "Death powerup scatter" below. The head-hit facts above
describe `sub_421F7E`'s own machinery and do not touch death; the two only share
the tile-placement primitive `sub_4255B2`.

### Stun does NOT gate flame-death or pickup — RESOLVED (offset+8 ≠ stun)

Read 2026-07-10, resolving the disease audit's "adjacent-but-out-of-scope"
note (below, under "Disease system fidelity audit"): does a stunned player
become immune to flames and unable to pick up floor powerups for the 16-tick
head-hit duration? **No — offset+8 (the DWORD the disease audit and `ai.cpp`
both labelled "Player::stun") is a DIFFERENT field: the player's "already
died this round" flag, not the stun countdown.**

Evidence, cross-checked three ways:

1. **`sub_421F7E`** (the head-hit handler) is declared as taking its single
   argument in EAX as an explicit pointer to 16-bit elements. It writes
   **16** into byte offset **+58** (element 29, word-strided: 29×2), **3**
   into the state word **+78** (element 39), and **0** into **+80**
   (element 40). It never touches a DWORD at +8.
2. **`sub_41DCB2`** (the death-application routine, called from the flame-kill
   path `sub_41DE63`) guards on TWO conditions ANDed together — the WORD at
   **+102** must be 0 and the DWORD at **+8** must be 0, i.e. "not currently
   spawn-invulnerable (+102) AND not
   already dead (+8)" — then unconditionally writes **1** into the DWORD at
   **+8** (pseudo.c ~21921/21956). +8 is written exactly once in the whole
   binary,
   as a hardcoded boolean `1`, never a countdown value, and is cleared only
   by the round-entry reset (+8 ← 0 at ~22874) which is
   gated on offset **+0** being 0 — a flag death never resets, so +8
   stays 1 for the rest of the round once set (Bomberman rounds have no
   mid-round respawn). A duration flag with a single hardcoded `1` and no
   in-round reset path cannot be a 16-tick stun counter.
3. **`sub_41F29B`'s own body is internally inconsistent with "+8 = stun"**:
   the giant block gated on "+8 is 0" (~22904, closes ~23456 — brace-
   depth-traced, not eyeballed) CONTAINS the real stun countdown's decrement
   (while the word at **+58** is > 0 the block runs and decrements **+58**,
   word-strided offset,
   ~22982-22990). A field cannot gate a block that only decrements *itself*
   inside that same block — that is circular. +8 and +58 are necessarily two
   different fields.

What stun (+58) actually gates, read end to end: **only new-input
acquisition.** A local can-act flag is initialised
to 1 at ~22981 and forced to 0 while +58 > 0 or while the player is in states
4/5/6/7. That flag, combined with `dword_4621E0` being zero, is the sole gate
on the one input-acquisition site at ~23028-23039 (the `sub_41E61E` human
decode / the AI decide path) — i.e., a stunned player cannot change direction
or start a new bomb action — plus one cosmetic standing-animation frame pick
at ~23086. Movement-budget accrual and
the `sub_41EC84` per-pixel-step call (~23422/23423 and ~23451/23452) are
**inside** the +8 block but are **not** gated on that flag or on +58 at all, so a
still-alive stunned player's movement machinery keeps executing every tick
of the stun (only issuing a *new* direction is blocked) — surprising, but
consistent with "the player got bonked and can't react" rather than "the
player is frozen solid." This movement-continues-during-stun behaviour is a
separate, adjacent finding from the immunity question this entry resolves.

**RESOLVED 2026-07-10 (follow-up commit): stunned-but-alive movement ported.**
The follow-up trace pinned the exact per-tick behaviour, refining the
"pre-existing momentum keeps executing" phrasing above — there is no momentum
retention to coast on, the un-gated machinery matters only when a STAGE ACTOR
drives it:

- The +58 decrement (~22982-22990) runs unconditionally every alive tick
  (also clears the head-hit action-state 3 when it hits 0); it is NOT inside
  the movement branch.
- The new-direction word +46 in the player struct is reset to `-1` every tick
  at ~22980, BEFORE the can-act gate — so a keyed direction lives exactly one tick
  and is never retained across ticks, stunned or not. While stunned,
  sub_41E61E/AI (the only writers of a keyed +46) are skipped, so +46 stays
  -1 into the movement dispatch.
- With +46 == -1 the player takes the IDLE movement branch (~23413): if a
  CONVEYOR is underfoot (~23417-23423) it forces +46 to the belt direction,
  adds the belt budget getvalue(190+idx), and calls `sub_41EC84` — so a
  stunned player IS still carried by a belt, still fires the in-loop kick
  probe, and still triggers warphole/trampoline step-ons (the stepper's
  own check for a still-unset (-1) direction), all exactly as a keyless idle
  player. If no actor is
  underfoot the branch jumps straight to the anim/draw block (23081) without
  touching the mover.
- The keyed branch (~23430-23453, the one that accrues the player's OWN
  speed/disease budget) is only reachable with +46 != -1, i.e. never while
  stunned. And `sub_41EC84`'s budget loop (22568) drains its budget to <= 0
  within the same tick that granted it (its body is additionally gated on
  +46 != -1 at 22572), so there is never positive leftover budget for a
  stun to "coast" on: an off-belt stunned player stands still, full stop.

Port (`player_turn`, simulation.cpp): the old full early-return on
`p.stun > 0` is replaced by decrement-and-fall-through — the input decode is
skipped (want_godir forced -1, mirroring the skipped sub_41E61E) and the
bomb-action block is skipped (mirroring the +56/+57 key bytes staying at
their per-tick 0 reset), but `move_on_actor`/kick-probe/step-on triggers all
still run, so a belt keeps carrying a stunned player into whatever it leads
to. **RESOLVED 2026-07-11 (follow-up commit):** the original still reaches
the bomb-action block (23277-23380) while stunned, so disease AUTO-drop (the
+135/+137 forced edge) and
the release-throw of a carried bomb keep firing during a stun there; this was
deferred (our port used to skip both while stunned) and is now ported — see
"The bomb-action block runs in every alive state (standing-stun
restructure)" below. The
grab's own pickup_pause window is a SEPARATE, still-deferred case (the
original forces the key HELD there, not zero — a different rule the
restructure below deliberately does not touch; see that entry).
GOLDEN: proven inert — no golden board has any stage actor, so the newly
executing path moves nothing; the action-block skip is behaviourally
identical to the old early-return (same prev_action1/2 updates); no RNG
draw added/removed/reordered. Full suite before/after: every constant in
`tests/test_golden.cpp` (all hashes, kExpectedRng at all four D checkpoints,
E's bounce count 10 and final rng) passes UNCHANGED — zero recapture.
Pinned by `tests/test_conveyor.cpp` "a head-stunned player on a conveyor is
still carried by the belt" / "a stunned player takes no new input and does
not coast; input resumes after". One adjacent ordering fix rides along: a
stunned player mid-bounce/mid-warp now ticks BOTH countdowns (the original's
+58 decrement is unconditional and the state-5/6/7 anim counters advance in
the same tick — the old early-return froze bounce/warp while stunned; no
scenario combines them today, golden unaffected).

The flame-death check (`sub_42708D`/`sub_41DE63`) and the floor-powerup
dispatch (`sub_42542D`/`sub_41E21E`, ~22915-22926, and again unconditionally
inside `sub_41EC84`'s loop at ~22710-22717) are gated ONLY on +8 (dead), which
a merely-stunned-but-alive player never sets. **Conclusion: stun is not flame
immunity and does not block pickup, in the original.** `field_vs_players`
(simulation.cpp) already matches — it gates on `!p.alive` only and has never
checked `p.stun` — so **no production code changed**. Pinned by
`tests/test_sim.cpp` "a stunned-but-alive player still burns and still picks
up floor powerups" (added 2026-07-10; confirmed failing if a `stun == 0`
guard is (re-)added to `field_vs_players`, so it is real regression coverage,
not a tautology).

**Fallout CORRECTED 2026-07-10 (same day, follow-up commit):** the concern
raised here was confirmed and fixed. `DiseaseSystem`'s `stun == 0` /
`stun > 0` gates (added by the disease audit below, "point 3", citing this
same +8 field as "Player::stun") DID rest on this mislabelling — every one
mirrors an original gate that reads `!player[2]` (offset +8 = **dead**), not
+58. They have been **removed**: a merely-stunned-but-alive player now ages,
spreads/catches disease, and is a valid Swap target, matching the binary
(`sub_41DFB6` 22073 tests the actor's +8 dead flag for zero; `sub_41F29B`'s
age/contagion block at 22904 is gated on that same +8 being zero, with the +58
stun decremented *inside* that block at 22982). `ai.cpp`'s
`pick_live_enemy`/`behave_bomb_enemy`/`behave_seek_enemy` target-liveness
checks (`q.stun` reads mirroring the identical +8 dead-flag tests at
`sub_421CB5` 24207 and `sub_422718` 24741) were the same mislabel and are likewise switched to `!alive`
(dead) only. GOLDEN: inert in scenario D (no head-hits there → no stun ever),
so all golden constants stayed byte-identical, kExpectedRng included — no RNG
draw added or removed. The AI-dispatch stun gate (`simulation.cpp`, §7 of
ai.md) is UNTOUCHED — that one correctly models +58 (stun blocks *new input
acquisition*). Tests: `test_disease.cpp` (stunned-but-alive ages/spreads/swap-
target), `test_ai.cpp` ("bombs a stunned-but-alive enemy"). The remaining
item — `player_turn`'s full early-return on `stun > 0` — is now RESOLVED in
its own pass: see the "RESOLVED 2026-07-10: stunned-but-alive movement
ported" box above (stun only skips the input decode and the bomb-action
block; the stage-actor mover still runs, golden proven inert).

## Player state machine (+78) — COMPLETE (2026-07-11 full-enumeration audit)

The original models each player's action/movement mode with ONE state word at
player byte offset **+78** (in `sub_41F29B`, which types the player record as
a pointer to 16-bit elements, that is element 39). The frame counter of the
current state is the word at **+80** and its ms accumulator the word at
**+82**, advanced in every animated state by the engine's shared accumulator
idiom: add the frame delta to **+82**, then while **+82** is still positive,
increment **+80** by 1 and subtract the per-tick quantum from **+82**.
Because there is only ONE word,
illegal state COMBINATIONS are structurally unrepresentable in the original —
the motivation for this audit of our multi-flag port. Enumeration method:
every access to byte offset +78 in the full decompile (a textual scan for
`+ 78)`:
sub_41DE63 21999/22003 reads, sub_41EC84 22592/22604/22620-22622 writes,
sub_42331C 25485-25487 carried-bomb read), every +78 site inside
`sub_41F29B` (22985-23054, 23110-23319, 23396-23407), and the +78 write in
`sub_421F7E` (24348). No other writer exists in the decompile; values 8-19
and >39 are asserted invalid at 23230-23235 ("invalid special").

| +78 | meaning | entry (cite) | exit (cite) |
|---|---|---|---|
| 0 | normal stand/walk | round start; every exit below | — |
| 1 | kick anim | mover kicks a bomb ahead: `sub_41EC84` 22617-22624 (if +78 is not already 1, set `+78←1; +80←0`, after the `sub_424708` dispatch) | anim complete → 0 (23120-23129); or direction change (the previous-godir word **+44** differs from the new-godir word **+46**) → 0 (23051-23055) |
| 2 | punch anim | action2 edge + punch glove: `sub_41F29B` 23302-23305 (`sub_424A50` returns 1 unconditionally → `+78=2; +80=0`) | anim complete → 0 (23132-23145); or direction change → 0 (23051-23055) |
| 3 | head-hit stun pose | `sub_421F7E` 24347-24349: `+58←16; +78←3; +80←0` — UNCONDITIONAL overwrite, see "clobber" note below | +58 countdown reaches 0 → 0 (22982-22990: decrement +58, and if it has just reached 0 while +78 is 3, set `+78←0; +80←0`) |
| 4 | pickup (grab) anim + pause | drop-block grab of own bomb underfoot: 23310-23320 (`sub_424AF4(bomb, player)` links +148 both ways, sets the BOMB's motion word to 3, clears the player's +57; then `+78=4; +80=0`) | "pickup" anim complete → 0 (23396-23407). NOTE: state 4 is only the pickup ANIMATION — carrying itself continues in state 0 (+37/+148 carried-bomb pointer; the walk anim switches to `walkbomb`/`standbomb` via +37 at 23088/23099) |
| 5 | trampoline hop | mover step-on centring (the no-keyed-direction, tile-centre-aligned case) over an actor with `type == 3`: `sub_41EC84` 22601-22606 (`bounce-flag on the actor; +78=5; +80=0; sound 350`) | +80 counter reaches getvalue(680)=30 → 0 (23158-23165); apex teleport at counter == 680/2 (23169-23187, the rand%5-twice ×100-try loop) |
| 6 | warp-out | mover step-on centring over actor `type == 1`: `sub_41EC84` 22590-22599 (`+78=6; +80=0`; dest stored to +20/+24 via `sub_405A81`; sound 1330) | +80 counter > 8 → 7 (23200-23213), position ← stored dest (+28/+32 = +20/+24 at 23211-23212) |
| 7 | warp-in | from state 6 only (23209) | +80 counter > 8 → 0 (23215-23226) |
| 20-39 | cornerhead idle-fidget | fully enclosed (the blocked-neighbour tally reaches 4, i.e. all 4 neighbours blocked) and state 0: 23006-23013 `+78 ← rand % max(1, getvalue(330)) + 20` | anim complete → 0 (23236-23246); or no longer fully enclosed → 0 (23001-23004) |
| 8-19, >39 | INVALID | never written | asserted at 23230-23235 |

What each state ALLOWS (all cites `sub_41F29B` unless noted):

- **New-input acquisition** (a local flag, initialised to 1 at 22981; the
  single gate at 23028-23039 runs `sub_41E61E` / AI `sub_40A1C6` only when
  that flag is set AND the game is not paused): the flag is forced
  0 by +58>0 (22982-22984, INDEPENDENT of +78), states **5/6/7** (23015-23016,
  redundantly 5 again at 23026-23027), and state **4 while its +80 counter is
  still <= getvalue(665)** (23017-23025 — which ALSO forces the bomb-key byte
  `+56 = 1`, so the bomb-action block's release-throw `!+56` cannot fire
  during the pause
  and the drop block sees no fresh edge). States 1/2/3/20-39 do NOT clear the
  flag
  themselves — you can steer during a kick/punch anim (steering cancels it),
  and state 3 is input-blocked only via its paired +58 counter.
- **Movement**: the mover (`sub_41EC84` via the keyed branch 23430-23453 or the
  idle-conveyor branch 23413-23429) is NOT +78-gated; it is driven by +46
  (want-godir), which stays -1 whenever the input flag was 0. So states 5/6/7 don't move
  (input blocked, and their tiles are trampolines/warpholes, not conveyors),
  but a state-3/4-blocked player on a CONVEYOR is still carried (the belt
  forces +46).
- **Bomb actions** (the bomb-action block, 23277-23380): reached EVERY alive
  tick — state
  5 jumps there explicitly (the jump at 23198) and 6/7/20-39 fall
  through the shared anim tail at 23248 into it. The auto-drop disease
  forcing (+135/+137 → +56=1,
  23279-23284) and the carried-bomb release check (when `+37` holds a carried
  bomb, throw it if the auto-drop flag is set OR `+56` is clear,
  23285-23297) therefore run in ALL states. Consequence: a
  player entering 5/6/7 while carrying has +56 = 0 from the first blocked tick
  (input skipped → key bytes stay at their per-tick reset, 22976-22979), so
  the carried bomb is THROWN at their current position on the first tick of
  the flight — carrying-through-a-warp is impossible. The edge-gated blocks
  (action2 23298-23309, drop 23310-23380) never fire while input is blocked
  because +56/+57 stay 0 (exception: the state-4 pause's forced +56=1 sustains
  "held", which is not an edge).
- **Death** (`sub_41DE63`, the single kill funnel for flame 22917 + in-mover
  crush 22706 + enclosure crush + rover landing): early-outs, returning 0
  with NO death and NO RNG draw, for `+78 == 5` and `+78 == 6 || 7`
  (21999-22006). States 3 and 4 do NOT protect. (+102 spawn-invuln and +8
  already-dead are checked deeper, in `sub_41DCB2` 21921.)
- **Head hit** (`sub_421F7E` via the flying-bomb landing, `sub_42331C`
  25443-25449): the victim probe `sub_421CB5` (24197-24212) accepts a slot
  whose **+0** is truthy and whose **+8** is 0 — active and not-dead, NO +78
  guard — and the landing's victim scan
  runs BEFORE the warphole probe (25445 vs 25452), inside the
  wall/bomb/powerup-clear verdict. So a bomb CAN land on a bouncing/warping
  player, and the head-hit handler's `+78 ← 3` then CLOBBERS states 4/5/6/7: the pause/flight is
  cancelled in place. A warp cancelled during warp-out never relocates (the
  +28/+32 ← +20/+24 write only happens inside the state-6 branch); cancelled
  during warp-in it stays at the exit. There is no re-trigger on the actor
  tile without a fresh centring walk (the trigger lives in the mover's pixel
  loop).
- **Pickup / flame-check / disease contagion**: all inside the `!+8` alive
  block but NOT +78-gated (pickup 22919-22926, flame 22915-22917 — the kill
  is stopped by sub_41DE63's own 5/6/7 exemption, not by skipping the check —
  contagion/aging ~22928-22975). A mid-flight player still collects powerups
  under them and still spreads/catches disease.
- **Carried-bomb anim** (`sub_42331C` case 3, 25480-25497): the carried bomb
  reads the CARRIER's +78 (`== 4`) and +80 to place itself along the pickup
  arc (getvalue(500+2k)); in any other state it rides at the carry offset.

**Head-hit stun (+58) vs pickup-pause (state 4) are INDEPENDENT counters.**
`sub_421F7E` writes +58 and the state word; the grab path
(`sub_424AF4` 26018-26025) writes NEITHER — it only links +148 both ways, sets
the bomb's motion word 3, clears +57, plays sound 170; the pause comes from
state 4's own +80-vs-getvalue(665) window. A head hit DURING the pause
clobbers state 4 → 3 (pause cancelled) and starts the 16-tick +58 countdown.

### Port parity (fixes landed 2026-07-11, this audit)

Our port's mapping: state 0 = default; 1/2/20-39 = presentation-only anim
states (no sim field — they gate nothing but their own sprite pick, and the
kick/punch events already drive the poses); 3 = `stun > 0` (+58 is `stun`);
4's pause = `pickup_pause` (NEW field — was conflated into `stun`);
5 = `bounce > 0`; 6/7 = `warp > kWarpMid` / `warp <= kWarpMid`; carrying =
`carrying` (a flag, matching the original's +37/+148 pointer which is likewise
state-independent). Divergences found and fixed (each at the transition point,
citing the machine):

1. **`stun`/`pickup_pause` conflation** — one field served both the +58
   head-hit countdown and the state-4 grab pause. Split (player.hpp,
   bombs.cpp try_grab, simulation.cpp player_turn + AI gate, hash.cpp new
   mix word). Behaviourally the conflation was masked (a grab needs an input
   edge, which +58 blocks; a head hit overwrote the pause exactly like the
   state clobber), but the fields' independence is load-bearing for fixes
   2-3.
2. **Illegal combo `carrying && (bounce || warp)`** — our early-returns for
   bounce/warp skipped the throw block for the whole flight, holding the bomb
   through a warp. Original: the bomb-action block runs in states 5/6/7 with
   +56=0 →
   thrown on the FIRST flight tick. Fixed: `player_turn` releases the carried
   bomb (throw_carried) on entering the bounce/warp branch.
3. **Illegal combo `(bounce || warp) && stun`** — our head_hit left an
   in-flight bounce/warp running under the new stun. Original: the head-hit
   handler's `+78 ← 3`
   clobbers 5/6/7. Fixed: `PowerupSystem::head_hit` zeroes `bounce`, `warp`,
   `pickup_pause` (latches left set — re-trigger needs a fresh centring walk).
4. **Flame kill missing the 5/6/7 exemption** — `field_vs_players` killed a
   bouncing/warping player standing in flame; `sub_41DE63` early-outs (the
   enclosure crush already ported this same exemption). Fixed (bounce/warp
   gate on the kill only; pickup below it intentionally NOT gated). Same fix
   in the rover landing kill (rovers.cpp), which funnels through sub_41DE63.
5. **Flying-bomb landing skipped the victim scan on warphole tiles** — we
   folded the warphole into the same "blocked" verdict as walls, but the
   original checks the victim FIRST (25445) and only then the warphole
   (25452): a player stranded on a warphole IS head-hittable. Fixed in
   `BombSystem::fly` (warphole now only blocks settling).
6. **AI drew RNG while bouncing/warping** — the new-input flag is 0 in states
   5/6/7, so the
   original never reaches the AI dispatch; our `ai.decide` gate only checked
   stun. Fixed (gate extended to pickup_pause/bounce/warp).

**RESOLVED 2026-07-11 (follow-up commit):** the standing head-stun's
bomb-action-block
auto-drop/throw-while-stunned edge, deferred here, is now ported — see
"The bomb-action block runs in every alive state (standing-stun
restructure)" below.
states 1/2/20-39 stay presentation-side (nothing in the sim reads them; the
direction-change cancel and enclosure-entry rolls affect only sprite choice).
The cornerhead entry roll (23008-23012) draws `rand()` in the ORIGINAL sim
loop, but per determinism rule 6 it is cosmetic (sprite pick only, no
gameplay effect) and stays out of `State::rng` — same collapse as the
death-variant roll ("Death powerup scatter" entry).

GOLDEN: fixes 2-6 are inert on every golden scenario (no trampolines/
warpholes/rovers/AI there; scenario B's grabs never coincide with a head hit —
verified by the unchanged kExpectedRng/bounce-count assertions). Fix 1 adds a
`mix(pickup_pause)` word to `state_hash` — a hash-LAYOUT-only change under
determinism rule 5 (the mixed value is 0 everywhere a grab isn't in its
2-tick pause), so all five golden hash constants were recaptured in this
commit; every `kExpectedRng` checkpoint, bounce count, and final-rng value is
byte-identical, proving the recapture is layout-only, zero behaviour drift.
Tests: `tests/test_state_machine.cpp` (new suite pinning the table's
transitions and each forbidden combination).

## The bomb-action block runs in every alive state (standing-stun restructure) — RESOLVED (`sub_41F29B`, 2026-07-11 follow-up)

Follow-up to the two audits above (both deferred this same item, each citing
the other). Full re-read of `sub_41F29B` (function body 22740-23497; the
player record is typed as a 16-bit-element pointer there, so every element
index `N` in the decompile is byte offset `N*2`) to pin exactly
which sub-actions the bomb-action block (23277-23380) contains and their
`+54..+57`
conditions, cross-checked against the earlier "Player state machine (+78)"
and "Diarrhea/super auto-drop × grab-glove" entries (which already document
most of this — this entry supplies the missing piece: that the bomb-action
block's reach is unconditional on +78, including the plain head-stun state 3).

**Reachability (why it runs in ALL states):** the giant alive-block
(22904-23456), gated on the player's dword at **+8** being 0, contains the
walk/anim
display code gated on `+16==4` (23079-23408) and, INSIDE that, the state
dispatch on the **+78** state word: states 0-3 take the punch/kick/idle
branch (23112-23151); state 5 jumps explicitly to the bomb-action block
(23198, after
positioning a mid-air bomb sprite); states 6/7/>7 fall through their own
branches into the shared anim tail at 23248, which unconditionally continues
into the bomb-action block at 23277 (no jump, straight fall-through,
confirmed by reading
the raw line sequence 23248-23277 with no intervening `return`/`goto`).
State 3 (head-stun) and state 4 (pickup-pause) are NOT special-cased in this
dispatch at all — they take the same "state word < 4" branch as state 0
(states 3 and 4 both land there, per this entry's original reading
"since 3,4 < 4"), which
itself falls through to the 23248 tail and then the bomb-action block after
the anim-pick
`switch` (23112-23151 has no early return either). **So the bomb-action block
is reached
on every alive tick regardless of +78**, confirming facts.md's own summary
line ("Bomb actions... reached EVERY alive tick") — this entry's contribution
is tracing the CONTROL FLOW proof end to end and porting the standing-stun
case, which both prior audits explicitly left out of scope.

**Effective key-byte model (`+54/+55` = last tick, `+56/+57` = this tick):**
the shuffle `+54←+56; +55←+57; +56←0; +57←0` (22976-22979) runs UNCONDITIONALLY
at the top of every alive tick, before the new-input-acquisition
gate is even computed. That gate (a local flag initialised to 1 at 22981) is
cleared by: +58>0 (head
stun, 22982-22984), state 4's own pause window (23017-23025 — which ALSO
FORCES `+56=1`, uniquely among the blocked states), and states 5/6/7
(23015-23016/23026-23027). Only when the flag stays 1 does the real controller
read (`sub_40179F`/AI `sub_40A1C6`, 23030-23038) run and set `+56/+57` from
the actual input. So going into the bomb-action block, `+56/+57` are 0 for
EVERY blocked
state except state 4 (pickup-pause), which is uniquely forced to 1 (held, not
an edge) — the standing head-stun (+58>0) and bounce/warp (5/6/7) all leave
the keys at their bare 0 reset, identically.

**Bomb-action truth table** (23277-23380, in order; `auto_drop` is the
block's own auto-drop flag, computed at 23278-23284):

| # | block | condition | action | cites |
|---|---|---|---|---|
| 1 | auto-drop force | `+135 (diarrhea) \|\| +137 (super)` | `+56←1; +54←0; auto_drop←1` — unconditional override, runs regardless of +78 or `blocked` | 23279-23284 |
| 2 | carried throw | `+37 (carrying)` and (`auto_drop` or `!+56`) | launch via `sub_424987`, fuse reset (`+68←0`), `+37←0`; NOT gated by constipation | 23285-23297 |
| 3a | action2: kick-stop | `+57 && !+55`, then `+89` | flag every own sliding non-jelly bomb to halt at next centre | 23298-23301 |
| 3b | action2: punch | `+57 && !+55 && !+56`, then `+91` | `sub_424A50`; `+78←2` | 23302-23306 |
| 3c | action2: trigger | `+57 && !+55`, then `+95` | `sub_424B41` detonates the oldest own trigger bomb | 23307-23309 |
| 4a | drop: grab | `+56 && !+54 && !+134`, then `+92` and own-bomb-underfoot | `sub_424AF4`; `+78←4` (pickup-pause) | 23310-23321 |
| 4b | drop: spooge | same edge, `auto_drop` clear, `+93` and own-bomb-underfoot | lay a line of bombs, one tile/tick | 23322-23342 |
| 4c | drop: plain | same edge, else | `sub_41EB13` new bomb, RNG dud roll | 23343-23379 |

Blocks 3/4 share one edge gate each (`+57&&!+55` / `+56&&!+54`); with `+56/+57`
pinned at 0 while blocked (state 4 excepted), NEITHER edge can ever fire
without a real controller read — matching the already-documented consequence
("edge-gated blocks never fire while input is blocked"). Block 1's override
is the one exception: it re-arms `+56/+54` EVERY tick regardless of `blocked`,
so blocks 2 and 4 (but not 3, which only reads `+57/+55`, untouched by block 1)
keep firing under auto-drop through a stun, a bounce, or a warp.

**Port** (`player_turn`, simulation.cpp): the four blocks, previously computed
inline only in the `!stunned` path (2026-07-04 commit, "Diarrhea/super
auto-drop × grab-glove"), are now a single `bomb_actions(bool blocked)` local
lambda called from THREE sites — the bounce branch, the warp branch, and the
tail of the normal path — replacing: (a) the bounce/warp branches' narrow
`if (p.carrying) bombs.throw_carried(...)` patch (now redundant: the same
release falls out naturally from block 2 with `blocked=true`, `a1_now=false`),
and (b) the tail's `if (!stunned) { ...four blocks... }` skip for a plain
head-stun (`p.stun>0`, no bounce/warp/pickup-pause). `blocked` maps `+56/+57`'s
this-tick default to 0 (mirroring the reset that never gets overwritten by a
real read) unless block 1's auto-drop override fires inside the lambda
regardless. `p.prev_action1/2` (`+54/+55`) are now latched to the EFFECTIVE
`a1_now/a2_now` used this tick (post-override, post-blocking) rather than the
raw `in.action1/2` the prior port latched — a related correctness fix: the
original's `+54=+56` copy at the top of the NEXT tick reflects whatever `+56`
was left at (which can be the auto-drop-forced 1 even if the real button was
never pressed), and the raw-input latch only coincided with this whenever
auto-drop's own in-block override was inert anyway.

**pickup_pause (state 4) is explicitly NOT covered by this restructure** — the
original forces `+56=1` SUSTAINED (not a fresh edge) for the whole pause
window, a materially different rule from every other blocked state's 0
default; modelling it needs a third `blocked`-like mode this entry does not
add. `bomb_actions` is therefore still fully SKIPPED whenever
`p.pickup_pause>0` at all three call sites (the pre-existing behaviour,
unchanged), with the pre-existing narrow bounce/warp-entry release preserved
verbatim for the (vanishingly rare, conveyor-into-actor-tile) case where a
pickup-pause window is still open when a bounce/warp starts. `p.stun>0` and
`p.pickup_pause>0` never coincide entering `player_turn` in practice: a fresh
grab needs an input edge, which a stun already blocks, and
`PowerupSystem::head_hit` (port-parity fix 3, above) clears `pickup_pause` the
instant it sets `stun`.

**GOLDEN: proven inert, zero recapture.** The full suite (`ctest --test-dir
build/headless -C Debug`, all 42 registered suites incl. `test_golden`) is
byte-identical before and after this restructure — every hash constant,
`kExpectedRng` checkpoint, and bounce/warp count in `tests/test_golden.cpp`
is UNCHANGED, so no scenario needed recapture. This matches the prior audits'
own prediction ("it never fires in any current scenario/test"): none of the
golden boards A-E combine a head-hit stun with either a carried bomb or an
active diarrhea/super infection, so the newly-reachable code paths (blocks
1/2/4 executing under `blocked=true` for a plain stun) are never entered on
any golden tick — confirmed empirically, not just by inspection, by running
the identical scenario set through both the pre- and post-restructure binary.
RNG draw count/order is therefore unaffected on every existing scenario;
the auto-drop's dud-roll (block 4c) and the grab/spooge primitives draw
exactly where they always did, just now ALSO reachable from a stunned/
bouncing/warping tick, which no current scenario reaches.

Tests: `tests/test_state_machine.cpp` — "a standing head-hit stun releases a
carried bomb (release-throw while stunned)", "diarrhea auto-drop still fires
every tick during a standing head-hit stun", "diarrhea + grab keeps cycling
grab/throw/drop through a whole trampoline flight" (the last one exercising
blocks 1/2/4 repeatedly across an entire bounce, not just the single release
at entry the earlier port-parity fix already covered).

**Known follow-up, NOT fixed here (flagged, out of scope):** `Player::prev_
action1`/`prev_action2` (the hashed-looking `+54/+55` mirror, doc-commented
"part of state!" in player.hpp) are NOT actually mixed into `state_hash()`
(`hash.cpp` has no `prev_action` reference) — a pre-existing determinism-
contract gap (CLAUDE.md rule 4) predating this restructure, which only makes
the field's correctness MORE load-bearing (it now also gates behaviour across
stun/bounce/warp boundaries, not just plain edge detection). Not fixed in
this commit: hashing it is a "one-time hash-layout growth" everywhere else in
this file, but `prev_action1/2` flip on nearly every human/AI tick with any
button held, so adding it would recapture essentially every golden hash from
the first button press onward — far outside this restructure's isolation
proof. Tracked as a separate follow-up.

## Death powerup scatter — CONFIRMED (`sub_41DBFE`, via the death funnel `sub_41DE63`)

Traced 2026-07-11 (user report: "collected powerups scatter onto the board when
a player dies — our port didn't do this"). This was a genuinely UNPORTED
mechanic, distinct from the head hit.

**The death funnel does NOT scatter.** Every kill (flame `sub_41DE63` @ 22917,
wall crush @ 22706, rover landing @ 27241) routes through **`sub_41DE63`** (the
kill dispatcher), which: early-outs for a network-remote player (`+16 == 4`) or
a bounce/warp-immune player (`+78 == 5/6/7`); picks a death-animation VARIANT
`*(+4) = rand()%max(1,getvalue(105)) + 1`; then calls **`sub_41DCB2`** (the
death applier) which sets the "already died this round" flag `*(+8) = 1`, clears
the head-hit stun word `*(+48) = 0`, updates kill tallies/score, and destroys
the carried bomb (`+148`). Neither function scatters powerups. The variant roll
is the ONLY rand draw at the death tick and it is a **cosmetic death-sprite
pick** — under CLAUDE.md determinism rule 6 it stays presentation-side, NOT in
`State::rng` (our sim never draws it; it collapses the death animation).

**The scatter is `sub_41DBFE`, fired at death-ANIMATION-END.** In the player
updater `sub_41F29B` the dying player plays its `"die green %d"` sequence; when
the anim counter reaches its statecnt (the die-anim-complete branch, pseudo.c
~23474-23478) the game
calls `sub_41DBFE(player)`, then clears `+0` (active) and `+8`, and downgrades
the player's live trigger bombs (`sub_424C47`). `sub_41DBFE` (pseudo.c
~21870-21908):

```
if (player[+16] != 4)                       // not a network-remote player
  for (kind = 0; kind < 15; ++kind) {
    baseline = getvalue(50+kind);           // start-with count for this kind
    if (sub_425C10(kind))                    // flag kind: 5,6,7,9,10
      if (player[86+kind] > baseline) { sub_4255B2(kind); player[86+kind] = baseline; }
    else                                     // counted kind
      while (player[86+kind] > baseline) { sub_4255B2(kind); --player[86+kind]; }
  }
```

- **What drops:** EVERY kind's surplus over the VALUELST start-with baseline
  getvalue(50+kind). NO count roll and NO kind roll — the token multiset is
  fully determined by the victim's inventory. Flag kinds (`sub_425C10` = kinds
  **5/6/7/9/10** = punch/grab/spooger/trigger/jelly) scatter ONE token and
  reset to baseline; counted kinds scatter one per surplus unit, decrementing
  to baseline. (Kick=3 and Goldflame=8 are NOT flag kinds here, so they take
  the counted branch — but with a 0/1 count that is identical to "scatter one,
  clear". Every real kind's count is 0/1 or a small int, so a single
  `have - baseline` surplus loop reproduces both branches exactly.)
- **This is the OPPOSITE of the head hit.** Head hit (`sub_421F7E`) = a
  rand-LIMITED count (getvalue(670)+rand%getvalue(671)) of RANDOMLY-ROLLED
  kinds (rand%15, 200 tries). Death = the WHOLE surplus, deterministic order.
- **RNG:** the only `State::rng` draws are `sub_4255B2`'s per-token tile
  selection (`x=rand()%W`, `y=rand()%H`, inner budget 100 solid/brick re-rolls,
  outer budget 100 occupancy attempts; a grounded bomb / ANY powerup record / a
  live player burns an attempt, flame does NOT block, token LOST if all fail —
  the SAME primitive and predicate as the head-hit and eviction scatters).
  Iterated in **kind order 0..14** — that sequence is the determinism contract.
  `sub_4255B2` is a no-op for kind 13 (clog) and draws nothing there.
- **Occupancy note:** the scatter runs AFTER `+8` (dead) is set, so
  `sub_421CB5`/our `grid::player_at` (which excludes dead players) lets a token
  land on the victim's OWN tile.

**Ported** as `PowerupSystem::death_scatter(Player&)` (powerups.cpp), called
from all three death sites (`field_vs_players` flame kill in simulation.cpp,
`EnclosureSystem` wall crush, `RoverSystem` landing kill). `held_count`/
`reset_to_baseline` are shared with `head_hit`'s surplus test.

**TIMING — deliberate, documented divergence.** The original defers the scatter
to the death animation's final frame (tens of ticks after death; the DIE*.ANI
length is asset data the SDL-free sim must not know). Our port scatters on the
DEATH TICK — the same animation-delay collapse this sim applies everywhere else
(a dead player is immediately inert). The scatter CONTENTS (kinds/counts/tiles)
and RNG arithmetic are identical; only the tick the tokens appear differs. No
tunable/constant is guessed.

**GOLDEN (recaptured 2026-07-11, same commit).** New draws land ONLY on a death
tick, so every scenario that produces a death shifts: B (first death tick 39,
all 6 checkpoints), C (tick 10, its one hash), D (the disease gauntlet's
auto-drop diseases make the key-silent players lay bombs; first flame death tick
487, so 200/400 stay byte-identical and only 600/800 move), E (tick 231, only
tick 300 + final rng). Golden A (no deaths) is UNCHANGED. Isolation proven by
running this revision against `main`'s sim with identical instrumentation: the
first-death TICK and the full hash+rng at the END of the tick BEFORE each first
death are byte-identical across both builds, confining every divergence to the
death tick's scatter draws. Tests: `tests/test_sim.cpp` ("a dying player
scatters its whole surplus…", "…at its start-with baseline scatters nothing",
"death scatter places the surplus token on the sole legal tile").

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
- **Mutual exclusions** via the evict helper `sub_41E16A`: punch removes
  trigger; grab removes spooger; spooger removes grab; trigger removes punch
  AND jelly; jelly removes trigger. **DEEPENED 2026-07-10 (core-feel audit
  §2):** eviction is not a silent flag clear. `sub_41E16A` splits on
  `sub_425C10(kind)` (true for exactly kinds 5/6/7/9/10 — the flag kinds):
  when the count exceeds the getvalue(50+kind) start-with baseline it
  **SCATTERS the evicted token back onto a random floor tile**
  (`sub_425BED` → `sub_4255B2`, the head-hit scatter — same RNG draw
  pattern) and writes the count back to the baseline (the non-flag branch
  loops, scattering ALL surplus — never reached from the dispatcher). And
  when the evicted kind is TRIGGER (the kind argument is 9) and the flag ends
  cleared, it
  calls **`sub_424C47`**, which walks the bomb array and DOWNGRADES every
  live kind-1 bomb of that player to kind 0 with fuse-elapsed reset to 0 —
  the orphaned trigger bombs relight with a fresh full fuse (they'd
  otherwise sit inert forever, since the flag gates the detonate key).
  Ported: `PowerupSystem::evict` (scatter + downgrade, incl. a carried
  trigger bomb — `sub_424C47` matches on kind alone); `remove()` stays the
  head-hit primitive (the head hit `sub_421F7E` decrements + scatters
  itself and does NOT call `sub_424C47`). Tests:
  `tests/test_trigger_allowance.cpp`. GOLDEN (new scatter RNG draws).
- **Trigger pickup** also zeroes the live-trigger-bomb counter (+85); bomb
  creation lays trigger kind only while `+85 < +86 (max bombs)`.
- **AWESOME cadence**: pickup counter +101 (not incremented by skulls):
  voice at 7, then every 5th; counter wraps to 7 past 50. (Our SoundDirector
  had "every 3rd" — fixed, with the wrap.)
- **Pickup sounds**: normal kinds play the 400 voice group; **jelly plays
  135 ("bombboun")** instead — fixed in SoundDirector.
- Per-kind limits clamp `player[86+kind]` against getvalue(550+kind) in the
  common tail — matches our `PowerupSystem::apply` clamping.

## Floor-powerup icon table — AUDITED, no mismatch found (2026-07-09)

Triggered by a user report that the TRIGGER powerup's floor icon "looks
wrong — should look like a clock/timed bomb". Full audit of the icon
pipeline: kind index -> original name table -> our ANI/PCX name tables ->
shipped asset content. Conclusion: **every layer already matches the
original 1:1; no code fix was needed.**

**Original side.** The floor-powerup drawer is `sub_424F89` (pseudo.c
26219-26266, the per-tick grid scanner that draws a token when its cell
state == 2) and the shared icon builder is `sub_425C7F` (pseudo.c
26718-26735, also reused by the Goldman wheel's 6-slot prize render, §9.5 of
`goldman-roulette.md`). Both build the sequence name via `aPowerS` ("power
%s", pseudo.c 1566) + `off_45BE50[kind]` (pseudo.c 2261-2280), where `kind`
is the grid cell's raw `+4` byte — the SAME unmodified value stored by the
token-drop writer `sub_425383` (pseudo.c 26352-26375, which writes its own
kind argument straight into the record's **+4** with no transformation, that
argument coming straight through from its caller, e.g. `sub_4255B2`'s
scatter roll) and read by the pickup dispatcher `sub_41E21E`'s kind switch
(facts.md "Powerup pickup dispatcher" above) — so the drawer's index space,
the dispatcher's case numbers, and `off_45BE50`'s index are all the SAME
0-13 raw kind, confirmed via three independent call sites, not just one.

`off_45BE50` holds 18 name pointers, in index order:

```
 0 bomb        1 flame      2 disease    3 kicker     4 skate     5 punch
 6 grab        7 spooge     8 goldflame  9 trigger   10 jelly    11 disease3
12 random     13 clog      14 ?1        15 ?2        16 ?3       17 ?4
```
(pseudo.c 2261-2280; the same table `goldman-roulette.md` §9.5 already
extracted for the clogs wheel-slot fix, here transcribed in full.)

**Truth table** (kind index = `sim::PowerupType` value = original's raw
`+4`/dispatcher-case index; verified against the real
`DATA/ANI/POWERS.ANI` via `abtool ani`, 2026-07-09):

| kind | `sim::PowerupType` | original name (`off_45BE50`) | our `kPowerNames` (`sequences.cpp`) | POWERS.ANI frame | our PCX fallback (`kPowFiles`) | status |
|---|---|---|---|---|---|---|
| 0 | ExtraBomb | bomb | "power bomb" | POWBOMB.TGA | POWBOMB.PCX | correct |
| 1 | Flame | flame | "power flame" | POWFLAME.TGA | POWFLAME.PCX | correct |
| 2 | Disease | disease | "power disease" | POWDISEA.TGA | POWDISEA.PCX | correct |
| 3 | Kick | kicker | "power kicker" | POWKICK.TGA | POWKICK.PCX | correct |
| 4 | Skate | skate | "power skate" | POWSKATE.TGA | POWSKATE.PCX | correct |
| 5 | Punch | punch | "power punch" | POWPUNCH.TGA | POWPUNCH.PCX | correct |
| 6 | Grab | grab | "power grab" | POWGRAB.TGA | POWGRAB.PCX | correct |
| 7 | Spooger | spooge | "power spooge" | PWSPOOGE.TGA | POWSPOOG.PCX | correct |
| 8 | Goldflame | goldflame | "power goldflame" | POWGOLD.TGA | POWGOLD.PCX | correct |
| 9 | Trigger | trigger | "power trigger" | POWTRIG.TGA | POWTRIG.PCX | correct — see below |
| 10 | Jelly | jelly | "power jelly" | POWJELLY.TGA | POWJELLY.PCX | correct |
| 11 | SuperDisease | disease3 | "power disease3" | POWEBOLA.TGA | POWEBOLA.PCX | correct |
| 12 | Random | random | "power random" | PWRANDOM.TGA (+ 11 more cycling steps) | PWRAND.PCX | correct |
| 13 | (not a `PowerupType`; clogs) | clog | "power clog" (`clogs_anim`, outside the 13-kind loop) | TURT2.TGA | n/a | already ported, §9.5 |

Every row's ANI sequence name (`libs/game/src/sequences.cpp`'s
`kPowerNames[]`) and PCX fallback filename (`libs/game/src/asset_store.cpp`'s
`kPowFiles[]`) already matched `off_45BE50` exactly, in the same order as
`sim::PowerupType` (`libs/sim/include/bomber/sim/types.hpp`) — which itself
matches the dispatcher's case numbers per "Powerup pickup dispatcher" above.
`Renderer::draw_powerups` (`renderer.cpp`) indexes `powerup_anim[]` directly
by `static_cast<int>(s.floor[y][x])`, so no reordering happens between the
sim's enum and the draw call either.

**The trigger icon itself, pixel-checked**: `POWTRIG.TGA` (POWERS.ANI frame
9) and `POWTRIG.PCX` are byte-identical in content (dumped and visually
diffed via `abtool ani`/`abtool pcx`, 2026-07-09) — a black bomb with a
lit/sparking fuse and a yellow "Tr" mark, i.e. already the "timed bomb with
a marker" look the bug report described wanting. There is no separate
"clock face" asset anywhere in `POWERS.ANI`'s 15 frames; the lit-fuse bomb
+ "Tr" IS the original's trigger icon.

**Conclusion**: the reported visual bug does not correspond to any
kind-index / sequence-name / PCX-name mismatch in this codebase — every
layer of the floor-powerup icon pipeline already reproduces `off_45BE50` +
`aPowerS` faithfully for all 13 kinds. No code change made. If the in-game
icon still looks wrong to a live build, the cause is outside this table
(e.g. a stale binary, a recolour/z-order issue at render time, or a
different UI element than the floor pickup) and needs a live screenshot to
diagnose further.

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
  (`sub_41E21E` case 9, whose FIRST statement writes 0 into the player byte
  at **+85**). There is **NO
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
- **Drop-time reach** (`sub_41EB13`): the flame reach is derived per bomb, in
  this order — start from `player[+87]` (the flame stat); if `player[+136]`
  (short-flame) is set, force it to 1; if `player[+94]` (goldflame) is set,
  force it to `gridW <= gridH ? gridH : gridW`. So the
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
   The slide's re-steer — look the current tile up with
   `sub_405654(tileX, tileY)`, and if a record exists AND its type word at
   **+4** is 0 (dirarrow), copy that actor's godir word at **+44** into the
   bomb's own direction word **+44** — reads the **level-actor registry**
   `dword_45E0A8`
   (allocated 152×100 at `sub_404D16`), whose entries are stage objects parsed
   from the level file: **type 0 = DIRARROW**, type 2 = conveyor, type 3 =
   trampoline, type 1 = warphole (registration at `sub_...6970-7086`, strings
   "dirarrow"/"conveyor"/"trampoline"/"warphole"; godir at actor `+44`). It is
   **not** the human-player array (`dword_461BC4`). So the original has no
   player-based bomb re-steer — a sliding bomb adopts a **directional-arrow
   tile's** direction. We do not model dirarrows/conveyors yet, so there is
   nothing faithful to add; this belongs to ROADMAP #7 (conveyors/trampolines).
   (This corrects the task's "resting player re-reads godir" premise.)
   Sidenote — **CORRECTED 2026-07-10 (core-feel audit):** the earlier claim
   here that `sub_4230A5` "does not test for players" was wrong. Re-read of
   the function (0x4230A5): its SECOND check calls `sub_421CB5` on the
   candidate tile and returns 0 (blocked) the moment it hits — and
   `sub_421CB5` IS the player-at-tile
   scan (the head-hit helper). A sliding bomb is **blocked by a live
   player**, exactly as our slide already behaved; the "future pass" this
   note requested is unnecessary. (It also probes `sub_405654` type 1 —
   sliding bombs cannot ENTER a warphole tile via normal passability; the
   warp handling happens elsewhere in the mover.)
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

## Core-feel audit 2026-07-10 — line-by-line arithmetic pass (movement / drop / powerups / timing)

A full side-by-side re-read of `sub_41F29B` (player updater), `sub_41EC84`
(pixel mover), `sub_41EB13`/`sub_422EDE` (bomb creation), `sub_41E21E`
(pickup dispatcher, incl. `sub_41E16A`/`sub_424C47`/`sub_41DFB6`),
`sub_421F7E` (head hit), `sub_42464B`/`sub_4247C5`/`sub_424B41` (kick/stop/
detonate handlers) and `sub_422C13` (dud gate) against the sim, triggered by
a "core feel deviates" report. Verified-identical areas (no change): the
per-pixel mover's corner/glide/settle resolution and its (dir±1)&3 rotations;
the walk-speed budget `base(42=923) + skates*90 − clogs*91`, molasses ÷3 then
hyper ×3/2, frame-scaled, conveyor ±getvalue(190+idx) after disease scaling;
base speed set once at spawn from getvalue(42); the opposite-key resolution
(blocked-pressed-dirs filter, last-index wins); ice buffering order; bomb
kind exclusivity + trigger allowance + goldflame/short-flame ordering; fuse
40 (id 41, stored as duration for every kind); drop tile-snap via the
floor-division tile of the player centre; head-hit stun 16 / drop-count /
kind-roll / scatter; the 550-block caps in the pickup tail; cure roll before
dispatch; Random reroll `%12` ×200; skull rolls (1 vs 3, first announces);
disease durations 50ms×getvalue(130+i); flame lifetime 10. Six deviations
were found and fixed (each cites its sub above; GOLDEN recaptured in the
same commit, `tests/test_golden.cpp` 2026-07-10 note has the per-scenario
proofs):

1. **Kick timing + redirect (`sub_41EC84`'s in-pixel-loop kick probe branch,
   `sub_42464B`).**
   The kick check lives INSIDE the per-pixel loop, firing whenever the
   player sits on the tile centre along the travel axis with a bomb ahead
   and the tile beyond it passable — so a walk-up kicks on the ARRIVAL tick
   (our old post-stall gate was one tick late on every approach), a parked
   player holding the direction re-fires every tick, and `sub_424708 →
   sub_42464B` accepts a bomb ALREADY SLIDING (`sub_422E48` matches motion
   0 and 1): same direction = silent speed refresh (no sound: `sub_42464B`
   plays 120 only when `dir != new || !moving`), different direction =
   **snap to tile centre + redirect, still sliding**. Ported as a post-move
   centred-along-axis probe in `player_turn` (same-tick equivalent of the
   in-loop check; the belt-forced no-input mover probes along the belt dir)
   plus the redirect branch in `try_kick`. Tests: `test_kick_nuances.cpp`.
2. **Powerup eviction scatters + trigger downgrade (`sub_41E16A`,
   `sub_424C47`).** See the deepened "Mutual exclusions" bullet in the
   pickup-dispatcher section: evicted tokens return to the board via the
   scatter, and an evicted Trigger converts the player's live trigger bombs
   to fresh-fused normal bombs. `PowerupSystem::evict`.
3. **Drop/spooge block details (`sub_41F29B`'s bomb-action block).** (a) A drop on a
   WARPHOLE tile is refused (`sub_405654` type 1 short-circuits placement;
   sound 40/41 "enrt" unless disease-auto-drop — `Event::DropRefused`).
   In practice this is a HUMAN-only sound: an AI never presses the bomb key
   on a warphole, because `sub_423188` vetoes the tile first — see "AI never
   bombs a warphole" (2026-07-26) below, which also corrects a wrong
   2026-07-24 note that claimed otherwise.
   (b) The spooge branch requires the underfoot bomb to be OWN (owner word
   +62 == self), not just any bomb. (c) The spooge run ALSO stops at a live
   player (`sub_421CB5` is the loop's first break). (d) The run index n is
   passed to `sub_41EB13` → `sub_422EDE` inits fuse-elapsed to −50·n ms:
   each successive spooge bomb burns **one tick longer** — the line pops as
   a near-to-far cascade, one tile per tick (our old port detonated the
   whole line simultaneously). Tests: `test_spooge.cpp`,
   `test_stage_actors.cpp`.
4. **Kick + action2 stops own sliding bombs (`sub_4247C5`).** The action
   edge-gate's FIRST branch (before punch and trigger): a kick player's
   action key sets byte +57 on every own SLIDING, non-jelly (kind != 2)
   bomb; the slide loop consumes it at the next at-or-past-centre step
   (stop flag `+57` set AND the bomb at-or-past the tile centre along its
   travel axis → snap + stop), and a DIRARROW clears it (~25535).
   Previously missing entirely. `Bomb::stop_pending` (hashed),
   `BombSystem::stop_own_sliding`. Also from `sub_424B41`: the trigger
   detonate scan exempts ONLY carried (3) and flying (2) — a SLIDING
   trigger bomb detonates fine (ours wrongly excluded it), and it picks the
   OLDEST by creation stamp (+64), which our creation-ordered vector's
   first match reproduces. Tests: `test_kick_nuances.cpp`,
   `test_trigger_allowance.cpp`.
5. **Throw restarts the fuse** (`sub_41F29B`'s bomb-action block, the `+37`
   carried-bomb release: the carried bomb's fuse-elapsed WORD at **+68** is
   written 0). The carried bomb's fuse-elapsed is zeroed
   right before the launch — a thrown bomb lands with its complete
   creation-time duration (incl. a short-fuse ÷3 baked at creation), not
   the remnant frozen at grab. Also `sub_422E48` (the grab's underfoot
   probe) matches motion 0 AND 1: a player can grab their own bomb
   mid-slide. Ported via `Bomb::fuse_init` (hashed; the duration word +74
   the original stores for every kind). Tests: `test_punch_throw.cpp`.
6. **Reversed-controls application point (`sub_41F29B` ~23049).** The flip
   is `(godir + 2) & 3` on the RESOLVED direction — after the opposite-key
   passability filter ran on the RAW pressed dirs, before the ice buffer
   push (delayed samples store the flipped value) — and it is gated
   `+16 != 1`, i.e. **humans only**; an AI's direction is never flipped.
   Our old port swapped the four input flags pre-resolution (divergent
   under multi-key input) and flipped AIs too. Also from `sub_41DFB6`: the
   skull's 200-try reroll of Swap is NET-game-only; locally a Swap with no
   valid target is simply LOST (nothing assigned), not rerolled into a
   different disease — `assign_random` now matches. Tests: existing
   disease/ice suites still pin the composition order.

Plus the **dud-gate units correction** (see "Dud bombs": VALUELST 320/321
are SECONDS — one opportunity per 3–6 minutes, not 9–18 s; re-arm is `+=`
on the previous deadline) and the **skate-recompute clogs term** (a skate
pickup/loss recomputes `speed = 923 + skates·150 − clogs·150`; the old
recompute dropped the clogs penalty — `sub_41F29B` recomputes the whole
term per tick, so the penalty never vanishes in the original). Deviations
found but deliberately NOT changed, documented for honesty: the original
writes the GLIDE's diagonal direction into the facing word (+44) mid-loop
(a punch thrown mid-glide can aim the lateral way — sub-tick, cosmetic-
adjacent; ours keeps the input facing); the cure roll draws `rand()` even
for healthy players (outcome-identical — our internal RNG draws only when
diseased); and the swap-disease target pick is one draw over the valid set
instead of the original's up-to-200 rejection sampling (outcome-equivalent
distribution). These are internal-RNG/bookkeeping differences with no
player-visible effect; our own draw-order contract stays self-consistent.

## Bomb/warphole reconciliation 2026-07-10 — CONFIRMED (`sub_4230A5`, `sub_405A81`)

Read 2026-07-10, following up the just-merged Core-feel audit's warphole-drop
finding (§3a above) and a report that `sub_4230A5` (the sliding-bomb per-pixel
cell-entry probe, already cited in "Kick nuances" #2) **blocks** warphole tiles
for sliding bombs — apparently in tension with `docs/re/stage-actors.md` §6
item 4, which claimed "Bomb entering a warphole: teleport via the same
`warp_dest` grid used by the player" (and `docs/ROADMAP.md`'s "player & bomb
both warp"). Re-read both functions end to end; the stage-actors.md claim was
wrong and is now corrected there.

**The truth table (bomb state × warphole tile → outcome):**

| bomb state                       | outcome at a warphole tile                                   |
|-----------------------------------|---------------------------------------------------------------|
| Placement (drop)                  | **Refused** — already fixed (Core-feel audit §3a): `sub_405654` type-1 short-circuits the drop, sound 40/41, `Event::DropRefused`. |
| Sliding — kicked (`sub_42464B`)   | **Blocked at the doorstep, stops there** (non-jelly) or **bounces** (jelly) — exactly like a wall. Never enters, never warps. |
| Sliding — conveyor-carried (`BombSystem::conveyor_carry`/case 0) | **Same block** — shares the identical per-pixel stepper and `sub_4230A5` probe as the kicked case. |
| Sliding — dirarrow-redirected (case default, mid-slide) | **Same block** — the redirect only changes `dir`; the very next tile-entry probe still runs `sub_4230A5`. |
| Flying (punched/thrown, `sub_42331C` case 2) | **Cannot land there either** — pseudo.c 25453's landing verdict is "no actor on the tile OR (the dead `exp_` term AND that actor's type word is not 1)", which excludes a type-1 actor from the "clear to land" verdict the same way a wall does; the bomb just hops onward (the loop's `continue`), same as over a wall/bomb/powerup. (Pre-existing behaviour, untouched by this pass — see caveat below.) |
| Resting/stationary                | **Cannot occur** — every path that could put a bomb ON a warphole tile (placement, slide-entry, flight-landing) is blocked, so a bomb is never actually located on a warphole tile in the original. |
| — (for contrast) Player, any approach | **Warps** (two-phase, 18 ticks) — `sub_41EC84` step-on, `sub_405A81` idno↔linkto resolver. Unchanged by this entry; see §5/§6 in stage-actors.md. |

**Evidence.** `sub_4230A5` (pseudo.c 25155-25179, called from the kicked/
conveyor slide loop at 25555) runs, on the candidate tile `(tx, ty)`, in this
exact order:

1. `sub_422E48(tx, ty)` — a grounded bomb here → return 0 (blocked).
2. `sub_421CB5(tx, ty)` — a live player here → return 0 (blocked).
3. `sub_42542D(tx, ty)` — look up the powerup record. If one exists AND its
   state field reads 2 (visible on the floor), destroy it via `sub_4254F3`,
   and — when its kind is 2 (disease) and `diseases_destroyable` is off —
   scatter a replacement skull. This is a SIDE EFFECT: it happens whether or
   not the tile turns out to be enterable.
4. `sub_405654(tx, ty)` — level-actor lookup.
5. Return **(no actor OR that actor's type word at +4 is not 1)** AND
   `sub_425FB9(tx, ty) == 0`.

Step 5 is the crux: passable requires **(no actor OR actor.type != 1)
AND blank cell**. If an actor exists and its type IS 1 (warphole, per the
`+0=active,+4=type:0=dirarrow/1=warphole/2=conveyor/3=trampoline` layout
already pinned in stage-actors.md §1), the whole expression is `false`
**regardless of the underlying cell type** — a warphole tile is impassable to
a sliding bomb exactly like a solid wall, full stop.

The warp resolver `sub_405A81` (pseudo.c 7341-7388, idno↔linkto partner scan,
zero RNG) has **exactly one call site in the entire binary**: `sub_41EC84`
line 22594 (the PLAYER per-pixel stepper's step-on handler, reached on the
"no keyed direction" / tile-centre alignment case). `grep -n "sub_405A81" pseudo.c` confirms this —
declaration, definition, one call. `sub_42331C` (the bomb mover) calls
`sub_405654` three times (bomb-on-conveyor check ~25365, flying-landing check
~25452, dirarrow-restring check ~25529) and **never** calls `sub_405A81`. So
there is no code path anywhere that computes a teleport destination for a
bomb — the earlier stage-actors.md claim was simply never backed by a real
call site.

**Divergence found and fixed.** Our port's `BombSystem::slide()` (`libs/sim/
src/systems/bombs.cpp`) had an `at_centre && actor_type == Warphole` branch
that teleported a sliding bomb to `warp_dest_x/y` and set a `Bomb::warp_latch`
one-shot guard — unfaithful, since the entry probe (our analogue of
`sub_4230A5`) never actually treated a Warphole tile as impassable, so a
sliding bomb could reach and "use" a warphole. Fixed:
- The cell-entry probe now adds `actor_type[ny][nx] == Warphole ⇒ blocked`,
  mirroring `sub_4230A5`'s "actor type at +4 is not 1" verdict (checked after the powerup
  squash, same order as the original: the squash is unconditional on actor
  type, only the final passability verdict cares about it).
- The now-unreachable teleport branch (and `Bomb::warp_latch`, a hashed field
  that served no purpose once bomb-warping is impossible) were removed.
  `hash.cpp` keeps bit 42 retired rather than reassigned, so no other bomb
  field's shift moved.
- `docs/re/stage-actors.md` §6 item 4 and `docs/ROADMAP.md` roadmap item #7
  corrected to match.

**RESOLVED 2026-07-10 (flame-system audit) — flying-bomb landing now blocks
warpholes too.** The landing check's exact condition is "**no actor on the
tile**, OR (**`exp_`** AND **that actor's type word at +4 is not 1**)", where
`exp_` decompiles to a bare (no call parens) reference to
`sub_4443CC` — a real, statically-linked CRT `exp()` implementation (a
three-instruction thunk: load the double argument off the stack, call
`sub_44436A`, return popping 8 bytes — matching the declared stdcall
`exp(double)` signature), confirmed by direct disassembly of `BM95.EXE`
around the reference site. Two independent facts pin `exp_`'s contribution
as inert:
- **`idautils.XrefsTo`** against the `BM95_copy.idb` database finds exactly
  **one** cross-reference to `sub_4443CC` in the entire binary: a `dr_O`
  (offset/immediate load, not a call) at `0x423B11`, inside `sub_42331C`
  (the bomb mover) — i.e. `exp()` is never called anywhere in the program;
  its only "use" is this one address load.
- **Direct disassembly at `0x423B08-0x423B26`** (raw bytes, capstone). The
  nine instructions, in order:

  | addr | what happens |
  |---|---|
  | 423b08 / 423b0f | test the actor pointer local against 0; if NULL, jump to 423b28 = **land** (skipping the `exp_` term entirely) |
  | 423b11 | load the compile-time constant `0x4443cc` (the address of `exp`) into `eax` |
  | 423b16 / 423b18 | test that value against 0 and branch to 423b26 if zero — **NEVER TAKEN**, the value is a hardcoded non-null address |
  | 423b1a / 423b20 | reload the actor pointer and compare its **+4** type field against 1 |
  | 423b24 | if the type is NOT 1, jump to 423b28 = **land** |
  | 423b26 | otherwise fall into a jump to 423b93 = **hop onward** (the type IS warphole) |

  The zero-test at 423b16-423b18 tests a hardcoded non-null
  pointer for zero — a branch that can never be taken. The `exp_` term is
  provably dead code, not a mislabeled integer op or a jump-table artifact;
  the compiler could not fold it away because, at COMPILE time (before the
  linker assigns concrete addresses), an external symbol's address is not
  yet known to be non-null, so it still emits a real (if unreachable) test.
  With the dead term removed, the REAL condition is exactly "**no actor on
  the tile, OR that actor's +4 type is not 1**" — the identical "type 1
  (warphole) blocks like a wall, any
  other actor is fine" rule already confirmed for the sliding-bomb probe
  (`sub_4230A5`) above.
- (Sidenote, same disassembly window: at `0x423ab4-0x423aec`, a player found
  under the bomb — `sub_421CB5`/`sub_421F7E`, the head-hit call — jumps
  straight to `0x423b93` with no `continue`/settle path, i.e. a headshot
  bounces the bomb onward exactly like a blocked tile; already matched by
  `BombSystem::fly`'s existing `victim >= 0` handling, no change needed.)

Ported: `BombSystem::fly`'s `clear` computation gained
`s.actor_type[ty][tx] != ActorType::Warphole` alongside the existing
wall/bomb/powerup checks. Tests: `tests/test_stage_actors.cpp` "a flying bomb
cannot land on a warphole; it hops onward instead" (plus a same-setup control
over open ground, to isolate the actor check from the rest of the landing
logic).

**GOLDEN IMPACT (both the original sliding-bomb fix and the flying-bomb
fix above): none for the shipped golden scenarios (A-E have no
warpholes), but real for boards WITH warpholes.** Before this fix, a scenario
with a kicked/conveyor bomb sliding into a warphole would teleport it; after,
it stops at the doorstep. Proved by running the full suite before and after:
golden A-E are byte-identical (no warpholes there, so `s.actor_type[..] ==
Warphole` is never true on any tile any golden bomb slides toward — the new
`blocked` branch never evaluates true, and the removed teleport branch was
equally never reached, so removing it changes nothing on those boards
either). The dedicated `tests/test_stage_actors.cpp` warphole suite (which DOES
place warpholes) is the only place behaviour changes, and it has been rewritten
to assert the corrected (blocked, never-warps) behaviour: "a sliding bomb is
blocked at a warphole (never warps, sub_4230A5)", "a jelly bomb bounces off a
warphole instead of entering it", "a bomb resting on a belt is blocked by a
warphole ahead" (replacing the old "a bomb warps through a warphole while
sliding" test, whose premise was wrong). The pre-existing "no bomb can be
dropped while standing on a warphole" test is unaffected (a different code
path, already correct).

(Provenance: `sub_4230A5` pseudo.c 25155-25179; `sub_405A81` 7341-7388, sole
call site 22594 inside `sub_41EC84`; `sub_42331C` bomb-mover call sites to
`sub_405654` at ~25365/25452/25529, no call to `sub_405A81` anywhere in the
function; `exp_` import declaration pseudo.c 980, flying-landing check
pseudo.c 25443-25469; `exp_`/`sub_4443CC` resolution via direct `BM95.EXE`
disassembly (capstone) at `0x423A80-0x423C48` and `0x4443CC-0x444440`, and
`idautils.XrefsTo(0x4443CC)` against `BM95_copy.idb` — read-only queries via
`python-idb`/`pefile`/`capstone` against a scratch copy of the idb, no
exe-derived material committed.)

## AI never bombs a warphole — CONFIRMED (`sub_423188` / `sub_405654`, 2026-07-26)

**Observation first.** Ege, playing the ORIGINAL and the port side by side,
reported that the bomb-refused-on-a-warphole sound (SOUNDLST 40/41, the
`sub_427961(40)` in `sub_41F29B`'s drop block ~23354 — "Core-feel audit" §3a)
**never happens for a computer player in the original**, while the port
machine-guns it. A 2026-07-24 audit note in `bombs.cpp` had concluded the
opposite ("an AI warphole drop plays 40/41 too; an AI simply reaches this branch
rarely"). That note's MECHANISM was right and its PREMISE about our own port was
wrong; this entry records the resolution and supersedes it.

**The gate is on the AI's decision side, in `sub_423188`.** The drop-tile
clearance predicate (0x423188), taking the candidate tile `(tx, ty)`, is
exactly three steps:

1. If `sub_422E48(tx, ty)` is true — a bomb already on this tile — return 0
   (not clear) immediately.
2. Look the tile up in the STAGE-ACTOR registry: `sub_405654(tx, ty)`.
3. Return true only when **(no actor there OR that actor's type is not 1)**
   AND `sub_425FB9(tx, ty) == 0` (blank cell).

`sub_405654` (0x405654) is the **stage-actor registry** scan — `dword_45E0A8`,
stride 38 dwords, matching the record's tile fields at actor
`+28`/`+32`, exactly as pinned in `stage-actors.md` §1 — and the type tested
in step 3 is the actor **type word at +4**
(`0=dirarrow, 1=warphole, 2=conveyor, 3=trampoline`).
So "type != 1" is a **warphole rejection**, and this is the *same tail
expression* as the sliding-bomb cell-entry probe `sub_4230A5` documented in
"Bomb/warphole reconciliation 2026-07-10" above — the two functions are adjacent
in the binary and share the verdict verbatim.

`sub_423188` gates **both** AI drop behaviours, in each case called on the AI's
OWN standing tile and evaluated BEFORE the behaviour's `rand()%N` whim:
- `sub_40AD8D` (blast bricks, priority 3): calls
  `sub_423188(brain+46 >> 16, brain+48 >> 16)` — the brain's own 16.16 tile
  coordinates — and only inside that success branch does it draw
  `rand() % getvalue(915)`; otherwise it returns 0 with no draw.
- `sub_40ABED` (bomb near an enemy, priority 4): returns 0 immediately when
  `sub_423188(...)` is false, BEFORE its `rand()%5`.

The only other writer of the AI's bomb-key byte `+56` is `sub_40BD44` (grab
glove, priority 0), which first requires `sub_422E48(pos)` — a bomb already on
the AI's own tile — and a bomb can never be on a warphole tile (placement,
slide-entry and flight-landing are all blocked there; see the truth table in
"Bomb/warphole reconciliation" above). An exhaustive grep of the AI batch found
exactly those three `+56 = 1` sites. **Therefore an AI in the original never
presses the bomb key while standing on a warphole**, and `sub_427961(40)` — a
global SFX with no per-source gate — is never reached from an AI. The remaining
AI-adjacent bomb press, the diarrhea/super auto-drop (`+135`/`+137` in
`sub_41F29B`'s bomb-action block), is explicitly excluded from the sound by
that same block's own "only when NEITHER +135 nor +137 is set" guard, so it
is silent for humans and AI alike.

**Our divergence (fixed here).** `docs/re/ai.md` §3.3 had glossed `sub_405654`
as "an ENTITY (rover/ghost) — empty in versus" and told the port the term "drops
out"; `AISystem::drop_tile_clear` (`libs/sim/src/systems/ai_grids.cpp`) therefore
implemented only the bomb + blank-floor conditions. Our AI happily parked on a
warp exit next to a brick and pressed bomb on ~1-in-5 eligible ticks, which
`BombSystem::drop` refused and turned into a `DropRefused` event (and the deny
SFX) every time — measured at 9 refusals in 300 ticks in the new regression
fixture. The missing `actor_type == Warphole ⇒ not clear` condition is now
ported. **This is an AI-behaviour fix, not a sound fix**: the sound was only the
symptom of our AI walking onto warpholes and pressing bomb, which was wrong
regardless of what it sounded like. Only type 1 blocks — an AI may still drop on
a dirarrow, conveyor or trampoline tile, and a test pins that.

**RNG / golden impact.** The new condition sits where the original's does: BEFORE
the `rand()` whim in both behaviours, so on a warphole tile the behaviour now
returns 0 **without drawing**, and the dispatcher chain falls through to the
lower-priority behaviours (which take their own draws) — exactly the original's
stream. Golden A-E are **byte-identical** (verified): they have no AI players at
all (`Player::ai` defaults false, no golden config sets it) and no warpholes, so
the branch is unreachable there. The only behaviour change is on boards that have
both an AI and a warphole.

(Provenance: `sub_423188` @ 0x423188; `sub_405654` @ 0x405654; `sub_40AD8D` @
0x40AD8D; `sub_40ABED` @ 0x40ABED; `sub_40BD44` @ 0x40BD44; `sub_41F29B`'s drop
block and its `sub_427961(40)`; the `+56` writer census over the AI batch
0x40A140-0x40BEE7. Cross-checked against the `sub_4230A5` reading already
recorded in "Bomb/warphole reconciliation 2026-07-10" and `stage-actors.md` §1's
registry layout. Live-play observation by Ege is the finding's origin and its
independent confirmation.)

## Chain-reaction timing — CONFIRMED (`sub_423209` queue, `sub_42331C` drain, 2026-07-10 flame-system audit)

A full line-by-line re-read of the flame system (facts.md's own "Core-feel
audit 2026-07-10" precedent, applied here to `flames.cpp`/`bombs.cpp` for the
first time). The central finding: **a flame arm reaching another bomb does
NOT detonate it synchronously.** `sub_423209` (pseudo.c 25195-25204):

It takes the bomb pointer in EAX and the orientation byte in EDX (Watcom
register convention) and does exactly one thing: **while the queue count
`dword_462200` is below 100**, store the bomb pointer into the pointer array
`dword_4621F8` at index `count`, store the orientation byte into the parallel
array `dword_4621FC` at the same index, and post-increment `count`. (It
returns the orientation byte when it pushed, the bomb pointer when the queue
was already full — the return value is never used by any caller.)

This is a bare QUEUE PUSH (two 100-slot parallel arrays + a counter) — it
does not touch the bomb's state at all. The drain sits at the very TOP of
`sub_42331C` (the bomb updater), gated to run once per tick
(`dword_462210 != dword_464994`, pseudo.c 25330-25346), **before** that same
function's 100-slot bomb-processing loop:

The drain, in order:

1. **Once-per-frame gate** — run only when the drain stamp `dword_462210`
   differs from the current frame stamp `dword_464994`; the first statement
   inside copies `dword_464994` into `dword_462210`, so it can fire at most
   once per frame.
2. **For each queued index `i`, 0 up to the count `dword_462200`:**
   - take the bomb pointer from `dword_4621F8[i]`;
   - **if** that pointer is non-null AND the bomb's own state dword at **+0**
     is non-zero (still alive): copy the bomb's fuse-DURATION word **+74**
     into its fuse-ELAPSED word **+68** — forcing it "expired" — and copy the
     parallel `dword_4621FC[i]` orientation byte into the bomb's **+56**;
   - either way (alive or not), clear `dword_4621F8[i]` to 0.
3. **Reset** the count `dword_462200` to 0.

THEN the 100-slot loop runs, and each affected bomb's own fuse-expiry check
(now forced true) detonates it as part of ITS OWN slot's normal processing.

**Four call sites push to this SAME queue**, all confirmed by direct
pseudo.c reads:

1. **A flame arm reaches a grounded bomb** (pseudo.c 25641-25651, inside the
   per-direction arm loop): FIRST the owner word at **+62** of the HIT bomb
   is overwritten with the owner word at **+62** of the EXPLODING bomb
   (ownership transfers from the exploder to the bomb it hits — see "owner
   attribution" below), THEN the hit bomb is pushed with
   `sub_423209(hit_bomb, ((k + 2) & 3) + 1)` — the orientation byte is
   `opposite(k)+1` where `k`
   is the arm's travel direction (1-based; 0 means "no restriction").
2. **A flying bomb lands on flame** (pseudo.c 25459-25465): settles first
   (position/motion committed), THEN, if `sub_42708D` reports flame on the
   settled tile, pushes `sub_423209(bomb, 0)` — unconditional, no exemption.
3. **A trigger-button press** (`sub_424B41`, pseudo.c 26027-26067): scans
   for the oldest live/grounded trigger bomb, `sub_423209(bomb, 0)`.
4. **A sliding bomb enters flame** (pseudo.c 25545-25554, per-pixel slide
   loop): `sub_42708D` at the stepped position; if hit AND the flame
   cell's kind is NOT 9 (brick-burn — see "Brick crumble timing" below),
   push `sub_423209(bomb, 0)`; either way (kind 9 or not) the bomb still
   snaps to the tile centre and stops/bounces in the slide's shared
   stop/bounce tail — jelly reverses and keeps
   sliding, non-jelly halts, exactly like hitting a wall.

**Same tick or next tick depends on WHEN the push happens relative to the
drain**, not on which of the four sites pushed it. [CORRECTED 2026-07-11,
"Per-tick call order — END-TO-END" below: within one frame the BOMB pass runs
FIRST (`sub_4245B9` at `sub_42A191` 29522) and the player pass AFTER
(`sub_420F07` at 29527) — the earlier phrasing here had it backwards. The
conclusions are unchanged, because the drain that catches a player-pass push
is the top of the NEXT frame's bomb pass, which is still the very next
bomb-phase after the press in the flattened event stream:]
- **Trigger-button (#3)** is pushed during frame N's player pass and caught
  by frame N+1's drain — the first bomb phase after the press, with no player
  move in between. A manual detonation is effectively instant, just routed
  through the queue instead of a direct call.
- **Arm-hit (#1), landing-on-flame (#2), and slide-into-flame (#4)** are all
  pushed from INSIDE `sub_42331C`'s own per-slot loop — i.e. AFTER that
  frame's drain already ran — so they wait for the NEXT frame's drain, with
  one full player pass in between. A chain reaction resolves **one link per
  tick**, not the whole chain at once.

**Owner attribution transfers on chain** (site #1's "hit bomb's +62 ← exploding
bomb's +62", executed unconditionally before the queue push): the chained bomb's owner
becomes the TRIGGERING bomb's owner, so `flame_owner`/kill credit follows
whoever's blast actually set it off, not the original placer.

**Flame-on-flame overlap (two INDEPENDENT bombs, no chain involved) —
CONFIRMED IDENTICAL, no change.** `sub_426FCC` (the ignite call, pseudo.c
27479-27504) unconditionally re-initialises the WHOLE flame-cell struct on
every call — state, elapsed-timer, kind, AND the owner/colour byte — with no
"already lit" check anywhere in `sub_42331C`'s arm loop or epicentre block
(confirmed: `sub_425FB9`, the cell-TYPE read the arm-stop logic consults,
reads a completely separate array from the flame array `sub_42708D`/
`sub_426FCC` touch — "Scatter occupancy test" already established this same
separation). So when a second, later (or simultaneous) explosion's arm
crosses a tile another bomb's flame already lit, it simply overwrites that
tile's lifetime AND owner — the freshest flame to touch a tile always wins,
both for how long it burns and who gets kill credit there. Our
`FlameSystem::spread_to`/`ignite_epicentre` already did exactly this before
this audit (`s.flame[ty][tx] = fresh; s.flame_owner[ty][tx] = owner;`,
unconditional, no "already lit" guard) — verified matching, no fix needed.

**A chain-triggered bomb skips one direction of its own blast** (bomb+56,
consumed at pseudo.c 25621: `if (!field56 || k+1 != field56)` — the block
runs, i.e. the arm IS cast, when field56 is 0 OR this k is NOT the stashed
one; so field56 != 0 skips exactly ONE of the four directions). Combined
with the `(opposite(k)+1)` encoding at the push site, a chain-triggered bomb
never re-casts an arm back toward the flame that hit it — the other three
directions fire at full, normal reach. Trigger-button/landing/slide pushes
always use orientation 0 (no restriction).

**The "kind 9" exemption in site #4 is already faithfully modelled by our
architecture without any extra code**: the original stores brick-burn state
as KIND 9 of the SAME flame-cell array a real blast uses, so a sliding bomb's
flame check has to explicitly exclude it. Our port already keeps these in
TWO SEPARATE arrays (`State::flame` for real blast, `State::burning` for
brick-crumble) — `BombSystem::slide`'s flame-entry check only ever reads
`s.flame`, so it already never fires on a merely-crumbling brick tile.

**Fixed:** `FlameSystem::explode` gained a `skip_dir` parameter (applied as
a `continue` in its 4-direction loop); a new `FlameSystem::queue_chain` /
`drain_chain_queue` pair implements the deferred queue (a new hashed
`State::pending_chain` list of `{bomb_id, skip_dir}`, drained once per tick
immediately after the player pass, before bombs move — the same relative
position the original's drain occupies relative to its player pass). A new
hashed `Bomb::id` / `State::next_bomb_id` give bombs a STABLE identity across
the tick boundary (our `bombs` vector compacts dead entries every tick,
which would invalidate a raw index held from one tick to the next — the
original's slot-reuse array has an analogous, unreplicated hazard: a stale
queue entry whose slot got reused within the same 1-tick gap would
force-detonate the WRONG bomb; unreachable in our architecture since we
never reuse an id). All four call sites (`FlameSystem::spread_to`'s bomb-hit
branch, `BombSystem::fly`'s landing-on-flame check, `BombSystem::
detonate_triggered`, `BombSystem::slide`'s flame-entry check) now push
through the queue instead of calling `explode()` directly. The drain
processes queued entries in ASCENDING BOMB-VECTOR-INDEX order (not push
order) — not bit-identical to the original's slot-index order (our vector
has no slot reuse, so "vector position" is simply creation order among
currently-active bombs), but the same idea: a well-defined, deterministic
order for any RNG draws a chained explosion's arm makes (e.g. a `scatter()`
on a Disease tile).

**The bug this corrects:** our previous port called `FlameSystem::explode`
directly from all four sites — a chain reaction cascaded fully within a
single tick (recursively, through the whole call stack), a chain-triggered
bomb always did a full undirected 4-way blast, and a chained bomb kept its
ORIGINAL owner instead of the triggering bomb's. `BombSystem::detonate_
triggered`'s and `EnclosureSystem::drop_wall`'s synchronous calls were
ALREADY flagged as known simplifications in "Options toggles" (2026-07-08,
"queues... drained as a real explosion") but the one-tick-defer nuance
itself was not yet investigated at that time. `detonate_triggered` is fixed
here (same tick either way, so no observable timing change — see above);
`EnclosureSystem::drop_wall`'s parallel `sub_423209(bomb, -1)` call
(pseudo.c 27262, the "stomped bombs detonate" wall-crush case) is a
DIFFERENT system with its own facts.md section and was not touched by this
pass — flagged as a follow-up.

Ported: `libs/sim/src/systems/flames.{hpp,cpp}` (`explode` skip_dir param,
`queue_chain`, `drain_chain_queue`), `libs/sim/src/systems/bombs.cpp`
(`detonate_triggered`, `fly`, `slide`), `libs/sim/src/simulation.cpp` (new
step 1b, right after the player pass), `libs/sim/include/bomber/sim/
{state.hpp,bomb.hpp}` (`State::pending_chain`/`next_bomb_id`,
`Bomb::id`), `libs/sim/src/hash.cpp` (all three newly hashed). Tests:
`tests/test_sim.cpp` ("flame arm stops at a bomb it chain-detonates..." —
rewritten to check the intermediate one-tick state; "a chain-detonated bomb
skips re-blasting back toward its trigger"), `tests/test_kick_nuances.cpp`
("a jelly bomb sliding into flame bounces, but still chain-detonates"),
`tests/test_stage_actors.cpp` (flying-bomb-warphole, above).
`tests/test_dud.cpp` and the renamed `tests/test_sim.cpp` "chained bombs
explode within a tick of the trigger, not on bomb B's own fuse" already had
enough slack in their numeric assertions to pass unchanged — only their
names/comments (which had claimed "instantly"/"same tick") needed
correcting for honesty.

**GOLDEN IMPACT: real, reaching every scenario with active bomb play (B, C,
D, E); golden A (0 players, 0 bombs) is layout-only.** Proved: the full
suite's every NON-hash assertion — golden A's final `rng`, golden D's
`kExpectedRng` at all four checkpoints, golden E's bounce count (10,
unchanged) and final `rng` — is BYTE-IDENTICAL before and after this change
(7 of 24 golden-file assertions; the other 17 are exactly the hash checks
that moved). This proves the fix draws no new RNG anywhere: it only changes
WHEN a chain-queued bomb actually detonates and WHICH direction it skips,
never the random stream. Recaptured all five golden hash constants (`tests/
test_golden.cpp`) plus two supporting hash-layout-growth fields
(`Bomb::id`, `State::next_bomb_id`, `State::pending_chain`).

(Provenance: `sub_423209` pseudo.c 25195-25204; drain guard 25330-25346;
arm-hit push 25637-25651; flying-landing-on-flame push 25459-25465;
`sub_424B41` trigger-button push 26027-26067; slide-into-flame push
25540-25554; bomb+56 consumption 25617-25621; owner-transfer 25644
(hit bomb's +62 ← exploding bomb's +62, distinct from the flame-CELL struct's own +62 field,
which `sub_426FCC` pseudo.c 27479-27504 sets from an unrelated bomb+60
upper-half value we did not chase further — looks like a rendering/tint
detail, not re-examined here); `EnclosureSystem::drop_wall`'s parallel,
untouched `sub_423209(bomb,-1)` call at pseudo.c 27262.)

## Bomb capacity is a derived live-bomb count — CONFIRMED (`sub_4245DA`, 2026-07-11)

Read 2026-07-11, root-causing a live-play report ("I had 4 extra bombs and
suddenly dropped to a single bomb", while alive; a later, more precise repro
from the same user: it happened right as a diarrhea bout ended). The original
keeps **no per-player bomb counter**:

- **`sub_4245DA(player_idx)`** (pseudo.c 25795-25812) scans all 100 bomb
  slots and counts the ACTIVE ones whose owner word — bomb offset **+62**,
  read as the UPPER half of the dword at +60 (the dword shifted right 16) —
  equals the player. That count is the
  player's current "bombs out".
- **Both placement gates recompute it live** (`sub_41F29B`): the plain drop
  branch calls `sub_4245DA(idx)` and proceeds only while `player[+86]` (max
  bombs) is strictly greater than that live count (~23345), and the
  spooger loop re-calls it EVERY laid bomb (its loop condition stops as soon
  as `player[+86] <= sub_4245DA(idx)`, ~23336).
- **The explosion frees no counter** — `sub_42331C`'s detonation block just
  clears the slot (writes 0 into the bomb's +0 state dword, ~25616); the
  derived count drops by itself.
- **Consequence for chains:** the chain ownership transfer
  (hit bomb's +62 ← exploding bomb's +62, pseudo.c 25644 — see
  "Chain-reaction timing")
  rewrites the very word `sub_4245DA` matches on. So chaining someone else's
  bomb MOVES THE PLACEMENT SLOT along with kill credit: the victim's
  capacity frees IMMEDIATELY at transfer time (one tick before the chained
  bomb even explodes), and the chained bomb counts against the CHAINER's
  capacity until it goes off.

**The bug this corrects (port-only, not in the original):** our
`Player::bombs_placed` is a running counter (`++` at placement, `--` at
explosion), and `FlameSystem::explode` decremented `bombs[i].owner` — which,
after a chain transfer, is the CHAINER. Every cross-owner chained bomb
therefore leaked one placement slot from the victim FOREVER (and decremented
the chainer's counter, clamped at 0). Worst case is exactly the reported
flow: diarrhea/super auto-drop poops the whole `max_bombs` capacity onto the
field as one adjacent cluster; one enemy blast chains the first bomb, and
each transferred link (now enemy-owned) chains the next — the victim's
counter sticks at ~max_bombs for the rest of the match, reading as "my
bombs got reset when the disease ended" even though `max_bombs` was never
touched. Refuted along the way, by direct audit: the head hit cannot drop
more than getvalue(670)+rand%getvalue(671) = 1..3 kinds per hit (shipped
VALUELST: `670,1`/`671,3`); one flight cannot multi-hit without physically
re-crossing the victim (pseudo.c 25445-25448 has no per-flight latch — the
bomb just advances and keeps flying, same as our re-hop); the disease
expiry/cure path (`sub_41DF4C` semantics / our `DiseaseSystem::clear`) only
zeroes the disease fields — short-flame/short-fuse are computed at DROP time
from the live flags, nothing is "restored"; and `reset_to_baseline` has
exactly one caller (`death_scatter`), gated on death at all three sites.

**Ported** in `FlameSystem::spread_to`'s bomb-hit branch: the slot moves with
the owner word (`--old.bombs_placed` (clamped, defensive) /
`++new.bombs_placed`, only when the owners differ — the original's
unconditional word write is a no-op for a self-chain). `bombs_placed` remains
a stored, hashed counter; with the transfer ported it tracks `sub_4245DA`'s
derived value exactly at every tick boundary. Tests:
`tests/test_chain_slot.cpp` (transfer timing — freed at transfer, charged to
the chainer until explosion; the 4-bomb 5→1 collapse repro; the full
diarrhea-cluster → enemy chain → recovery flow; a self-chain control) and
`tests/test_head_hit_bounds.cpp` (the refuted-hypothesis bounds, pinned).

**GOLDEN IMPACT: none — proven, zero recapture.** The fix's ONLY behavioural
delta is inside `if (hit->owner != owner)`; `bombs_placed` is hashed, and a
transfer permanently changes the victim's counter (and placement behaviour)
from that tick on — so if any golden scenario ever chained across owners,
its downstream checkpoint hashes would move. All five golden scenarios pass
BYTE-IDENTICAL with the fix in place (full suite run, all 24 assertions):
no golden ever performs a cross-owner chain, and the new branch is inert
there.

(Provenance: `sub_4245DA` pseudo.c 25795-25812; drop gate ~23345 and spooge
loop gate ~23336 in `sub_41F29B`; slot clear (bomb +0 ← 0) ~25616 and
owner transfer 25644 in `sub_42331C`; head-hit flight fall-through
25445-25448; shipped `VALUELST.RES` ids 670/671 verified from the install.)

## Per-tick call order — END-TO-END (`sub_42A191`, 2026-07-11)

The definitive reconstruction of the original's complete per-frame call
sequence, read top to bottom from the per-frame tick callback `sub_42A191`
(pseudo.c 29488-29557; registered via `sub_43A6FC` at 29701) and every
gameplay callee, and diffed against `Simulation::tick`. Motivated by the
observation that every recent in-game deviation was an INTERACTION bug, not a
per-system arithmetic error — this pins the interaction ORDER itself.

### The frame sequence (every callee, in order, with line citations)

1. 29506-29510 — frame delta `dword_464958` = elapsed ms, clamped to
   `getvalue(31)`. At the locked 20 Hz rate it equals `dword_46494C`
   (= 1000/getvalue(30) = 50 ms) every frame.
2. 29516 `sub_40E765` — network receive pump (12922-13008; remote-input
   dispatch through `funcs_40E9D7`). No local-game state.
3. 29517 `++dword_464994` — the FRAME STAMP. Everything "once per frame" in
   the engine gates on it: the chain-queue drain (25331), the Goldman
   sparkle ager (23611), the tile-regen re-arm window (27966-27971), the
   hurry-text flash (29546). At locked 20 Hz, per-FRAME == per-TICK; there is
   no frame-gated gameplay logic that our tick-only sim needs to subdivide.
4. 29518 `sub_4105D2` — MATCH CLOCK update (14456-14530: elapsed
   `dword_4601B8 += delta` when running; remaining-seconds recompute;
   `dword_4601A8 == 1001` sudden-death ids 110-112 block).
5. 29519 `sub_415CA4` (18132-18139, backdrop restore memcpy), 29520
   `sub_42641F` (26988-26991 → `sub_415DD9`, clip-rect reset) — draw only.
6. 29521 `sub_4056CA` — stage-actor SPRITES (7193-7324: dirarrow/warphole/
   conveyor/trampoline anim + blit). Draw only, EXCEPT a one-shot per-level
   init on each warphole's first frame (7247-7269: `+146` latch — blanks the
   warphole tile and one RANDOM in-bounds neighbour, `rand_() % 4` re-rolled
   until in-grid; setup-time board mutation, not per-tick logic).
7. 29522 `sub_4245B9` → `sub_42331C(0)` — the BOMB PASS (grounded+flying,
   i.e. every record whose carrier ptr +148 is 0 — gate at 25352):
   a. 25331-25346 — chain-queue DRAIN, once per frame (`dword_462210 !=
      dword_464994`): each queued bomb gets `+68 = +74` (fuse forced
      elapsed) and its skip-direction byte `+56`; the queue is emptied.
      Drains ALL entries, but only STAMPS — the explosions happen as each
      bomb's own slot is reached below.
   b. 25350-25742 — the 100-slot loop, PER SLOT, in slot order: anim
      counter (25356); motion switch on `+46` when the record is a bomb
      (`+16 == 9`, 25358): case 0 resting/conveyor start (25362+), case 1
      slide (25517-25583: per-pixel — dirarrow re-steer 25525-25537,
      slide-into-flame queue 25545-25554, kick-stop consume 25557,
      cell-entry probe `sub_4230A5` 25555), case 2 fly (jelly veer, landing
      verdict 25443, head-hit 25445-25448, land-on-flame queue 25459-25465),
      case 3 carried (25480-25512, position sync — only reached in the
      mode-1 pass); THEN dud transition (25588-25600, getvalue(323)); THEN
      the fuse TICK (25605-25612, gated: not fizzling, not flying, not
      carried, not trigger-kind); THEN the fuse CHECK `+68 >= +74` →
      EXPLOSION (25613-25681: slot clear 25615, sound 200, epicentre ignite
      + powerup burn 25623-25636, the 4-direction arm walk 25637-25678 with
      chain-queue/owner-transfer 25641-25651, powerup-stop 25653-25661,
      solid-stop 25662-25664, brick ignite + reveal 25665-25672, blank
      ignite 25673-25677); THEN the AI danger-grid stamp (25683-25705,
      `sub_424DFE` with live remaining-fuse) and the draw (25734).
      NOTE: motion and fuse/explosion are INTERLEAVED PER SLOT — slot 3's
      slide happens after slot 1's explosion within the same frame.
8. 29523 `sub_424F89` — powerup token DRAW (26218-26267; blank-cell gate).
9. 29524 `sub_41B961` (20793 → 20775-20789, sprite-list update) — draw.
10. 29525 `sub_426D06` — FLAME/BRICK-BURN AGING + draw (27366-27463): flame
    cells age and expire; a brick-burn reaching its end flips the cell open.
    A flame lit by THIS frame's bomb pass ages once the same frame.
11. 29526 `sub_426818` — ENCLOSURE (27140-27298): match-active gate, arm on
    `remaining <= getvalue(101) - 5`, TILE REGEN `sub_426704` inside
    (before the wall stepper), then the 250 ms-cadence wall drops
    (crush/stomp — see the enclosure audit entry).
12. 29527 `sub_420F07` — the PLAYER PASS (23628-23724): slots 0..9
    ascending, `sub_41F29B` per player, each player's FULL turn completing
    before the next slot (in-place cross-player effects are slot-ordered).
    Within ONE player's turn (sub_41F29B):
    a. 22857-22891 — absent-slot skip; respawn/re-entry bookkeeping.
    b. 22909-22914 — entering-countdown (word +102) decrement.
    c. 22915-22917 — FLAME DEATH at the CURRENT (pre-move) tile:
       `sub_42708D` + the shared kill funnel `sub_41DE63` (which early-outs
       for bounce/warp states 5/6/7). A kill diverts the rest of the turn
       to the death-anim branch (23457+).
    d. 22919-22926 — PICKUP at the CURRENT tile (`sub_42542D` state 2 →
       `sub_41E21E` dispatch + `sub_4254F3` record clear). NOT gated on
       bounce/warp — a mid-hop player still hoovers the token under it.
    e. 22927-22928 — disease freshness (+128) decrement.
    f. 22929-22942 — disease age += delta; expiry → `sub_41DF4C` cure.
    g. 22943-22975 — CONTAGION scan (in-place, all 10 slots, both lower and
       higher indices; `dword_464A78` multiply semantics).
    h. 22976-22979 — key-byte shuffle (+54=+56, +55=+57; +56=+57=0).
    i. 22981-22990 — STUN (+58) decrement; clears the new-input gate only.
    j. 22991-23014 — fully-enclosed check → cosmetic "cornerhead" anim pick
       (`rand_() % getvalue(330) + 20` — a shared-stream draw our sim
       deliberately does not mirror, determinism rule 6).
    k. 23015-23027 — state gates: 5/6/7 (bounce/warp) clear the new-input
       gate; state 4
       (pickup pose) FORCES the bomb key held (+56=1) for getvalue(665) ms
       — this, not the +58 stun, is what keeps a just-grabbed bomb carried.
    l. 23028-23039 — INPUT acquisition (gated on the new-input flag still
       being set AND `dword_4621E0` being 0):
       AI `sub_40A1C6` or human `sub_41E61E`.
    m. 23040-23057 — godir clamp; REVERSED-disease flip (+140, humans).
    n. 23058-23078 — ICE input-lag buffer (humans).
    o. 23413-23454 — MOVEMENT dispatch: idle-on-conveyor (belt budget,
       23415-23428) or walking (speed calc 23432-23451); both call the
       per-pixel mover `sub_41EC84` (22525-22722). INSIDE the pixel loop,
       per pixel step: warphole/trampoline step-on (22583-22609, pre-
       commit), kick probe (22610-22628), corner/glide resolution, pixel
       commit (22695-22698), then POST-COMMIT FLAME DEATH (22699-22708 —
       `sub_42708D` + `sub_41DE63`; a kill returns 1 immediately,
       abandoning the remaining budget) and PICKUP (22710-22717 —
       `sub_42542D` state 2 → `sub_41E21E`). So a walking player dies or
       picks up mid-move, per pixel, BEFORE the same tick's bomb actions —
       and a fast player can consume several tokens in one tick.
    p. 23081-23276 (the anim/draw block) — anim/draw; state 5 trampoline apex
       relocation (rand draws), state 6/7 warp midpoint relocation.
    q. 23277-23380 (the bomb-action block) — BOMB ACTIONS, in order:
       auto-drop force (the auto_drop flag, +135/+137) → carried THROW (+37;
       fires on auto_drop OR key-up) →
       action2 edge (+57 && !+55): kick-stop +89, punch +91 (needs !+56),
       trigger-detonate +95 → drop block (+56 edge, !+134): grab (own bomb
       underfoot) / spooger (own, auto_drop clear) / plain drop (capacity =
       `sub_4245DA` live scan; warphole refuse; dud gate).
       A player killed mid-move at (o) NEVER reaches this block.
    r. DEATH branch 23457-23496 — die-anim advance; at anim end (the
       die-anim-complete branch, ~23474-23478)
       `sub_41DBFE` scatter + slot clear + `sub_424C47` trigger downgrade.
13. 29528-29529 — campaign only (`dword_46489C`): `sub_4016DA` (4613-4651)
    — the ROVER/GHOST MOVER `sub_401F76` FIRST (4619), then round-end
    timers (clock ≤ 1, hazard-clear grace `dword_4646C0`).
14. 29530 `sub_42459A` → `sub_42331C(1)` — the CARRIED-bomb pass (records
    with +148 != 0): case 3 position sync to the (post-move) carrier, draw.
    The fuse-tick gate excludes motion 3, so no fuse burns here.
15. 29531-29549 — HURRY banner check (`sub_410578` vs `getvalue(101)`,
    latch `dword_464984`, sound 2700, flash on `dword_464994 & 4`).
16. 29550-29555 — `sub_429F1A` (easter-egg overlay), `sub_415ED1`
    (draw-queue flush), `sub_40E765` again, `sub_41B961`, `sub_415C1F`,
    `sub_40EA1E` (network send flush). No gameplay.

Round-END evaluation (`sub_421969`/`sub_4219B0`, draw banner, winner) lives
in the OUTER loop `sub_42A3F6` (its round-end block, 29790-29830), not in the per-frame
callback — our GameApp layer equivalent, not a sim tick step.

### The rotation: our tick vs the original's frame

`Simulation::tick` starts at the player pass; the original's frame starts at
the clock/bomb pass. The two are the SAME infinite event stream cut at
different points — what matters is the relative order of gameplay phases
between two consecutive player passes (one "gap"). Original gap order,
players → players (from the sequence above):

```
players_N → rovers_N → carried-sync_N → (hurry) ‖frame boundary‖ clock →
bomb pass (drain → per-slot move+fuse+explode) → flame/brick age →
regen → enclosure walls → players_{N+1} head (flame death → pickup →
disease fresh/age/expire → contagion) → players_{N+1} input+move+actions
```

Fuse alignment across the cut: our port decrements a new bomb's fuse the
same tick it is dropped (players step 1 → fuses step later the same tick);
the original's first decrement is the NEXT frame's bomb pass. Both place the
40th decrement — the explosion — in the SAME gap (after the 39th post-drop
player move, before the 40th), so fuse timing is rotation-identical, as the
golden suite has always pinned.

### Diff vs `Simulation::tick` — findings

DIVERGENT (fixed 2026-07-11, golden recaptured, this entry):

1. **Flame-death/pickup ran only as a post-batch pass, never inside the
   mover.** The original checks BOTH per pixel step inside `sub_41EC84`
   (22699-22717) AND at the next turn's head (22915-22926). Our sim only
   had the head-equivalent (`field_vs_players`, after the bomb phase).
   Player-visible consequences of the missing in-move check, all fixed by
   running the same flame-then-pickup pair per pixel inside
   `MovementSystem::move`:
   - a player stepping onto a visible token picked it up only AFTER the
     same tick's explosions — so a flame arm igniting that tile the same
     tick BURNED the token first and STOPPED there (arm-stop rule),
     leaving the player alive and empty-handed; the original picks up at
     the step, the arm then finds no token, passes through, ignites the
     tile and kills — opposite outcomes on both counts;
   - a picked-up ability was not usable until the next tick (the original
     picks up mid-move, BEFORE the same turn's bomb-action block);
   - a player walking into a flame on its LAST tick of life survived (the
     head check runs after the aging pass; the in-move check sees the
     pre-aging value);
   - a player killed mid-move still executed its bomb actions that tick
     (the original's mid-move kill returns straight into the death branch,
     skipping the bomb-action block).
2. **Enclosure/regen/clock ran AFTER the head checks** (our old step 6 vs
   step 5). The original runs clock → … → regen → walls BEFORE the player
   pass (29518/29526 before 29527), i.e. before the head-equivalent
   checks in the gap. Same-gap coincidences diverged: a wall dropping on a
   player standing on flame credited the flame owner (ours) instead of
   crushing with no credit (original); a wall dropping on a token tile
   under a player let the pickup win (ours) instead of the wall destroying
   the token (original). Fixed: clock/regen/enclosure moved before
   `field_vs_players`.
3. **Rovers ran after the bomb phase** (our old step 5b). The original
   moves them immediately AFTER the player pass (29528), BEFORE the next
   frame's bomb pass — so a rover never walks into a flame lit later in
   the same gap. Fixed: `rovers.tick()` moved to directly after the player
   loop.
4. **Flame death ignored the bounce/warp immunity.** The head check goes
   through `sub_41DE63` (22917), which early-outs for states 5/6/7 — the
   SAME guard already ported for the wall crush (enclosure audit #4). Our
   `field_vs_players` killed a mid-hop/mid-warp player standing over
   flame; so did the rover landing kill (also `sub_41DE63`, campaign.md
   clause 4). Both now exempt `bounce/warp`, matching `drop_wall`.

ORDER-EQUIVALENT (verified, no change):

- Trigger-press/chain/wall-stomp queue timing (drain slot in the rotation —
  see the corrected "Chain-reaction timing" note above).
- Stun, key shuffle, input, reversed-disease, ice, movement dispatch,
  bomb-action sub-block order inside the player turn (all previously audited;
  re-verified against the full read).
- Diseases: head pickup before disease aging before contagion; our step
  order preserves the per-player age-then-spread relation (the single-sweep
  cross-player quirk stays a documented deviation, see the disease audit).
- Flame aging in the same gap as the batch, after it (29525 after 29522).
- AI decide slot (interleaved per player, ADR-0005) and the danger-grid
  snapshot (original stamps it in the bomb pass; both sides of the rotation
  read the same post-batch state).
- Bomb-capacity reads (`sub_4245DA` live scan): the chain owner transfer
  happens in the bomb phase in both (between two player passes), so the
  drop gate reads the same value (see "Bomb capacity" entry).

ACCEPTED DEVIATIONS (documented, deliberately not replicated):

- **Within-batch slot interleave.** The original interleaves motion and
  fuse/explosion PER SLOT (finding 7b above): whether a sliding bomb sees a
  same-frame explosion's flames depends on the two bombs' relative slot
  indices — and slots are allocated first-fit with reuse, so the order is
  not even creation order. Our phase split (all moves, then all fuses)
  makes the same-frame case uniformly "slider first"; the flame-slide
  interaction then lands one tick later than an original whose exploder
  happened to sit on a lower slot. Matching this exactly would require
  porting the 100-slot allocator; the divergence is confined to sub-tick
  bomb-vs-bomb coincidences within one gap. Same spirit as the contagion
  single-sweep note.
- **Bomb actions during bounce/warp.** The original's state-5 anim block
  jumps to the bomb-action block (23198) and states 6/7 fall through to it,
  so a bouncing/warping player still auto-drops (diarrhea/super raise the
  auto_drop flag) and
  still auto-throws a carried bomb (key bytes zeroed → `!+56`). Our
  player_turn early-returns for both states, skipping the action block —
  narrow (disease auto-drop or a carried bomb + trampoline/warp), deferred
  with this citation; the analogous stun-throw edge is already documented
  as deferred in player_turn's comment.
- **Cornerhead anim pick** (22991-23014): cosmetic shared-stream `rand_()`
  draw for a fully-enclosed player's taunt animation — presentation-side
  by determinism rule 6; our sim draws nothing.
- **Carried-bomb pass** (`sub_42459A`): our carried bomb is fields on the
  carrier (no separate entity to position-sync); the mode-1 pass has no
  other gameplay effect (fuse gate excludes motion 3).

(Provenance: `sub_42A191` 29488-29557; `sub_420F07` 23628-23724;
`sub_41F29B` 22740-23497; `sub_41EC84` 22525-22722; `sub_42331C`
25330-25743; `sub_4245B9` 25788-25792; `sub_42459A` 25782-25786;
`sub_4016DA` 4613-4651; `sub_4056CA` 7193-7324; `sub_4105D2` 14456-14530;
`sub_415CA4` 18132-18139; `sub_42641F` 26988-26991; `sub_41B961`
20775-20796; `sub_429F1A` 29344-29374; outer loop `sub_42A3F6`
29610-29830.)

## Brick crumble timing — CONFIRMED (`sub_425EFC`/`sub_425107`/`sub_426D06`, 2026-07-10 flame-system audit)

A brick hit by flame does NOT open up immediately — it stays fully solid
(blocking movement, bombs, and later flame arms) for the entire crumble
animation, and only becomes passable when that timer expires. Its hidden
powerup, if any, reveals far EARLIER than that — immediately at ignition.
These two facts are independent of each other and were BOTH backwards in our
previous port.

**The cell-type grid is untouched at ignition.** The arm-loop's brick branch
(pseudo.c 25666-25671) calls `sub_425EFC(x, y, 0)` then `sub_425107(x, y)`.
`sub_425EFC` (pseudo.c 26826-26846) does exactly four things, in this order,
on the `(x, y)` it is given and the new cell type passed as its third
argument (0 = blank at this call site):

1. **Save** the CURRENT cell type via `sub_425FB9(x, y)` (2 = brick here).
2. **Durably set** the cell type to the argument (0 = blank) via
   `sub_425E36(x, y, newtype)`.
3. **Redraw/bookkeep**: `sub_4151AD()`, then `sub_425D22(x, y)`, then
   `sub_415189()`.
4. **Durably set it BACK** to the saved type (brick, 2) via
   `sub_425E36(x, y, saved)` — this is also the function's return value.

`sub_425E36` writes straight into `dword_46222C` — the SAME array
`sub_425FB9` (our `s.cells`) reads — with no indirection, so this is a real
write, not a redraw-queue push. The net effect of the whole call is a
no-op: the cell reads brick before, is briefly blank ONLY while the redraw
calls run, and is brick again the instant `sub_425EFC` returns. The tile is
NOT open at this point, for anyone.

**The cell only actually opens up later, in the per-tick flame-cell
animator** `sub_426D06` (pseudo.c 27391-27462), which drives BOTH the
regular flame's 10-frame lifetime (already-confirmed, unchanged) and the
brick-burn cell's crumble. The brick-burn path is selected when the flame
cell's KIND dword at **+4** equals 9, and inside it:

- once the cell's elapsed-ticks counter exceeds `getvalue(20)`
  (`brick_burn_frames`, id 20 = 10 shipped):
  - **if** `sub_425FB9(col, row)` still reads 2 (STILL brick — a defensive
    re-check), call `sub_425E9B(col, row, 0)` to NOW durably clear it to
    blank;
  - then write −1 into the flame cell's **+0** state dword — purely a
    same-call sprite-wind-down marker.

`sub_425E9B` is the same "set + redraw" helper WITHOUT a revert — this call
is the durable one. Between ignition and this point (`brick_burn_frames`
ticks later — id 20, same 10-frame shipped value as the regular flame's id
10), the cell reads brick continuously: it blocks movement/placement (same
as a fresh, unburnt brick) AND, if a SEPARATE flame arm reaches it in that
window, that arm hits the ordinary brick branch again — the crumble timer
simply resets, exactly matching `sub_426FCC`'s unconditional reinit
(`+68=0`) on every ignite call, fresh or repeat.

**The hidden powerup reveals immediately, at ignition — not when the tile
opens.** `sub_425107(x, y)` (pseudo.c 26274-26343) runs right after
`sub_425EFC`, in the SAME ignition call. Its tail (the "reveal tail" this
file cites throughout the relocation entry below) is unconditional, and on
the token record found at that tile it does exactly this:

- **if** the record's state field (**+0**) reads 1 (hidden, i.e. still under
  a standing brick):
  - stamp the record's **+64** dword (element 16 of its dword view) with the
    current frame stamp `dword_464994` — the reveal tick, presentation-only;
  - flip the state field to **2** (VISIBLE).

So the token starts being rendered as soon as the brick catches fire — well
before a player could possibly reach it (the tile is still fully blocking,
per the cell-type finding above) — rather than popping in only once the
brick is fully gone.

**`sub_425107`'s earlier gated branch (the one whose comparand Hex-Rays
flagged "possibly undefined")
is now RESOLVED — see "Overpowered-powerup relocation" below.**

**The bug this corrects:** `FlameSystem::spread_to`'s brick branch used to
flip `s.cells[ty][tx]` to `Cell::Blank` immediately (letting movement/bombs/
later flame arms through right away, and letting a second flame hit
during the crumble skip past it via the unrelated `burning > 0` early-return
instead of correctly re-triggering the brick branch), while
`age_flames_and_bricks` revealed the hidden powerup only once `burning`
finished — both timings backwards relative to the original.

Fixed: `FlameSystem::spread_to`'s brick branch no longer touches
`s.cells`; it only (re)sets `s.burning[ty][tx]` to a fresh
`brick_burn_frames` and reveals `s.hidden[ty][tx]` into `s.floor[ty][tx]`
right there. `FlameSystem::age_flames_and_bricks` is now the ONLY place
`s.cells[ty][tx]` flips Brick → Blank, guarded on it still reading Brick
(mirroring `sub_425FB9(...)==2`), with the (now redundant) powerup-reveal
code removed from there. The player-movement-blocking CONSEQUENCE of the
premature flip was already masked in practice by `grid::tile_open`'s
separate `burning > 0` check (which already refused movement onto a
crumbling tile regardless of `s.cells`), so this fix's only NEW observable
effects are: (a) a second flame arm reaching a still-crumbling brick now
correctly re-triggers the brick branch (resets the timer) instead of
silently no-op'ing via the `burning > 0` early-return (which is now
removed — dead code once the cell-type ordering is correct, since
`burning > 0` can only be true while `s.cells` still reads Brick, so the
brick branch above always catches it first), and (b) the powerup reveal
timing, which is genuinely earlier and player-visible (the token fades in
over the still-burning brick instead of popping in when it's gone).

Ported: `libs/sim/src/systems/flames.cpp` (`spread_to`'s brick branch,
`age_flames_and_bricks`). Tests: `tests/test_sim.cpp` ("bomb explodes at
its fuse and burns the brick" — extended to assert the cell stays Brick
through the crumble and only opens after `brick_burn_frames` more ticks;
"burned brick reveals its powerup, players pick it up" — extended to assert
the powerup is visible in `s.floor` immediately after ignition, while the
tile is still `Cell::Brick`).

**GOLDEN IMPACT:** folded into the "Chain-reaction timing" entry's combined
proof above (both fixes landed in the same commit and were verified
together) — golden D (the only scenario with floor/hidden powerups under
bricks in meaningful quantity) is among the recaptured hashes; its RNG
stream (`kExpectedRng`) is unaffected, confirming this fix draws no RNG
either.

(Provenance: `sub_425EFC` pseudo.c 26826-26846; `sub_425E36` 26782-26797;
`sub_425E9B` 26802-26820; `sub_425107` 26274-26343; `sub_426D06`
27365-27463 (brick-kind branch 27404-27424); `sub_426FCC` 27479-27504;
arm-loop brick branch 25662-25678; VALUELST ids 10/20 both = 10 shipped,
`docs/valuelst-map.md`.)

## Overpowered-powerup relocation — CONFIRMED (`sub_425107`'s early gated branch, 2026-07-10 follow-up)

Resolves the "Open question, NOT resolved, NOT ported" flag left by the
"Brick crumble timing" entry above: `sub_425107` (the brick-ignite reveal
helper) has an EARLIER branch, before its unconditional reveal tail (the
"reveal tail" this file refers to below, at pseudo.c ~26336), that can
relocate a hidden token instead of letting it show. Its guards, in the order
the function evaluates them — any one failing falls straight through to the
reveal tail:

1. `sub_40C06A()` must return 0 — a local (non-networked) game.
2. Call `sub_4105B0()` (elapsed seconds, see below) and `sub_412135(102)`
   (= `getvalue(102)`); the elapsed value must be **strictly less** than
   `getvalue(102)`.
3. Read the token record's KIND (the record's **+4** field — its kind, NOT
   its state) and require it to be **5, 6 or 11**.

Only past all three does the relocation itself run: pass 1 is a 200-try swap
search, and only if that exhausts, pass 2 is a 200-try empty-brick move
search (both detailed below).

**The "possibly undefined" elapsed value — RESOLVED by direct disassembly,
same technique as `exp_` above.** Hex-Rays could not prove where the left
operand of guard 2's comparison got its value (it emits a "variable is
possibly undefined" note against pseudo.c line 4251A4). Raw disassembly of
`0x425184-0x4251A4` (capstone, `BM95.EXE` imagebase `0x400000`) settles it —
the eight instructions, in order:

| addr | what happens |
|---|---|
| 425184 | call `sub_40C06A` → the network role `dword_460058` |
| 425189 / 42518b | test the result; if NON-zero (networked) jump to 42535e, i.e. skip straight to the reveal tail |
| 425191 | call `sub_4105B0` → `dword_4601BC` |
| 425196 | **copy that return value into `edx`** — this is the comparison's left operand, assigned explicitly right here |
| 425198 / 42519d | load 102 into `eax` and call `sub_412135` (= `getvalue(102)`) |
| 4251a2 | compare `edx` (elapsed) against `eax` (the threshold) |
| 4251a4 | if elapsed `>=` threshold, jump to 42535e — no relocation, straight to the reveal tail |

The register copy at 425196, immediately after the call at 425191, is plain
and unambiguous — the comparand IS `sub_4105B0()`'s return value, full stop.
Hex-Rays' "possibly undefined" is a dataflow-tracking miss (likely because
Hex-Rays doesn't know `sub_4105B0`'s true signature crosses the call boundary
cleanly), not a sign of dead/garbage/uninitialized data — the exact same
false-alarm shape as `exp_`.

**What `sub_4105B0()`/`dword_4601BC` and `getvalue(102)` actually are.**
`sub_4105B0` (pseudo.c 14448-14453) is a one-line accessor: `return
dword_4601BC;`. That field is written by `sub_4105D2` (pseudo.c 14455-14552,
the in-round MM:SS clock's own per-tick updater, called once per game tick
from `sub_42A191` — `docs/valuelst-map.md` ids 110-112) as `dword_4601A8 -
<remaining seconds>`, i.e. **elapsed seconds since the round timer started**
(`dword_4601A8` is the round's total length in seconds; the subtrahend is
`dword_4601B0` when hosting a network game, or a fresh wall-clock read via
`sub_43ACF8()` otherwise — either way the complement of "seconds left", so
their difference is monotonically-increasing elapsed time). `sub_40C06A()`
(pseudo.c 11138-11142) returns `dword_460058`, the local/host/guest network
role flag (0 = not networked, 1 = host, 2 = guest — set by `sub_40C035`,
read the same way by `sub_4105D2`'s own host/guest split). `getvalue(102)`
is the ordinary VALUELST lookup (`sub_412135` = `getvalue`,
`docs/formats/valuelst.md`); the SHIPPED `DATA/RES/VALUELST.RES` line is:

```
; the period of time when "over-powerful" powers won't appear (will go elsewhere)
102,40                                              ; PGT
```

This is the smoking gun that independently confirms every part of the
disassembly reading: id 102 is a SECONDS value (40, i.e. 800 ticks at the
20 Hz nominal rate), the direction is "won't appear" for the opening period
(elapsed < threshold), not late-game as `docs/valuelst-map.md` previously
guessed ("102 = late-game powerup gating, still unpinned" — backwards; now
corrected), and the file's own author already calls kinds 5/6/11
"over-powerful". So the REAL, fully-resolved condition is: **while
`elapsed_seconds_since_round_start < getvalue(102)` (40s shipped) AND the
game is not networked**, a hidden token of kind 5, 6, or 11 relocates
instead of revealing when its brick ignites.

**Which kinds, and why.** `off_45BE50` (pseudo.c 2261-2281, the powerup
name table `sub_424F89`/`sub_425107` both index by kind) is `{0:"bomb",
1:"flame", 2:"disease", 3:"kicker", 4:"skate", 5:"punch", 6:"grab",
7:"spooge", 8:"goldflame", 9:"trigger", 10:"jelly", 11:"disease3",
12:"random", ...}` — a 1:1, same-order match with `PowerupType`
(`types.hpp`: `ExtraBomb, Flame, Disease, Kick, Skate, Punch, Grab, Spooger,
Goldflame, Trigger, Jelly, SuperDisease, Random`). So kinds 5/6/11 are
**Punch, Grab, and SuperDisease** — NOT the base Disease (kind 2, unaffected)
— matching the VALUELST comment's "over-powerful" framing: Punch and Grab
let a player punch/throw ANY bomb (including opponents'), and SuperDisease
is a nastier disease variant; all three are plausible "don't let someone
grab this in the opening seconds" candidates.

**This is a DIFFERENT mechanism from `sub_4255B2`/`PowerupSystem::scatter`**
(the "Options toggles" `diseases_destroyable` relocation). `sub_4255B2`
scatters a NEW floor powerup onto a random EMPTY WALKABLE tile (cell type
0/blank); it is never called from `sub_425107`, and `sub_425107`'s own
search loops below operate on STILL-STANDING BRICK tiles (cell type 2) and
directly swap/move the underlying hidden-token RECORDS — a structurally
different operation with its own 200-try (not 100-inner/100-outer) budget
shape. The two share only the surface-level "random-tile retry loop"
pattern.

**The two search passes** (pseudo.c 26304-26334), both drawing `x =
rand()%W` then `y = rand()%H` — 2 draws EVERY iteration, even when the
candidate is rejected (the draws sit before the guard/checks, so a miss
still burns its budget; unlike `sub_4255B2`'s inner/outer split, there is no
"free re-roll" case here):

1. Up to 200 tries: accept the first still-standing-brick tile
   (`sub_425FB9(x,y)==2`) holding ANY record at all (`sub_42542D(x,y)` —
   hidden OR already-visible-but-still-crumbling; the state field isn't
   checked) whose kind is NOT ALSO 5/6/11. On a hit, **swap the two full
   152-byte records** (a three-way whole-struct byte copy through a scratch
   buffer) — the whole struct,
   including its state byte, not just the kind — and fall through to
   the reveal tail, which now reveals-or-not based on whichever record ended up
   at the original tile.
2. Only if pass 1 exhausts all 200 tries: a second, independent 200-try
   search for a tile that is a still-standing brick with NO record at all
   (`sub_425FB9(x,y)==2 && !sub_42542D(x,y)`). On a hit, MOVE (not swap) the
   record there and `return` immediately — bypassing the reveal tail entirely, so
   this ignition reveals nothing.
3. If both passes exhaust (only plausible on an almost-fully-cleared board):
   fall through to the reveal tail with the record untouched — a plain, immediate
   reveal of the "over-powerful" kind, same as if the gate had never fired.

**Port:** `FlameSystem::relocate_overpowered_here` (`libs/sim/src/systems/
flames.cpp`), called from `spread_to`'s brick branch right after arming
`s.burning` and right before the existing hidden→floor reveal check — which
needs no changes at all, since it already does the right thing purely by
reading whatever `relocate_overpowered_here` leaves in `s.hidden[ty][tx]`
(populated = reveal fires normally on the swapped-in kind; empty = no
reveal, matching the original's early `return`). `Tuning::
overpowered_relocate_seconds` (id 102, default 40) gates it, multiplied by
`kTicksPerSecond` and compared against `State::tick` — the port's direct
analogue of `dword_464994` (both start at 0 and increment exactly once per
tick), used here as "ticks elapsed since round start" since a fresh
`Simulation` always begins a round at tick 0; no separate "elapsed" field is
needed.

**Which network gate, and why it's dropped.** The original also requires
`!sub_40C06A()` — a local, non-networked game (host=1/guest=2 both skip the
whole branch). This port has **no netplay concept at all** (`docs/adr/
0003-deterministic-sim-netplay-deferred.md`: "Netplay is explicitly out of
scope for now... without writing any netcode" — confirmed by an exhaustive
grep of `libs/`/`apps/` turning up zero transport/host/guest code; this is
currently a local-only, hotseat/shared-keyboard build). Every match this
port ever runs IS the original's "local" case, so `!sub_40C06A()` is
always-true here and is simply omitted rather than ported as a
permanently-true no-op field.

**The full-record-swap subtlety.** Because the original swaps the WHOLE
struct (state byte included), a candidate tile in pass 1 that is currently
mid-crumble (`s.floor[y][x]` populated, `s.burning[y][x] > 0`, still
`Cell::Brick`) is a legal swap target too, and the swap correctly trades
which ARRAY (`hidden` vs `floor`) each tile's kind lives in, not just the
kind value — modeled in the port as an explicit `cand_hidden` branch. The
mirror-image case (the SOURCE tile itself already being in `floor` state
when this code runs) is unreachable in practice: the only way a kind-5/6/11
token ever becomes visible while the window is still open is failing BOTH
400-draw passes on a nearly-brick-free board, at which point there are no
further candidate tiles left for a LATER re-hit to matter; the port reads
the source's kind from `s.hidden[ty][tx]` only, which is exactly what's
populated on every reachable call.

**Tests:** `tests/test_sim.cpp` — "a hidden Punch powerup relocates instead
of revealing near match start" (pass 1, swap), "...reveals normally once the
relocation window is disabled" (gate closed via `overpowered_relocate_
seconds = 0`), "...with no swap partner moves to an empty brick unrevealed"
(pass 2, move).

**GOLDEN IMPACT: none, verified two ways.** First, the full suite (38
suites including `golden`) passes byte-identical before and after this
change. Second — since "the golden boards hide no powerups of those kinds"
is NOT actually true (golden B's default `spawn_counts`, id-audit unchanged,
places 2 Punch + 2 Grab + a probabilistic SuperDisease under random bricks
on its dense near-full-brick board) — a throwaway instrumented run of golden
B's exact scenario (traced every tile that started with a hidden Punch/Grab/
SuperDisease token, then logged its `cells`/`hidden`/`floor`/`burning` state
every 100 ticks across the full 3000-tick run) showed all 5 such tiles in
that seed's placement (`(6,1)`/`(4,5)` Punch, `(3,6)`/`(10,10)` Grab,
`(1,6)` SuperDisease) are NEVER reached by a flame-triggered brick
ignition — 3 are crushed by the sudden-death closing wall around tick
2997-2999 instead (`EnclosureSystem::drop_wall`'s unconditional destroy, a
completely different code path that never calls `relocate_overpowered_
here`), and 2 are never touched by anything in the 3000-tick window at all.
So `relocate_overpowered_here` is provably called zero times across every
golden scenario — not merely "the hash happens to match", but "the new code
path never executes on any pinned board" — golden C/E's boards are
`pillars_config()`-based (near-zero bricks) or zero out `spawn_counts`
entirely, golden D zeroes `spawn_counts` and seeds its powerups directly
onto `floor` (never `hidden`, so the brick-ignite path never touches them),
and golden A has no board at all.

(Provenance: `sub_425107` pseudo.c 26274-26343, disassembly `0x425107-
0x425383`; the elapsed-value resolution, disassembly `0x425184-0x4251a4`; `sub_40C06A`
pseudo.c 11138-11142; `sub_4105B0`/`sub_4105D2` 14448-14552; `sub_40C035`
11123-11133; `off_45BE50` 2261-2281; `sub_4255B2` 26443-26483 (contrast,
see "Options toggles"/"Scatter occupancy test" above); VALUELST.RES line
"102,40" with its own "over-powerful powers... will go elsewhere" comment;
`docs/adr/0003-deterministic-sim-netplay-deferred.md`.)

## Disease system fidelity audit 2026-07-10 — line-by-line re-read (`sub_41DFB6`, `sub_41E21E`, `sub_41F29B` ~22904-22975, `sub_41DF4C`)

A full re-read of the disease code paths against `libs/sim/src/systems/
diseases.cpp`, following up the just-merged Core-feel audit (which already
fixed the reversed-disease application point and the Swap-locally-lost
behaviour — not re-litigated here). **Roster reconfirmed: exactly 9 diseases**
(`sub_41DFB6` line 22055 `rand_() % 9`), matching the existing table in
"Disease system" above (slow/fast/constipation/diarrhea/short-flame/super/
short-fuse/swap/reversed) — no 10th disease, no "tiny bombs"/"jelly-force"
kind exists in the binary.

Three real deviations found and fixed (per-disease arithmetic — molasses ÷3,
hyper ×3/2, diarrhea/super auto-drop, constipation gate, short-flame=1,
goldflame-overrides-short-flame, short-fuse ÷3, reversed `(g+2)&3` humans-only
— were independently re-verified term-by-term against `sub_41EB13` (pseudo.c
22480-22519), the movement disease-scaling site (23436-23440), and the
drop-gate site (23279-23310) and found **byte-for-byte IDENTICAL** to the
existing port; no changes there):

1. **Swap swapped an extra field.** `sub_41DFB6`'s swap (pseudo.c 22072-22082)
   is a 2-field XOR trick on the player's integer-pixel position ONLY
   (the two dwords at player **+28**/**+32** = the mover's `+0x1c`/`+0x20`,
   our x/y —
   cross-checked against the "Player movement / collision stepper" entry
   above, which independently pins +0x1c/+0x20 as "a plain integer pixel
   count"). `DiseaseSystem::give()` additionally swapped `move_budget`, which
   has no counterpart in the original — removed.
2. **Contagion ran before aging, not after.** `sub_41F29B` processes each
   player, in slot order, as: freshness-- (~22927-22928), then
   age+=frameDelta/cure (~22929-22942, `sub_41DF4C` on overflow), THEN that
   SAME player's own contagion scan (~22943-22974) — age-then-spread, per
   player, all nested inside "not stunned" (see #3). `DiseaseSystem::
   spread_and_age()` did the reverse: one global contagion pass over ALL
   players (using each source's PRE-age timer), then one global age pass.
   Reordered to age-then-contagion (two passes, age first). Effect: a disease
   that would expire this tick no longer spreads on its last tick (the
   original cures it, zeroing `+120`, before the contagion check runs), and a
   surviving disease transmits its post-age value. **Deliberately NOT
   replicated:** the original's single interleaved pass lets a source at a
   LOWER slot index hand a target at a HIGHER index a disease that then gets
   one bonus age-tick the same frame (the target's own turn, later in the
   same sweep, still runs after receiving it) — an index-order-dependent,
   sub-tick artifact of in-place mutation with no stable player-visible
   effect beyond one tick's timing out of a 300-tick duration. Same
   "documented, not replicated" treatment as the two items below.
3. **Stun did not freeze disease aging/contagion.** The entire block above —
   freshness decrement, age/cure, and the contagion scan (both as source AND
   as target: the scan's own validity check reads the candidate's dword at
   **+8** and requires it to be 0, pseudo.c 22951) —
   sits inside a block gated on the CURRENT player's dword at **+8** being 0
   (~22904), i.e. **not stunned**
   (`+8`, `Player::stun` — the same field/convention `ai.cpp` already uses as
   `present && alive && stun==0`, e.g. `ai.cpp:896/907/973`). Our port never
   checked `stun` for disease aging or contagion (either side), nor for the
   Swap target scan (`sub_41DFB6`'s target-validity test is the identical
   triple: candidate is not the source, candidate's +0 is truthy, candidate's
   +8 is 0). Added `stun == 0` to: the age/expire
   loop's per-player gate, the contagion source gate, the contagion target
   validity check, and `has_swap_target`/`give()`'s Swap target list.

   **CORRECTED 2026-07-10 (same-day follow-up commit; gate-fidelity audit,
   "Stun does NOT gate flame-death or pickup" under "Head hit" above) — point
   3 was WRONG and is REVERTED.** A from-scratch, brace-traced re-read of
   `sub_41F29B` (not relying on this entry's own citation) found `+8` to be
   the player's "already died this round" flag, NOT the head-hit stun
   countdown (which is a separate WORD at `+58`, confirmed against
   `sub_421F7E`'s explicit 16-bit-pointer parameter typing and never written
   by anything
   else). The `+8` gate that wraps the whole aging/contagion block (~22904 —
   "the player's dword at +8 is 0") is therefore the **ALIVE** gate, and the
   +58 stun is decremented *inside* it (~22982) — a field cannot gate a block
   that only decrements itself. The contagion target's own "+8 is 0" test
   (22951) and `sub_41DFB6`'s Swap-target "+8 is 0" test (22073) are the same
   +8/not-dead test.
   The `ai.cpp` convention this point leaned on ("`+8`, `Player::stun`") was
   the same mislabel propagated from an earlier AI RE pass (`sub_422718` 24741
   and `sub_421CB5` 24207 both test the candidate's +8 dword — both "skip
   DEAD", not "skip stunned":
   the AI has no reason to spare a defenseless stunned foe but obviously cannot
   target a dead one). **All `stun == 0` / `stun > 0` guards added by this
   point have been REMOVED** from `DiseaseSystem` (age/expire loop, both
   contagion gates, `has_swap_target`/`give()`'s Swap list) and the mirroring
   ones from `ai.cpp` (`behave_bomb_enemy`, `pick_live_enemy` ×2,
   `behave_seek_enemy`) — leaving the `present && alive` checks, which
   faithfully mirror `!player[2]`. GOLDEN: inert in scenario D (no head-hits
   there → no player is ever stunned), so every golden constant AND
   `kExpectedRng` stayed byte-identical (no RNG draw added/removed — verified;
   `tests/test_golden.cpp`'s own CORRECTION note documents this). Points 1
   (no move_budget swap) and 2 (age-then-spread) of this audit STAND
   unchanged. The two `test_disease.cpp` cases this entry originally added to
   pin "stun freezes aging/contagion" were themselves the mislabel and are
   rewritten to pin the opposite (stunned-but-alive DOES age/spread), plus a
   new stunned-but-alive Swap-target case.

**Two related items re-confirmed, NOT changed** (both were already flagged
"deliberately NOT changed, documented for honesty" by the Core-feel audit;
this pass independently re-derived the same conclusions from the raw
pseudocode and endorses them as-is):
- **Swap target pick: one draw over the valid set vs. the original's
  up-to-200 `rand()%10` rejection loop.** Rejection sampling over a uniform
  distribution is uniform over the accepted subset, so the CHOSEN target's
  distribution is provably identical; only the RNG draw COUNT differs (fixed
  1 draw vs. a geometric count, and 0 vs. up to 200 wasted draws when no
  valid target exists). Confirmed still true after this pass's fixes.
- **Cure roll draws even for a healthy player** (`sub_41E21E` ~22159-22167:
  the `rand() % cure_chance == 0` roll is gated only on `diseases_curable`,
  not on the player currently being sick) **vs. our `maybe_cure_on_pickup`,
  which short-circuits the draw when `disease_timer == 0`.** Curing an
  already-healthy player is a no-op either way (`sub_41DF4C` just re-zeroes
  already-zero fields) — outcome-identical, RNG-draw-count differs only on
  the already-documented healthy-pickup path.

**Inert detail, no action:** `sub_41F29B`'s contagion copies **14** bytes
(`+132`..`+145`) per infection, not the 9 documented disease-flag bytes
(`+132`..`+140`). `grep`ing pseudo.c for player-struct-relative accesses to
`+141`..`+145` finds none anywhere in the binary (the few raw `+141..+145`
hits are unrelated structs — linked-list node fields). Nothing ever writes or
reads these 5 bytes outside this blanket copy and `sub_41DF4C`'s matching
14-byte clear loop, so they are always zero and the extra copy is
unobservable. Not ported; noted here for anyone re-deriving the struct layout
who wonders why the copy width doesn't match the flag count.

**`diseases_destroyable` (id 120) composition with contagion/pickup:
re-verified correct, unchanged.** The skull-relocate path (`FlameSystem::
burn_powerup_here`, `BombSystem::slide`'s squash) only ever touches the FLOOR
token before pickup; `DiseaseSystem` only ever runs after a pickup already
happened. The two never interact within the same code path, so there is no
ordering question — `tests/test_stomped_diseases.cpp` already covers this
end to end (ON/OFF, flame-burned and slide-squashed skulls). `diseases_
will_recycle` (id 122) remains genuinely unconsumed (`docs/valuelst-map.md`
already flags this); out of scope here — it governs what happens to a
powerup that "leaves" play by a mechanism this codebase has not identified
yet, not a disease-arithmetic question.

**Hashed state: complete.** `disease` (9-bit mask), `disease_timer`, and
`disease_fresh` are all packed into one `mix()` call (`hash.cpp` ~127-132) —
every field `DiseaseSystem` reads or writes is covered.

**Follow-up RESOLVED 2026-07-10 (see "Stun does NOT gate flame-death or
pickup" under "Head hit" above) — the premise below was wrong.** The
~22904 gate is real, but the DWORD it tests (offset+8) is not the stun
countdown; it is the player's "already died this round" flag (set once, by
`sub_41DCB2`, on flame/other death — never by the head-hit handler
`sub_421F7E`, which writes a completely separate WORD at +58). The original
does NOT block powerup pickup (disease or otherwise) while merely stunned —
only while dead. `field_vs_players` not gating pickup on `stun` is therefore
**already correct, not a gap**; nothing changed there. The other condition
this note asked to have decoded (`sub_41DE63`/`sub_42708D`, ~22915-22917) IS
now fully decoded in that entry: it is a flame-death check, ANDed with the
same +8/"not dead" gate, run once per tick ahead of the pickup dispatch —
unrelated to stun. **Concern raised by that same pass, now CORRECTED (same-
day follow-up commit):** this section's OWN `stun == 0` additions (point 3)
cited the identical +8 field as "Player::stun" and DID rest on the same
mislabel — all of them (plus the mirroring `ai.cpp` target-liveness checks)
have since been removed and the two "stun freezes aging/contagion"
regression tests rewritten to pin the opposite. See point 3's own CORRECTED
box above and the "Head hit / Stun does NOT gate flame-death or pickup" entry.

**GOLDEN IMPACT (this audit's commit).** Zero RNG draws added or removed by
any of the three fixes (state/ordering only). Full suite run before/after:
golden A/B/C/E are byte-identical (proved — only "golden D: the disease
gauntlet" changed, and only at the tick 600/800 checkpoints; tick 200/400 and
`kExpectedRng` at all four checkpoints are byte-identical). `tests/test_golden.cpp`
recaptured; `tests/test_disease.cpp` gained 5 new cases (swap-vs-move_budget,
stun-freezes-aging, stun-blocks-both-contagion-ends, freshly-infected-not-
double-aged, multiply=off-stops-after-first) — the first four were confirmed
to fail against the pre-fix code before being accepted as real coverage.
**Superseded by the +8/+58 correction (follow-up commit, point 3 CORRECTED
box):** the `stun-freezes-aging` and `stun-blocks-both-contagion-ends` cases
were the mislabel and are rewritten to pin stunned-but-alive DOES age/spread
(+ a new stunned-but-alive Swap-target case); the D 600/800 hashes captured
here are unchanged by that revert (it is inert in D — no head-hits, no stun).

(Provenance: `sub_41DFB6` pseudo.c 22041-22098; `sub_41E21E` 22148-22280
(cure roll ~22159-22167, skull dispatch case 2/0xB ~22183/22225-22227);
`sub_41F29B` per-player block 22856-22975 (respawn/state gate 22856-22891,
stun gate 22904, pickup dispatch 22921-22926, freshness/age/cure 22927-22942,
contagion scan 22943-22974); `sub_41DF4C` 22023-22038; `sub_41EB13`
22480-22519; movement disease-scaling 23415-23454; drop-gate/diarrhea
23277-23310; reversed-disease site 23040-23057 (spot-checked, already fixed,
matches); `ai.cpp` present/alive/stun convention cross-reference.)
## Enclosure/HURRY arithmetic audit 2026-07-10 — line-by-line pass (`sub_426818`)

A full re-read of `sub_426818` (the enclosure stepper) and everything it
calls — `sub_410578`/`sub_4105D2` (clock), `sub_412135` (getvalue,
register-convention-confirmed), `sub_421D3F`/`sub_41DE63` (player crush),
`sub_42542D`/`sub_4254F3` (powerup destroy), `sub_422E48`/`sub_423209`/
`sub_424841`/`sub_42331C` (bomb detonate/eat) — plus the HUD "HURRY!" banner
check in `sub_42A191` (~29531-29549), triggered by this repo's line-by-line
fidelity audit workflow (same rigor as the Core-feel audit above). Full
derivation, evidence, and the reconstructed spiral in `docs/re/enclosure.md`.
Four deviations found and fixed (GOLDEN recaptured in the same commit,
`tests/test_golden.cpp`'s 2026-07-10 enclosure-audit note has the
per-scenario proofs — all RNG-neutral, every `kExpectedRng`/final-`rng`
assertion in the golden suite is byte-identical before and after):

1. **Trigger-boundary arithmetic (`sub_410578` vs. `sub_412135(101)`,
   `sub_42A191` ~29531-29549).** The banner is STRICT (`remaining <
   hurry_seconds`, confirmed via Watcom's register calling convention —
   `sub_412135` takes its id argument in EAX and returns its value in EAX
   too, which is what makes the decompile's bare, unassigned-looking
   `sub_410578()` / `sub_412135(101)` call statements actually feed the
   comparison, via an EDX-preserved value, rather than being discarded); the wall-arm is NON-STRICT (`remaining <= hurry_seconds
   - 5`). The prior port compared raw `ticks_left` directly against
   `threshold * kTicksPerSecond`, which is not equivalent to flooring
   `ticks_left/kTicksPerSecond` first and then comparing with the correct
   strictness — it silently fired the banner ~1 tick early and the wall-arm
   ~19 ticks late. `EnclosureSystem::update()` now floors once
   (`seconds_left = ticks_left / kTicksPerSecond`) and compares that with
   the original's exact operators.
2. **Spiral cadence — THE CRUX (`sub_426818`'s four interlocking
   advance / accept-or-turn blocks, ~27225-27298).** The original does not
   emit one event per
   unique tile: every 250 ms cadence slot unconditionally re-drops
   `sub_425E9B(x,y)` (and replays the wall-slam sound) at whatever `(x,y)`
   currently is, THEN computes the next position. A rejected turn that
   doesn't move (three of a ring's four corners) means the NEXT slot
   re-drops the SAME tile — a real extra 250 ms pause with no new tile, but
   a genuine second crush/detonate chance on it. The fourth corner (where
   the ring completes and the walk steps inward) is reached differently: the
   bounds check has no memory of already-visited tiles, so the up-walk
   naturally runs all the way back to the ring's own start tile as an
   ordinary ACCEPTED step before failing and wrapping. Net: every ring costs
   4 extra events beyond its unique-tile count (3 phantom corner repeats + 1
   ordinary-but-duplicate start-tile revisit) — 52 events for the outer
   ring's 48 unique tiles on a 15×11 board. `EnclosureSystem::total`/
   `position` now replay `sub_426818`'s own advance/accept-or-turn state
   machine tile-for-tile (a literal port, not a hand-derived ring-perimeter
   formula) so both quirks — and the degenerate innermost rings, which are
   only 1 tile wide/tall — fall out for free instead of needing hand
   special-casing. Fully pinned: `tests/test_sim.cpp`'s "the enclosure
   spiral's full ring-0 event order, phantoms and all" (all 52 events) and
   "a ring corner replays the wall-slam event before the next new tile".
3. **Wall-triggered bomb detonation is deferred one tick, not synchronous
   (`sub_423209` queue → `sub_42331C` drain, traced end to end).**
   `sub_423209(bomb, -1)` only appends to a 100-slot pending queue; the
   queue drains inside `sub_42331C`, gated to run at most once per frame via
   a frame-stamp (`dword_462210 != dword_464994`). `sub_42A191` calls
   `sub_42331C` twice a frame (`sub_4245B9` mode 0 BEFORE `sub_426818`;
   `sub_42459A` mode 1 AFTER) — only the mode-0 call, which runs first, ever
   drains, so a bomb queued by THIS frame's wall drop isn't force-fired
   until the FOLLOWING frame. `EnclosureSystem::drop_wall` now sets
   `Bomb::fuse = 1` instead of exploding synchronously, so the sim's own
   next `tick_fuses()` pass (which already runs before `enclosure.update()`
   in our tick order) detonates it on schedule — reproducing the one-tick
   gap without a new pending-queue concept. Known narrow gap NOT closed: a
   bomb mid-dud-fizzle (`Bomb::dud_left > 0`) has its fuse check skipped
   entirely by `tick_fuses`'s own dud branch, so a wall crushing a
   currently-fizzling dud is silently absorbed (keeps fizzling) instead of
   being force-detonated, unlike the original (whose drain-forced explode
   check isn't kind-gated). Too narrow an intersection (wall-crush ∩
   currently-a-dud) to chase further here.
4. **Player crush now exempts bounce/warp states (`sub_421D3F` → `sub_41DE63`,
   the SAME shared kill routine ordinary flame-death and the campaign
   rover/ghost landing-tile kill funnel through — `docs/re/campaign.md`
   clause 4).** `sub_41DE63` early-outs (returns 0, no death, no RNG) while
   the victim's movement-state word (+78) is 5 (trampoline hop) or 6/7 (warp
   out/in); player-type 4 (network-spectator) has no equivalent slot in this
   port (N/A, unreachable). `EnclosureSystem::drop_wall`'s player-crush check
   now also requires `p.bounce == 0 && p.warp == 0`. No existing scenario
   combines a bounce/warp with the wall-drop phase, so this is a no-op on
   every golden/regression scenario; test: "a bouncing or warping player is
   immune to the closing wall".

Confirmed unchanged (verified, not just assumed): the enclosure draws ZERO
`State::rng` (the only `rand()` in `sub_426818` is the presentation-only
drop-sound variant pick, once per arm); the depth/id-46/motion-exemption/
order-of-checks details from the 2026-07-08 "Options toggles" entry below all
re-verified identical.

> **RETRACTED 2026-07-26 — this entry's reading of `sub_426818`'s top-level
> gate was WRONG.** It said `sub_421969() > 1` is "a general match-active
> check with no enclosure-specific round-end special-casing, so the existing
> `GameApp` post-decision linger already gives the correct *walls keep closing
> for a few seconds after one side is left* behaviour". `sub_421969` is not a
> static match-active flag — it returns `dword_4621D4` (or `dword_4621DC` in
> team mode), which the per-frame player pass `sub_420F07` **relatches every
> frame** from the alive-side accumulator `sub_41F29B` fills. The walls
> therefore STOP DEAD when the round is decided, on the identical edge that
> freezes the bomb fuses, and never restart. Corrected in full, with the gate
> table for bombs / bomb movement / flames / enclosure, in
> `docs/re/enclosure.md` §8. (Not to be confused with TimeUp: the match clock
> hitting zero still does NOT stop the spiral — `docs/re/enclosure.md` §2.)

**Flagged DEVIATION-reported, NOT changed** (see `docs/re/enclosure.md` §4
"Ring count" for the full writeup): a literal transcription of the ring-stop
check (`if (2*getvalue(27) <= depth) return;`, evaluated once a ring
completes) reads as closing rings `0..2*depth` INCLUSIVE — one ring more than
VALUELST 27's own authored comment ("0 is none, 1 is 2 rows, 2 is 4 rows, 3
is all the way") and the pre-existing golden/test-pinned behaviour both say.
Re-verified via an independent re-implementation of that exact check (not
just re-read); the conflict could not be resolved by static analysis alone
(depth 3 "all the way" can't discriminate the two readings on a 15×11 board —
both exhaust its 6 rings). Kept the comment-and-golden-corroborated "2 ×
depth" rule; `rings_for()` in `enclosure.cpp` carries the same note.

Also traced, and explicitly OUT OF SCOPE (not touched): the SAME
`sub_423209` queue-and-drain-next-frame mechanism used by wall-triggered
detonation (#3 above) is ALSO used by the flame-arm walk's ordinary
grounded-bomb chain hit (`sub_42331C` ~25645). If that reading holds,
ordinary bomb chain reactions in the original take one extra FRAME per link
to cascade, not the same-tick synchronous/recursive chaining
`FlameSystem::spread_to` currently performs. This is a fundamental,
codebase-wide question about bomb-chain pacing with enormous potential
golden impact — well beyond "enclosure" — and deserves its own dedicated
audit; flagged for follow-up, not touched in this pass.

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
  it calls `sub_422E48(tileAheadX, tileAheadY)` (scan the 100-slot bomb array
  for a resting/kicked bomb on the tile ahead — excludes motion states 2
  flying / 3 carried), and **only when that returns a bomb** does it launch
  that bomb (`sub_424987(bomb, facing)`) and play `sub_427961(150)`. So the
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
  (dword). Each tick `sub_41F29B` releases it in the bomb-action block
  (23277-23380): if `+37` is non-zero, take that carried bomb; if the release
  is not blocked, reposition the bomb actor at the player's x/y, launch it
  with `sub_424987(bomb, facing)`, and clear `+37` to 0. `sub_424987` is the
  SAME launch primitive the punch uses — but here there is **NO `sub_427961`
  call anywhere in the block**. The throw is silent.
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
  `cell = sub_404852(x,y)` (0=blank, 1=solid, **2=brick candidate**), and then
  — only when `cell == 2` AND `rand_() % 100 >= dword_4647A0` — knock `cell`
  back to 0 before writing it out. So each
  brick candidate becomes a real brick with **`brick_density`%** probability;
  `>=` density knocks it back to blank. `#`/`.` cells copy verbatim and, thanks
  to the short-circuit (the `cell == 2` test comes BEFORE `rand_()`), draw
  **no** rand.
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

## Diarrhea/super auto-drop × grab-glove = serial throw — CONFIRMED (`sub_41F29B` bomb-action block)

Read 2026-07-04. The auto-drop diseases and the grab/throw glove interact through
three INDEPENDENT blocks that all run in one pass (the bomb-action block,
23277-23380), which the earlier port had collapsed into a carrying-vs-not
if/else. Naming the block's own auto-drop local `auto_drop`:

- **(1) Auto-drop flag.** When `+135` (diarrhea) OR `+137` (super) is set:
  `+56 ← 1`, `+54 ← 0`, `auto_drop ← 1` — forcing the bomb-key edge (`+56`
  down, `+54` not-last) every frame so the drop block fires each tick, and
  raising `auto_drop`.
- **(2) Throw block.** When `+37` (a carried bomb) is non-zero AND
  (`auto_drop` is set OR `+56` is clear): launch it (`sub_424987`) and clear
  `+37`. Not gated by constipation. So a carried bomb is
  released on key-up normally, but **`auto_drop` forces the throw EVERY
  frame**.
- **(3) Action2 block** (`+57 && !+55`): punch (+91), trigger (+95). Unchanged.
- **(4) Drop block**, gated on `+56 && !+54 && !+134` (constipation): GRAB
  your own resting bomb underfoot (`+92`), else SPOOGER line (`+93` set AND
  `auto_drop` clear — suppressed during auto-drop), else normal DROP
  (`sub_41EB13`). Only THIS block is gated by constipation.

Net effect with **diarrhea + grab**: each tick the drop block grabs the bomb
underfoot, the next eligible tick the throw block force-throws it (auto_drop), the
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

The block, in order:

1. **Threshold gate** — the round timer value is compared `<` against
   `sub_412135(101)` (= `getvalue(101)`, the hurry threshold in seconds).
2. **Second gate**, nested inside it — `sub_410578()` (the clock reader) must
   be `>` that same threshold value minus 5.
3. **One-shot voice**, nested inside both — while the latch `dword_464984` is
   still 0: set `dword_464984 = 1` and call `sub_427961(2700)`, the "HURRY!"
   voice. It therefore fires exactly ONCE.
4. **Banner**, every frame the two gates hold — resolve the `"hurry"`
   sequence via `sub_41D957(aHurry)` and queue it through `sub_415920`,
   flashing it every 4th frame (the frame stamp ANDed with 4).

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
the call site.** `sub_4278F2` (pseudo.c 27879-27896) bounds-checks its id
argument against the loaded-sound count `dword_463080`, then indexes
`dword_463094`/`dword_463088` by it before
calling `sub_411D17` (the actual sample-play primitive) — the SAME three
globals `sub_427961`'s sound-play path (pseudo.c 27902-27952) uses for its own
count check and table lookup. `sub_4278F2` is therefore a
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
latch byte at actor **+146**: the block runs only while that byte is 0, and
its first act is to set it to 1. On first activation,
outside the editor (`sub_40C06A() != 1`), it:

1. Clears the warphole's OWN tile: `sub_425E9B(x, y, 0)` (write cell type 0 =
   Blank via `sub_425E36`, bounds-checked).
2. Picks ONE random adjacent tile and clears it too, as a
   retry-until-in-bounds loop:
   - draw `rand() % 4` → a cardinal index `d`;
   - candidate `nx = dword_45BECC[d] + x`, `ny = dword_45BEDC[d] + y`;
   - retry (re-draw `d`) while `nx < 0`, and retry again while
     `nx >= dword_4648AC` (W = 15) or `ny < 0` or `ny >= dword_4648B4`
     (H = 11) — i.e. keep rolling until the neighbour is inside the grid;
   - print the debug string "knocking out %u,%u" via `sub_42C0C8`;
   - `sub_425E9B(nx, ny, 0)` — clear the neighbour.

   `dword_45BECC={0,1,0,-1}` (dx), `dword_45BEDC={-1,0,1,0}` (dy) — the cos/sin
   dir tables. Both are never 0 together, so the centre is NEVER a candidate (no
   explicit skip needed). The retry loop just picks a fresh cardinal
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
neighbours (the blocked-neighbour tally reaches 4) and its fidget counter
`+39` is idle, it sets the state word **+78** to `rand() % spread + 20` and
zeroes the state frame counter **+80**, where `spread` is `getvalue(330)`
guarded to be ≥1 (if `getvalue(330) <= 1` the spread is forced to 1,
otherwise it is `getvalue(330)` itself). VALUELST
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

## `bmstats.dat` / `bmstats.txt` — write-only play-telemetry dump, no reader, no reachable UI (2026-07-09)

Read for coverage-audit.md §3 / "Top open items" #5: unlike `LEVELS.DAT`,
`bmstats.dat`'s file timestamp (post-dates the other installed files) implied
the game *writes* it at runtime. Confirmed — this is a debug/QA telemetry
feature, not a level table. `pseudo.c`'s partial decompile (1134/1533
functions) does not cover the responsible code (`grep -in "bmstats" pseudo.c`
and every individual stat-label word are all zero matches), so this entry was
pinned via targeted Capstone disassembly of `BM95.EXE` directly (per
`docs/re/method.md`'s documented fallback), anchored off the `bmstats.dat`/
`bmstats.txt` string literals (raw file offset `0x57484`, image VA
`0x458084`/`0x458090`) and their cross-references.

**The two files, byte-for-byte:**

- `bmstats.dat` is exactly 400 bytes = a flat array of **100 little-endian
  `int32` counters**. Only the first **19 slots** are ever populated (indices
  0-18); slots 19-99 are always zero (reserved/unused capacity). Verified by
  decoding this install's actual file and checking every dword against the
  human-readable `bmstats.txt` dumped alongside it — all 19 values match
  exactly, in order.
- `bmstats.txt` is a formatted two-column ("Total" / "Last Run") report of
  those same 19 counters. Its title, column header, and all 19 row labels are
  **not embedded in `BM95.EXE`** — they are pulled at runtime from
  `messages.txt` (already parsed by `libs/assets/src/messages.cpp`, ids are a
  flat lookup): id **900** = "Bomberman Statistics File:" (title), id **905**
  = the "Total      Last Run" column header, ids **910-928** = the 19 stat
  labels in file order (Matches Started, Games Started, Frames Rendered,
  Graphic Requests Serviced, Bombs Dropped, Player Deaths (all), Local AI
  Player Deaths, Bricks Destroyed, Total Pixel Distances Run, Net Games
  Hosted, Net Games Joined, Total Network Bytes In/Out, Total Network Packets
  In/Out, Attract Modes Started, Network Packet Retransmits, Ghost Bomb
  Actions Found/Lost). Confirmed by grepping the install's own `messages.txt`
  for these exact strings — found at ids 900-928, verbatim.

**The writer — `sub_40200C` (0x40200C-0x40214F), CONFIRMED:**

1. `0x40201A-0x402049`: loop `i = 0..99`, `Total[i] += Current[i]` where
   `Total` = a 100-dword array at VA `0x46442C` and `Current` = a 100-dword
   array at VA `0x46429C` (both plain zero-initialized process memory, well
   past `DGROUP`'s file-backed range — i.e. ordinary `.bss`-style globals,
   not resource data).
2. `0x40204B-0x40207D`: opens the file named by `*(char**)0x45B7B0` (a
   static-initialized global pointer; its compiled data value is
   `0x458084` = the `"bmstats.dat"` string — confirmed by dereferencing the
   global directly in the PE image) in mode `"wb"`, `fwrite`s exactly 400
   (`0x190`) bytes from `Total[]`, closes.
3. `0x402082-0x402145`: opens `*(char**)0x45B7B4` (statically = `0x458090` =
   `"bmstats.txt"`) in mode `"wt"`. Writes msg-id 900 and msg-id 905 each via
   format `"%s\n\n"` (reproducing the title line, blank line, column-header
   line, blank line seen in the file). Then loops `i = 0..18`: fetches msg-id
   `910+i` via `sub_4124A4` (= `getstring`, the MESSAGES.TXT string lookup —
   see the 2026-07-09 correction under "VALUELST lookup mechanism"; the ids
   in the 900s here are MESSAGES.TXT string ids, consistent with the labels
   being install-editable text), `sprintf`s
   `"<label>:"`, then `fprintf`s `"%-30s %13u %13u\n"` with the label,
   `Total[i]`, and `Current[i]` — i.e. **"Last Run" is literally
   `Current[i]`, un-accumulated**, not a before/after diff. Closes.

This fully explains both files: `bmstats.dat`'s 100-slot/19-populated layout,
and `bmstats.txt`'s exact title/header/row text and column semantics.

**Runs automatically at shutdown, not on a menu/key:** `sub_402150` ends with
`mov eax, 0x40200C; call sub_410EBF` — `sub_410EBF` (0x410EBF-0x410EFE) is a
generic "register a deinitializer" helper (append `fn` to a ≤32-slot table at
`0x4601C0`, bump a count at `0x460240` — confirmed by its body, and by the 20
distinct direct callers found across the binary registering their own
per-subsystem cleanup). The table is walked and every registered function
pointer invoked by `sub_410F00`, which logs `"Calling deinitializer
functions (%u total)"` (string cross-reference confirms this is the
dispatcher) — i.e. `sub_40200C` is wired to run once, automatically, whenever
the game's deinit sequence executes (process exit / return-to-DOS), not from
any menu, screen, or key the player can reach. (`sub_402150`'s own remaining
body — it opens a file via the same `0x45B7B0` global and a *different*
string, `"Statistics for a %u-player game."`, and writes the same `Total[]`
buffer through a different write call — wasn't fully resolved; it looks
adjacent to the separate `netstats.txt`/`critlog.txt` net-traffic logging
strings found in the same string pool, which is netplay-only and out of
scope per ADR-0003 regardless.)

**The 19 counters are live, not vestigial:** `Current[]` (`0x46429C`) is
incremented inline at real gameplay event sites, not through a single
"stat++" helper. Example, `sub_410F81` — already documented above/in
`docs/re/campaign.md` as the local player-setup / match-start entry point —
contains `inc dword ptr [0x46429C]` (index 0 = "Matches Started") right at
match start, alongside its (also already-documented) `sub_40C06A`
network-mode check and campaign/goldman-wheel dispatch. This confirms the
counters are fed by actual play, not dead instrumentation left disconnected.

**Negative finding — `bmstats.dat` is never read back, by anything:**
exhaustively searched the *entire* PE image (every section, not just code)
for every raw 4-byte occurrence of: the `Total[]`/`Current[]` array base
addresses (`0x46442C`, `0x46429C`), the `"rb"`/`"rt"` read-mode strings, and
the `bmstats.dat`/`bmstats.txt` string VAs themselves. Every hit resolves to
one of the two write call sites documented above — there is no third call
site, and no `"rb"` (read-binary) `fopen` anywhere in the binary references
these globals or this filename. Consequently:

- `Total[]` starts at zero on every process launch (it is uninitialized
  process memory, not loaded from the file) and only ever accumulates
  *within* that one run before being overwritten to disk at exit.
- The apparent cross-session accumulation visible in a populated
  `bmstats.dat`/`.txt` (e.g. this install's own copy shows "Total" values —
  115 matches, 13.9M frames rendered — far exceeding a single "Last Run") is
  **not produced by `BM95.EXE` re-reading and folding in its own prior
  totals**; nothing in this binary does that. It is an artifact of however
  that particular file came to be (most plausibly one very long-lived
  process run, or a copied-in/pre-populated file) — not something the port
  needs to, or sensibly could, reproduce.

**Reachability — confirmed nothing consumes these files either:**
`bmstats.txt` is written to the install directory and never opened for
reading, displayed, or referenced by any menu/screen/results-tally code path
in the RE'd frontend flow (`docs/re/frontend-flow.md`,
`docs/re/results-and-options.md`) — it is invisible during play, a debug/QA
artifact dropped next to the executable, not a player-facing "statistics"
screen. `bmstats.dat` has no reader at all, in this binary or any other
shipped executable (same exhaustive-search method as the `LEVELS.DAT` entry
above, repeated for this file).

**Conclusion — pinned, not ported (docs-only), and why:** every byte of
`bmstats.dat`/`.txt` is now fully explained (layout, labels, trigger, and the
fact that the "Total" column is not really cross-session-persistent in the
original either). It fails every one of CLAUDE.md's porting gates: it does
not gate a reachable feature (nothing displays it), the sim/determinism
contract explicitly forbids wall-clock/file I/O of this kind in `libs/sim`,
and adding a `libs/game`-side writer would only reproduce an invisible,
write-only side file with no observable effect on any test or on-screen
behavior — pure surface-area for zero player-facing value. Per the task's own
guidance ("if it's write-only telemetry with no user-visible consumer,
document that as a negative finding... if in doubt, docs-only"): **docs-only,
no port.** (Provenance: string literals + PE section table via a one-off
Capstone/PE-parsing script per `docs/re/method.md`; `sub_40200C`
0x40200C-0x40214F; `sub_402150` partial, 0x402150-0x4021A7; `sub_410EBF`
0x410EBF-0x410EFE; `sub_410F00` dispatcher; `sub_410F81` +0x410FDE inline
increment; `sub_4124A4` shared id lookup, already documented above as
`getvalue`; `messages.txt` ids 900/905/910-928 in this install's own copy.)

## Third `.DAT` file identified — `WINEREG/EReg058.dat` (2026-07-09)

Read for coverage-audit.md §3's `.DAT` row: `LEVELS.DAT` and `bmstats.dat`
were RE'd above; a `find`-based inventory of the install tree counted 3
`.DAT` files total but never named the third.

**Finding: `D:\...\BOMBRMAN\WINEREG\EReg058.dat`** (1079 bytes, plain text).
Hexdump shows an un-filled INI-style form:

```
[Public User Data]
Salutation=
FirstName=
Initial=
LastName=
JobTitle=
Company=
Division=
Address1=
Address2=
City=
State=
Zip=
Country=
Phone=
Extension=
Fax=
EMailAddress=
EMailService=
HaveModem=
ModemSpeed=
HaveCDROM=
CDROMSpeed=
HaveJoystick=
HaveGamepad=
LocationCountry=
OutsideLineAccess=
...
```

— every key blank: the bundled product-registration wizard's un-filled
"Public User Data" template, not user data actually entered on this
install. It sits in `WINEREG/` alongside `WINEREG.EXE`, `EREGUI32.DLL`,
`EREG3201.DLL`, `EREGUI.INI`, and `INTER.BMP` — the exact same bundled
registration-wizard tool folder already closed as non-gameplay by the
`.BMP` row (`docs/re/coverage-audit.md` §3: "lives inside the bundled
WINEREG.EXE/EREGUI32.DLL registration-wizard tool's own folder").

`grep -in "ereg\|058\.dat\|registration" pseudo.c` — **zero matches**,
confirming `BM95.EXE` never opens this file (same negative-evidence method
as the `LEVELS.DAT`/`bmstats.dat` entries above).

**Conclusion: non-gameplay tooling data, same class as `WINEREG/INTER.BMP`.**
Closes the `.DAT` row's outstanding "3rd file unchecked" item; no parser
warranted (it belongs to a separate bundled EXE, not the game).

## Presentation LCGs must reseed from real entropy at boot — CONFIRMED (`sub_41095A`, pseudo.c 14602-14663)

Read/fixed 2026-07-09, reported as "RANDOM level selection always gives the
same map." Root cause: our four presentation-only LCGs (`setup_lcg_`,
`attract_lcg_`, `goldman_lcg_`, `next_seed_` in `GameApp`) were left at fixed
literal seeds set once at construction (member-initializer defaults), so
every fresh process replayed the exact same "random" sequence — most visibly,
`next_seed_` feeds both `match::pick_stage` (the LEVEL & ROUNDS RANDOM pick)
and `match::build_match_config`'s per-round brick fill/spawn shuffle, so a
freshly-launched game's first RANDOM level pick was always identical.

- **The original seeds its C `rand()` from the wall clock exactly ONCE at
  process boot**, not per round/per match: `sub_41095A` (the top-level init
  routine, called once from `WinMain`'s equivalent at pseudo.c 30950) runs
  `time_(); srand_();` twice in a row — once at entry (pseudo.c 14610-14611,
  before any subsystem/config load) and again right after loading the boot
  dialogs/VALUELST config (pseudo.c 14639-14640). Both calls are boot-time
  only; nothing later in the binary calls `srand_()` again. Every downstream
  `rand_()` draw (stage RANDOM pick in `sub_410B6E` pseudo.c 14746-14755,
  brick fill in `sub_4260F5`, attract-mode roster/stage, the Goldman wheel's
  5 draws) just continues pulling from that one continuous, wall-clock-seeded
  stream for the rest of the process's life — this is the same `time_()`/
  `srand_()` pair the "Per-match brick fill" entry above already cites for
  the brick-fill RNG source.
- **Per-round cadence confirmed separately**: the RANDOM stage pick inside
  `sub_410B6E` (pseudo.c 14746-14755 — `dword_46499C ← rand_() % stage_count`,
  a 200-try retry against the enabled-level VALUELST flags 1150-1160) is NOT
  gated to run once per match. `sub_410B6E` is invoked from the top of
  `sub_42A3F6` (pseudo.c 29609, gated `if (!dword_464A68)`), and
  `sub_42A3F6` itself is called AGAIN, recursively, from the results/
  scoreboard screen handler for every subsequent round of the same match
  (pseudo.c 30650, `++dword_4642C4; sub_42A3F6();`) — the per-round tick
  loop `sub_427342`/`sub_4293E5` resets `dword_464A68 = 0` on entry (pseudo.c
  15044), which is what re-arms `sub_410B6E`'s `if (!dword_464A68)` gate for
  the next round. So a RANDOM level IS free to change round-to-round within
  one match in the original — this matches our existing architecture
  unchanged (`GameApp::run_match()` already calls `start_match(next_seed_++)`
  once per round, at line ~2457 of `game_app.cpp`); no cadence change was
  needed, only the boot seed.

**Port.** `GameApp::init()` now reseeds all four presentation LCGs from a
fresh per-process `random_boot_seed()` (`game_app.cpp`, mixing
`std::random_device` with `std::chrono::high_resolution_clock` — real OS
entropy rather than reproducing Watcom's exact `rand()`/`srand()` algorithm,
which is unnecessary since none of these four LCGs ever touch
`bomber::sim::State::rng` or the hashed sim state; ADR-0003 is untouched).
This is done first thing in `init()`, before anything reads one of the four
LCGs. Presentation-only, as with every LCG in this file: `setup_lcg_`,
`attract_lcg_`, `goldman_lcg_`, and `next_seed_` never feed
`bomber::sim::State::rng`. **No golden impact** — `libs/sim` untouched, only
`libs/game`'s boot sequence changed. (Provenance: `sub_41095A` pseudo.c
14602-14663; `sub_410B6E` pseudo.c 14688-14857; `sub_42A3F6` pseudo.c
29609-30185 and its recursive call site at pseudo.c 30650; `sub_4260F5`
already cited above.)

## id-audit.md presentation-side follow-ups — PORTED (2026-07-09)

Three presentation-only gaps from `docs/re/id-audit.md`'s work queue, ported
this pass. All three are cosmetic (sound picks / sprite overlays), so none
touch `libs/sim`, `State::rng`, or the golden hashes (determinism contract
rule 6) — see `docs/valuelst-map.md` for the id-to-consumer table entries.

**1. "Fire In The Hole" taunt (VALUELST 650/651, SOUNDLST 1200 group).**
`sub_41F29B` pseudo.c ~23343-23368 (the plain single-bomb-drop branch of the
bomb-action block's drop path, file comment "after laying out a HUGE string
of bombs"). In order: read the "many bombs" threshold `getvalue(651)`; the
taunt fires only when an UNRESOLVED register value is `>=` that threshold AND
the player's bomb count minus 1 equals the live-bomb count taken before this
drop (i.e. this drop just filled the player's capacity); then take the chance
denominator `max(1, getvalue(650))` and, on `rand() % denominator == 0`, play
`sub_427961(1200)`.

The unresolved comparand is read from a register the decompiler itself flags
"possibly undefined" (at `420C49`) — no visible assignment anywhere in the
function. Best-supported reading (matches the VALUELST comment "what
constitutes 'many' dropped bombs" and reuses the max_bombs byte already in a
register a few lines up for the drop-eligibility gate): it is the player's
current bomb-count powerup level (max_bombs). Ported to
`sound_director.cpp`'s `BombPlaced` handler: `pl.max_bombs >=
tuning.taunt_many_bombs(651) && pl.bombs_placed == pl.max_bombs` (post-
increment, since `BombSystem::place` bumps `bombs_placed` before emitting
the event) gates a `audio_.chance(tuning.taunt_many_chance(650))` roll.
Layering: the trigger reads straight off already-hashed sim state carried by
the event (no new `State` field); the roll itself is `AudioEngine`'s RNG,
never `State::rng`. Not gated to the single-drop-only path the original uses
(spooge's multi-drop loop shares `BombSystem::place` and could also complete
the cap) — an unobservable, cosmetic-only widening.

**SOUNDLST range correction:** id-audit.md described the sound group as
1200-1203 ("clear"/"fireinh"/"lookout"/"litemup") with 1204+ as an unrelated
"runaway/DMB/ZAE/JMB" pool. The raw `SOUNDLST.RES` has no gap or comment
between 1203 and 1204 — the block runs contiguously through id 1279, and the
file's own closing comment reads `;1299 is last "huge string of bombs" sound
value`. So 1200-1299 is ONE authored block for this taunt (mirroring the
700-999 death-taunt range fix already in `sound_director.cpp`), not just the
first four slots. Fixed to `play_random_in_range(1200, 1299)`.

**2. Gold-player "twinkle" sparkle (VALUELST 1010, `docs/re/goldman-
roulette.md` §6).** Addresses already pinned there: `sub_420D4E` (spawn,
pseudo.c 23549-23583) scans a fixed 100-slot particle pool for the first
empty slot and, while round-elapsed < getvalue(1010) seconds (0 =
indefinitely), rolls a 5-in-6 chance (`rand()%6 != 0`) to spawn a particle at
`(player_x + rand()%40-20, player_y + rand()%50-48)` — three presentation
rand draws per attempt, one attempt per matching player per tick.
`sub_420E39` ages/draws each active particle every tick (`sub_41DAA7(seq,
age)`, sequence name `"goldman"` — `pseudo.c` `aGoldman_0[8] = "goldman"`,
confirmed present in `MISC.ANI`'s sequence table alongside "ring"/"safe"/
"scan") and retires it once its age exceeds the sequence's own frame count.
`sub_420F07`'s per-tick loop (pseudo.c 23628-23670) selects WHICH player(s)
sparkle: solo play compares `dword_46492C == playerIndex` directly; team
play compares `dword_46492C == (player.team ? 2 : 0)` — i.e. in team mode
the stored value is a TEAM id, not a slot index, matching
`bomber::game::assign_gold_player`'s own documented dual encoding. Ported to
`Renderer::update_gold_sparkles` (spawn/age, called once per tick from
`sample_movement`) + a draw pass at the end of `draw_world`; `GameApp`
feeds the live `gold_player_`/`is_team_mode()` pair in every frame via the
new `Renderer::set_gold_player`. Presentation-only: the pool, its LCG
(`gold_lcg_`), and the resolved "goldman" `Anim` all live in `Renderer`,
never `State`.

**3. Bomb-pickup carry arc (VALUELST 500/502/504/506, "the curve (upwards)
of a bomb being picked up").** Consuming function pinned: `sub_42331C`'s
bomb state-3 ("carried") branch, pseudo.c ~25478-25497, entered every tick a
bomb is in the carried state. Gated on the CARRIER's player-state field +78
== 4 ("picking up"): a curve index `n = clamp((carrier's +80 elapsed-frames)
− 1, 0, 3)`
(the same +78/+80 state+counter packing documented for the trampoline hop,
`Player::bounce`'s doc comment) indexes a 4-point curve, ids 500/502/504/506
each holding an (X,Y) pair (`ValueList::column_or`, the VALUELST multi-
column flattening documented in `docs/formats/valuelst.md`):
`bomb.x = 10*dx[dir] + carrier.x + getvalue(2*n+500)*dx[dir]`,
`bomb.y = 10*dy[dir] + carrier.y - getvalue(2*n+501)`, where `dx[dir]`/
`dy[dir]` are the standard {Up,Right,Down,Left} unit-vector tables
(`dword_45BECC`/`dword_45BEDC`). `n` clamps at 3 and never resets while
carrying continues, so the bomb ramps up over the first ~4 ticks of the
grab then SETTLES at the last curve point `(12,40)` for the remainder of the
carry — not a one-shot pop. Ported to the carried-bomb draw in
`Renderer::draw_world`, replacing the previous unpinned fixed `sy - 78.0f`
offset guess; `Renderer::carry_ticks_` (updated once per tick in
`sample_movement`, gated on a `carrying_prev_` edge-detect so it starts at
index 0 on the grab tick) mirrors the +80 counter. Presentation-only, read
live via `ValueList::column_or` with the shipped values as fallback.

## Per-level tile regeneration — CONFIRMED (2026-07-09, `sub_426704`, called from `sub_426818`)

VALUELST ids 340-350 (per-level, one per stage: "which levels have
regenerating tiles, and how many seconds between tile regeneration attempts
(zero means no regeneration)") and 695 ("clear cell radius that must exist
around a potentially-regenerating tile spot; nobody can be within this
radius"). Shipped values: **340-350 are all 0 except id 347 = 4** ("cemetary/
mortuary" — the file's own inline comment; this is level index **7**, which
the port's own `level_fallback`/VALUELST-450 naming — see "Ice / input-lag"
below — independently confirms is **Haunted House**, so the dev-internal
codename and the shipped level name are the same board). **695 = 4**.

**Call site — `sub_426818` (pseudo.c ~27169-27170), the enclosure stepper**
(already pinned in `docs/re/enclosure.md` for the HURRY wall-closing spiral):

The call is guarded by two conditions ANDed together, in this order: first
`sub_40C06A()` must not return 1, then `getvalue(dword_46499C + 340)` — this
level's regen interval — must be non-zero. Only then is `sub_426704()`
called.

`sub_40C06A() != 1` is "not the editor"; `sub_412135` is `getvalue` (see the
2026-07-09 correction at the top of this file — `sub_4124A4` is `getstring`,
NOT `getvalue`; every `id-audit.md`/older-doc citation of `sub_4124A4` as
"getvalue" for this feature is the same pre-correction mislabel). This whole
call — and thus `sub_426704` and its 100-attempt RNG loop below — is
**short-circuited to never run at all** on any level whose regen id is 0, so
levels other than Haunted House draw **zero extra RNG** from this mechanic.
The call is nested inside `sub_426818`'s own outer `sub_421969() > 1` gate
(the same gate the wall-closing spiral itself uses). **CORRECTED 2026-07-26:**
that gate is NOT implied by our `run_tick` — it is the round-decided freeze
(`docs/re/enclosure.md` §8), re-latched every frame, so the regen stops the
moment one side is left, exactly like the spiral. `simulation.cpp` now gates
`tile_regen.update()` with `round_frozen` alongside `enclosure.update()`,
since the original covers both with this one `if`.

**`sub_426704`** (pseudo.c 27093-27132):

Its body, in order:

1. **Editor gate** — `sub_40C06A()` must not return 1 (redundant with the
   caller's own gate above; the function re-checks it anyway).
2. **Interval** — read this level's regen interval in SECONDS,
   `getvalue(dword_46499C + 340)`.
3. **Attempt clock** — read the ms clock `sub_43ACF8()` (`timeGetTime`) and
   compare against the last-attempt stamp `dword_464978`: the cycle runs only
   when `now − dword_464978 > 1000 × interval`.
4. **Re-stamp** — `dword_464978 ← now`, UNCONDITIONALLY, before any candidate
   is tried (so a cycle that finds nothing still costs a full interval).
5. **Candidate loop**, up to **100** iterations. Each iteration draws TWO
   randoms in this order: tile X = `rand_() % dword_4648AC` (in [0,15)), then
   tile Y = `rand_() % dword_4648B4` (in [0,11)).
6. **Eligibility**, evaluated as one short-circuited AND chain on the drawn
   tile: `sub_425FB9(x,y)` must be 0 (blank cell), `sub_42542D(x,y)` must be
   false (no powerup record), `sub_422E48(x,y)` must be false (no grounded
   bomb). Only past all three does it read the clear radius
   `getvalue(695)` and call `sub_422351(radius)` — true = no player within
   that radius.
7. **On success** — `sub_425F79(x, y, 2)` writes the tile as BRICK (cell type
   2), then the function `return`s the result of `sub_40FDE8(x, y, …, 2)`
   (see below — NOT a visual/sound call). The `return` is inside the loop, so
   the first success ends the whole cycle.

**Exactly ONE brick regrows per successful attempt cycle** — the loop
`return`s the instant the first eligible candidate is found; the remaining
`100 - i` attempts of that cycle are never drawn. If ALL 100 attempts fail
(no eligible tile that cycle), `dword_464978` was still reset at the top —
the next attempt cycle is a full interval later regardless of success. RNG
draws are exactly 2 per attempt (x then y), 2..200 per cycle depending on
when/if it succeeds — the draw COUNT is part of the contract, the exact
number of draws in a given cycle is data-dependent (matches the codebase's
existing `PowerupSystem`'s "Random powerup" re-roll idiom, `simulation.cpp`'s
`field_vs_players`, same 1-draw-per-attempt/stop-on-success shape).

**Eligibility, in order** (each a pre-existing, already-pinned tile-occupancy
helper — see "Explosion physics" / "Options toggles" entries above for
`sub_425FB9`/`sub_42542D`/`sub_422E48`'s own pinning):
1. `sub_425FB9(x,y) == 0` — the tile is BLANK (not brick, not solid).
2. `sub_42542D(x,y)` false — no floor/hidden powerup record on the tile.
3. `sub_422E48(x,y)` false — no grounded bomb on the tile.
4. `sub_422351(radius)` true — no live player within `radius` tiles.

**`sub_422351(radius)` — the clear-radius check.** High confidence on the
FORMULA (Manhattan distance, matching id 695's own comment "nobody can be
within this radius"), lower confidence on exactly HOW the candidate tile
reaches the callee: the decompiled call site shows only the RADIUS as an
explicit argument, no `(x,y)`. `sub_422351`'s body iterates
the 10-slot player array, converts each player's PIXEL position to TILE
coordinates via the same `sub_42665C`/`sub_4266A3` helpers used everywhere
else in the binary for pixel→tile conversion (confirmed via their OTHER call
sites — e.g. pseudo.c 24207 compares `sub_42665C(player+28)` against the
target tile X and `sub_4266A3(player+32)` against the target tile Y, an
explicit tile-coordinate COMPARISON), then combines two per-axis
`abs()` results into a single check against the radius. Both this function
AND its sibling call site (`sub_4019C2` pseudo.c ~4777, `sub_422351(3)` — an
apparent "place something 3 tiles from every player" spawn-placement helper)
call it with ONLY the radius as a visible argument, which — under Watcom's
EAX/EDX/EBX/ECX register calling convention (`docs/re/facts.md` "Binary
profile") — is consistent with the candidate (x,y) surviving in registers
left live by the immediately-preceding occupancy checks rather than being
freshly reloaded for this call (a Hex-Rays argument-reconstruction blind spot
for implicit register reuse, not a hidden global). The FORMULA itself is not
in doubt (own VALUELST comment + the `abs`+`abs` shape); the register
mechanism is inferred, not literally read off the call site.

**Port** (`libs/sim/src/systems/tile_regen.hpp/.cpp`, `TileRegenSystem`):
`Tuning::regen_seconds[11]` (ids 340-350) and `Tuning::regen_clear_radius`
(id 695), indexed by the new `Tuning::level_index` (NOT itself a VALUELST id
— the "which of the 11 stages is this" selector, set by `bomber::match`/
`GameApp::start_match` from the resolved stage number, the same value the
original calls `dword_46499C`). `TileRegenSystem::update()` mirrors the loop
above 1:1: a per-match `State::regen_timer` (ticks, hashed) counting down
from `regen_seconds * kTicksPerSecond`; on 0, ONE attempt cycle (≤100 random
tiles via `State::rng`, Manhattan-distance clear-radius check against every
PRESENT+ALIVE player — see below), then reset. Called from `simulation.cpp`
step 6, immediately before `EnclosureSystem::update()`, mirroring
`sub_426704` being nested inside `sub_426818`.

**`clear_of_players` gates on `present && alive`, not the original's raw
10-slot scan.** The decompiled `sub_422351` has no visible active-player
check (unlike its sibling `sub_42247A`, which does check a liveness byte) —
but the original's fixed 10-entry struct array always holds SOME position for
every slot, occupied or not, while our `State::players` is likewise a fixed
`kMaxPlayers`-size array whose unused slots default-construct to tile (0,0).
Iterating unconditionally would make every unused slot in any match with
fewer than 10 players permanently block regeneration within `regen_clear_
radius` of the top-left corner — clearly not the intended behaviour.
Gating on `present && alive` (the SAME convention already used throughout
this codebase — `grid::player_at`, `EnclosureSystem::drop_wall`,
`alive_count`) is the faithful, sensible interpretation; a dead player's last
known position does not block regrowth, matching how "dead" is treated
everywhere else in the sim.

**Visual/sound — CONFIRMED there is neither.** `sub_40FDE8` (the function
`sub_426704` calls after writing the brick) is a **netplay replication
packet send** (packs `(x,y,type)` into a buffer and hands it to the datagram
sender `sub_40CE27` under message kind **0x30**, the whole thing gated on
`dword_460058 == 2` — the same shape as the OTHER netplay-packet
senders `sub_40FE88`/`sub_40FF14` nearby in the same source region), not a
visual/animation trigger — out of scope per ADR-0003 (netplay deferred).
The brick write itself goes through `sub_425F79` → `sub_425E9B` →
`sub_425D22`, the SAME general-purpose tile-write path used for every cell
mutation (confirmed already for the enclosure wall drop, `enclosure.md` §4):
`sub_425D22` (pseudo.c 26737-26773) formats `"TILE%u_BRICK"` for the current
level and blits frame 0 of that ANI directly to the cell's rect — the level's
ordinary static brick art, instantly, no distinct "regrow" animation, no
`sub_427961`/`sub_4278F2` sound-play call anywhere in `sub_426704`. Port:
`Event::Type::TileRegrew` exists only so the presentation can react/redraw
without diffing the grid every frame — it does not imply a distinct sound or
animation; `SoundDirector` maps nothing to it (there is nothing to map).

**Cadence: process-lifetime clock, mapped to a per-match countdown.**
`dword_464978` (the last-attempt timestamp) has no init call site anywhere in
`pseudo.c` — it is a zero-initialised global that persists across ROUNDS
within the same process, not reset per-round. In the overwhelmingly common
case (the first time this code path ever runs in a session) `dword_464978 ==
0` and `timeGetTime()` is already well past `1000*4 = 4000` ms since boot, so
the FIRST attempt cycle fires on the very first tick the mechanic is live.
Our per-match sim has no meaningful analogue of "ms since process boot" to
carry across matches, so `State::regen_timer` starts at 0 (first attempt
cycle on tick 1 of any Haunted House match) rather than trying to emulate
cross-round timestamp leakage, which would not be reproducible/faithful
for a deterministic per-match model anyway.

**GOLDEN: no impact, proved by running the full suite before/after.**
`Tuning::level_index` defaults to 0 ("new traditionalist"), whose
`regen_seconds[0] == 0`, so `TileRegenSystem::update()` takes its very first
early-return branch on every existing scenario — zero RNG draws, `regen_
timer` pinned at 0 forever. Two NEW hashed fields were unavoidable
(`State::regen_timer`, and `Player::ice_history` from the ice mechanic below,
added in the same commit) — see hash.cpp's own comments and `tests/
test_golden.cpp`'s "UPDATE 2026-07-09 (per-level tile regeneration +
ice/input-lag)" note for the recapture and the byte-for-byte proof that every
non-hash assertion (RNG streams, jelly bounce count) is unchanged. New
suite: `tests/test_regen.cpp`.

## Ice / input-lag — CONFIRMED (2026-07-09; RE-DERIVED, cold-start CORRECTED 2026-07-10, `sub_41F29B` ~23058-23078)

> **2026-07-10 distrustful re-derivation** (prompted by a play-test claiming the
> feel is wrong). The core reading below stands: the mechanic is **pure uniform
> input lag** — `effective godir = the RESOLVED direction from ceil(delay/50)=5
> ticks ago, neutral (-1) samples included`. Every alternative was checked
> against the code and REJECTED: it is **not** skid/coast (releasing does not
> "keep" the old dir while a fresh press takes effect early), **not** turn-only
> inertia, **not** joystick smoothing. Two things the fast pass under-stated,
> now nailed, plus one outright error corrected — see the "aliasing", "ruled
> out", and "Cold-start" notes below. No arithmetic change to the port; it was
> already byte-faithful. Frontend wiring (map-select → `level_index` → ice)
> re-verified end-to-end: no off-by-one, ice fires on the real Hockey Rink.

VALUELST ids 449-460 ("these are the 'ice delay' values (how much the
controls are slowed by the presence of ice on each level). This is measured
in milliseconds"). id 449 is a documented sentinel ("just to prevent invalid
access in case the net screws up...") that the confirmed call site never
reads (see below) — the real per-level block is **450-460**, one per stage,
same index order as the ice-delay comments themselves (`450 = "new
traditionalist"`, `451 = "classic green acres"`, `452 = "hockey rink"`, …
`460 = "inner city trash"`) and as `libs/game/src/game_app.cpp`'s own
`level_fallback` table. Shipped values: **all 0 except id 452 = 250** (level
index **2**, Hockey Rink).

**Call site — inside `sub_41F29B`** (the per-player-per-tick updater,
pseudo.c 23058-23078; NOT a separate function):

The whole block is guarded by "this player's type byte at **+16** is NOT 1",
i.e. it runs for every non-computer player and is skipped entirely for an AI.
Inside the guard, over the player's own 30-slot history buffer (the
per-player array `dword_4621C8[playerIndex]`, each slot a PAIR of words: an
even AGE word and an odd DIRECTION word), in this exact order:

1. **Age**: every one of the 30 slots' age words is increased by the frame
   delta `dword_464958`.
2. **Shift**: slots 29 down to 1 each copy BOTH words from the slot below
   them (slot k ← slot k−1), oldest first, so the ring shifts one position
   toward "older".
3. **Push**: slot 0's age is set to 0 and slot 0's direction is set to the
   player's CURRENT resolved effective godir — the word at player **+46**,
   `-1` (none) or `0..3` (see the aliasing note below).
4. **Resolve**: walk k = 0..29 from freshest to oldest, assigning the
   player's **+46** word from slot k's direction each iteration, and STOP at
   the first k whose age has reached the level's delay
   (`getvalue(dword_46499C + 450) <= age[k]`).

When the walk finishes, player **+46** holds the delayed direction the mover
`sub_41EC84` reads for the rest of that tick.

**Aliasing the fast pass under-stated: the buffer stores the FULLY-RESOLVED
effective godir at player +46, not a separate raw-input field.** The player
record is typed as a 16-bit-element pointer in this function (pseudo.c line
22852), so what looks like "the dword at element 11, shifted right 16" (the
value pushed at 23070, and the `!= -1` test at 23040/23082) is the
sign-extended 16-bit field at byte offset **+46** — i.e. **exactly the
effective-godir word**, resolved _after_ the opposite-key filter (the tail of
`sub_41E61E`) and the reversed-controls flip (23049). So the delayed samples
carry the reversed value (as the "Reversed-controls application point" note
already asserted), and on a delay-0 level the resolve's k=0 iteration writes
**+46** straight back — a genuine no-op that leaves the flip intact. The
break test reads slot k's AGE word (Hex-Rays lost the induction pointer and
flags the local "possibly undefined" at 0x41FCDE, but the shift loop's own
explicit slot-pointer arithmetic — take the address of slot k, write the
direction at its second word — pins the EVEN word as the age and the ODD word
as the direction).

**Ruled out — why it is (a) uniform lag and not skid/coast/inertia.** The
resolve loop UNCONDITIONALLY overwrites **+46** with slot k's direction and
never returns
the live input: at k=0 the age is 0, so `delay(250) <= 0` is false and the walk
always continues to k=5. There is no "if current input is non-neutral, keep it"
branch anywhere — a fresh press is delayed by the same 5 ticks as a release, so
pressing a NEW direction does NOT take effect early (rejects skid/coast) and
stopping is NOT instant (rejects turn-only inertia). When the delayed sample is
`-1`, the mover's idle branch (23413) makes the player STAND STILL (or, if a
conveyor is under it, ride the belt) — it does not coast the last direction.
Note the mover `sub_41EC84` itself has NO ice/slide logic (same corner-slide
stepper on every board), and `getvalue(450+level)` is read at exactly one site
(23074): the input-lag buffer is the WHOLE ice mechanic — there is no per-tile
"ice actor". (What DOES read as "slippery": on RELEASE the player keeps moving
for 5 ticks before stopping — uniform lag's delayed-stop half is itself the
coast/skid feel; the delayed-start half is the "unresponsive" half. Both are
faithful.)

The guard's player-type byte at **+16** (value 1) is the SAME player-type byte
already pinned
elsewhere in this file and in `simulation.cpp`'s own AI-dispatch comment
("the original calls `sub_40A1C6` instead of reading DirectInput …, gated on
the +16==1 tag") — **1 means computer-controlled**. So the WHOLE history
buffer push+resolve is skipped for AI players: an AI's desired direction
reaches the mover UNDELAYED even on Hockey Rink. Human (and would-be network)
players are the only ones subject to the lag.

**The math is a plain FIFO ring buffer, not a physics/friction change.** At
the locked 20 Hz tick rate (`dword_464958 == dword_46494C == 1000/getvalue(30)
== 50` ms/tick, confirmed cadence unit shared with `enclosure.md` §3's 250ms=
5-tick derivation), "age every slot by the frame delta, then shift" collapses
to a plain per-tick FIFO push: after the push, the sample now sitting at
ring-buffer index `k` is exactly `k` ticks old (`k * 50` ms). The resolve loop
then walks from the FRESHEST sample (k=0) toward the OLDEST, returning the
first whose age has reached the level's delay — i.e. the smallest `k` with
`k*50 >= delay_ms`, i.e. `ceil(delay_ms / 50)`. For Hockey Rink (250 ms):
`k = 5` — the player's movement this tick uses the direction it WANTED
exactly 5 ticks ago, not the current one. For every other level (0 ms):
`k = 0` always immediately satisfies the break — the "delayed" sample IS the
fresh one, i.e. **zero effective delay**, functionally identical to no buffer
at all.

**Port** (`MovementSystem::ice_delay`, `libs/sim/src/systems/movement.hpp/
.cpp`; `Player::ice_history`, `libs/sim/include/bomber/sim/player.hpp`):
`Tuning::ice_delay_ms[11]` (ids 450-460), indexed by the SAME `Tuning::
level_index` the regen mechanic uses. `Player::ice_history` is a 30-entry
`std::int8_t` ring (mirrors the original's 30-slot capacity; `-1` = no
direction, `0..3` = the godir) pushed/read by `MovementSystem::ice_delay`,
called from `simulation.cpp`'s `player_turn` right before the movement
block, replacing `want_godir`/`moving`/`want` with the delayed values for
the REST of that tick's turn (movement AND the walked-into-bomb kick check —
the original has only the one resolved **+46** field downstream, no separate
raw/delayed split). `ice_delay` is safe to call unconditionally every tick
for every player: it returns the input UNCHANGED, without touching the
buffer, whenever the player is AI (`p.ai`) or the current level's delay is
`<= 0` — so `ice_history` stays a fixed, unwritten field (all `-1`, see cold-
start note below) on every level but Hockey Rink.

**Cold-start `-1` fill is FAITHFUL — the original DOES reset the buffer per
round (2026-07-10 correction).** An earlier note here wrongly claimed
`dword_4621C8` is a process-lifetime global with no per-round reset. It is
reset: `sub_4214BC` (pseudo.c 23880-23890) walks all 10 players × 30 slots and
writes `age = 0, dir = -1` into every one, and it runs from the per-round
match-setup sequence (pseudo.c 14788 — right after the level index
`dword_46499C` is resolved at 14751/14758 and alongside the stage/bomb/player
init calls `sub_422D3B`/`sub_426CDB`/`sub_424F5E`/`sub_40151B`). So a fresh
round starts with dir `-1` in every slot — exactly what `build_state`
(`setup.cpp`) fills. Consequence, matching in both: for the first
`ceil(delay/50)=5` ticks no slot has yet aged to 250 ms, so the resolve runs
off the end and returns the reset `-1` — the player is frozen for 5 ticks, then
the tick-1 input surfaces at tick 6 (pinned by `tests/test_ice.cpp` "delays a
human player's first step by exactly 5 ticks"). `0` is a valid godir ("Up"),
NOT a "no input" sentinel, which is why a plain zero-init would be a phantom
"Up" drift — but the original avoids that too, via the `-1` reset, so our fill
reproduces the original's own behaviour rather than diverging from it.

**Frontend wiring re-verified (2026-07-10).** Map-select (`present_map_select`)
cycles `selected_level_` over `-1`(RANDOM) then `0..getvalue(35)-1` (11 stages);
`GameApp::start_match` sets `cfg.tuning.level_index = stage` where `stage =
selected_level_` (or `match::pick_stage` for RANDOM, or the campaign stage) —
the SAME index space as the original's `dword_46499C`. Both the row NAME
(`getstring(150+n)` / `level_fallback(n)`) and the ice value (`ice_delay_ms[n]`
= `getvalue(450+n)`) are indexed by that one `n`, so the board labelled "Hockey
Rink" is index 2 and gets `ice_delay_ms[2]=250` with no off-by-one. Our
`level_fallback` order matches VALUELST.RES's own 450-460 ice-block labels
exactly (`0 new traditionalist … 2 hockey rink … 7 haunted house (= regen id
347) … 10 inner city trash`).

**GOLDEN: no impact, proved by running the full suite before/after.**
`Tuning::level_index` defaults to 0, whose `ice_delay_ms[0] == 0`, so
`MovementSystem::ice_delay` takes its early-return branch (buffer untouched,
input unchanged) on every existing scenario. `Player::ice_history` was a
NEW hashed field (added in the same commit as `State::regen_timer` above);
see `tests/test_golden.cpp`'s recapture note. New suite: `tests/test_ice.cpp`.

## `CFG.INI` / `soundonoff` — a THIRD config layer, boot-time only, N/A to port (2026-07-09)

A string-literal sweep flagged `aSoundonoff` = `"soundonoff"` (pseudo.c 1587)
read via `sub_41739C("soundonoff", buf)` inside `sub_42896E` (pseudo.c
28498-28556, already pinned by `docs/re/frontend-flow.md` "The boot LOADING
dialog" as the second boot loading-dialog phase, "Loading sound..."), then
fed to `sub_4272DD` at pseudo.c 28544-28546. Tracing what these actually
touch shows this is unrelated to `options.ini`'s 22-key table
(`docs/re/results-and-options.md` "The full options.ini key list"):

- **`sub_41739C`** (pseudo.c 18719-18788) is a distinct, primitive
  `key=value` line-scanner over **`CFG.INI`** — confirmed by its own debug
  format string, `"cfg_get_info: no cfg.ini file (CHCFG=='%s')"`
  (`exit_()`s if the file is missing), and an environment-variable override
  (`getenv("CHCFG")`) that can redirect the path. This is a DIFFERENT reader
  from `options.ini`'s `sub_406238`/`sub_405DE3` pair (which never call
  `sub_41739C` and vice versa) — two independent config files, not one file
  read two ways. `sub_41739C` has exactly four call sites, all inside
  `sub_41095A`'s boot-init chain (`docs/re/facts.md`'s "Presentation LCGs
  must reseed" section already cites this same function for its RNG-seed
  read): `aNetonoff` ("netonoff"), `aHdhome` ("hdhome"), `aCdhome`
  ("cdhome"), `aDebug` ("debug"), and `aSoundonoff` ("soundonoff") — i.e.
  CFG.INI carries install-path/hardware-detection state (`hdhome`/`cdhome` —
  the local/CD install roots) and boot-time feature gates (`debug`,
  `netonoff`, `soundonoff`), not gameplay options.
- **The shipped install's own `CFG.INI`** (read-only reference, never
  committed) confirms the exact key set and a header comment naming its
  generator: `; created by E:\MAKECFG.exe` / `hdhome=...` / `cdhome=...` /
  `; debug=3debug.log` (commented out) / `soundonoff=1` / `netonoff=1` — a
  separate installer tool (`MAKECFG.EXE`, already listed as a non-gameplay
  tool in `docs/re/coverage-audit.md`'s Executable/DLL row) writes it once at
  install time; the game only ever reads it.
- **`soundonoff` gates whether the audio HAL initializes AT ALL, not a
  per-session mute.** `sub_4272DD(parsed_value)` (pseudo.c 27604-27616): if
  truthy, it wires the mixer callbacks (`sub_4187ED`), reads a THIRD config
  file (`aSoundIni` = `"sound.ini"`, a low-level DOS audio-driver settings
  file — port/IRQ/DMA-era, distinct from both CFG.INI and options.ini), and
  calls `sub_418FF7(handle, 16, 0x8000, 0x8000, 22050)` — a 16-bit/22050 Hz
  driver bring-up call. If falsy, none of that runs — the whole sound
  subsystem never comes up, unlike the Options screen's "Disable music
  during gameplay" (`disable_game_music`, `docs/re/results-and-options.md`
  §3 row 13), which only skips the gameplay MUSIC track and leaves SFX/the
  driver itself running.

**Resolves `docs/re/coverage-audit.md` row #134's open question** ("verify
CFG.INI/nodename.ini are netplay-only before ignoring outright") —
**CFG.INI is NOT netplay-only.** `netonoff` is, but `hdhome`/`cdhome`/
`debug`/`soundonoff` are read unconditionally at every boot, local or
networked.

**N/A to port.** This is DOS-era install/hardware bring-up: an env-var
overridable path, an installer-generated ini, and a driver-init gate with no
analogue in a modern SDL3 backend (`AudioEngine::init` brings up SDL's audio
device unconditionally; there is no DOS driver layer to conditionally skip).
`assets::Options` correctly models only `options.ini`'s 22 keys — adding
`soundonoff` there would misplace it in the wrong config layer. No code
change; this section exists to pin the fact and close the coverage-audit
question with evidence instead of leaving it open.

(Provenance: `sub_41739C` @ 0x41739C pseudo.c 18719-18788 [4 call sites:
11505, 14615-14617, 28544]; `sub_4272DD` @ 0x4272DD pseudo.c 27604-27616;
`aSoundonoff`/`aSoundIni` pseudo.c 1587/1572; `sub_42896E` @ 0x42896E
pseudo.c 28498-28556 [already cited by `docs/re/frontend-flow.md`]; shipped
`CFG.INI` content, install root, 2026-07-09; `docs/re/coverage-audit.md` row
134 [`.INI` files]; `docs/re/results-and-options.md` "The full options.ini
key list" [confirms `soundonoff` is absent from that table, correctly].)

## ANI sequence-name audit — CONFIRMED (MASTER.ALI + `sub_41D695`/`sub_41D957`, 2026-07-09)

Full systematic parity pass over every sequence name the original composes
vs every name our `libs/game` requests; the complete truth table lives in
**`docs/re/sequence-map.md`**. The load-bearing mechanism fact: the boot
loader `sub_41D695` (0x41D695) reads `DATA/ANI/MASTER.ALI`, loads every
listed `.ANI` into ONE global name-sorted sequence pool (`dword_461B5C`,
`qsort`+`stricmp`), and `sub_41D957` binary-searches that pool — so a file
absent from (or `;`-commented in) MASTER.ALI contributes nothing, and
same-named sequences in unlisted files are dead art. That single fact
produced seven fixes: punch pose is PUNBOMB1-4.ANI `"punch <dir>"` (not
PUNCH.ANI's `"punch <dir> green"`); the pickup pose (PUP1-4.ANI
`"pickup <dir>"`, `sub_41F29B` action-state 4) was missing entirely; the
trigger bomb (and the main menu's cursor) is TRIGANIM.ANI's 19-step
`"bomb trigger green"` (TRIGBOMB.ANI is commented out); jelly bombs have
their own BOMBS.ANI `"bomb jelly green"` wobble (`sub_42331C`'s
`"bomb %s green"` over kinds regular/trigger/jelly); flames are MFLAME.ANI's
5-step cycles (FLAME.ANI is commented out); the scheme editor's `'0'`
tileset toggle's `-1` state resolves EDIT.ANI's `"tile -1 blank/brick/
solid"` (not a dead state); and the campaign rover/ghost hazards ship as
ALIENS1.ANI `"ghost <dir>"`/`"rover <dir>"` (the earlier "cut content"
conclusion looked for a GHOST/ROVER.ANI filename that never existed).
Original-only leftover: `"kface %s"` (KFACE.ANI, `sub_41F29B` ~23272, gated
on `dword_45BE3C` — a net/AI player-highlight marker, default −1, not
ported). All presentation-layer; sim/golden untouched. (Provenance: shipped
`MASTER.ALI` text; `abtool ani` dumps of all 95 `DATA/ANI` files;
pseudo.c cites in sequence-map.md.)

## Explosion/tile-crumble draw fidelity audit (2026-07-10, `sub_426D06`/`sub_42A191`/`sub_41DB41`/`sub_42331C`)

User report after the flame-system sim audit landed: the tile-explosion visual
sequence "doesn't look right" vs the original when a bomb destroys bricks
(plus a follow-up report of the flame effect visibly drifting off-tile). The
SIM side (deferred chain reactions, brick stays `Cell::Brick` through the
crumble, powerup reveal timing) was already deep-audited in the two entries
above and is NOT re-litigated here — this pass started scoped to `libs/game`'s
draw pacing/sequencing/order, six sub-questions, each independently verified
against pseudo.c and (where cited) empirical `abtool ani` dumps of the shipped
install. One finding (§3) turned out to need a small, deliberately-scoped
`libs/sim` change (a new hashed field) to fix faithfully rather than with a
presentation-only approximation — done on `main`'s explicit decision, with
full golden-discipline recapture; see that section for the boundary and the
proof.

### 1. Brick-tileset "stage" argument — CONFIRMED fixed per level, does not advance with burn age

`sub_426D06`'s brick-burn branch (kind == 9, pseudo.c 27404-27415) composes the
sequence name with the string formatter `sub_4518D0`, given a destination
buffer, the format `aFlameSU`, and the piece name `off_45BEA0[9]`, where
`aFlameSU` = `"flame %s %u"` (pseudo.c 1569) and `off_45BEA0[9]` = `"brick"`
(pseudo.c 2284-2296, the same 10-entry piece-name table `sub_426D06` also uses
for kind 0-8's `"flame %s green"`/`off_45BEA0[k]` = tipnorth/tipeast/tipsouth/
tipwest/midnorth/mideast/midsouth/midwest/center).

Decisive evidence is empirical, not the (partly corrupted — see the caveat
below) pseudocode: `abtool ani` dumps of the shipped `XBRICK0/1/5/10.ANI`
(extended in this pass to also print each step's per-STAT `dx/dy`, see §4)
each contain **exactly one** sequence, named `"flame brick <n>"` where `<n>`
is that FILE's own level index, with 9-10 frames baked into that one sequence:

```
XBRICK0.ANI:  seq 'flame brick 0'  (9 steps)
XBRICK1.ANI:  seq 'flame brick 1'  (9 steps)
XBRICK5.ANI:  seq 'flame brick 5'  (9 steps)
XBRICK10.ANI: seq 'flame brick 10' (10 steps)
```

This is structurally identical to the already-confirmed `"tile %u solid"`/
`"tile %u brick"` per-level tileset lookup (`sub_425D22`, `dword_46499C`,
`docs/re/sequence-map.md` row 58): `<stage>` is the FIXED per-level tileset
index, resolved once, exactly matching `SequenceSet::resolve_stage`'s existing
`"flame brick " + n` (`libs/game/src/sequences.cpp`). It does **not** advance
with burn/flame age — `sub_426D06`'s kind-9 branch never reads the flame
cell's elapsed-ticks pair (**+66**/**+68**) when composing the name; that
pair is only read
afterward to decide when the crumble EXPIRES (`docs/re/facts.md` "Brick
crumble timing"), not to pick a different sequence mid-burn. The 9-10-frame
crumble ANIMATION plays out entirely within that ONE sequence, via the frame
COUNTER (see §2), not by swapping sequences.

**Caveat (does not change the verdict):** the exact register/stack mechanics
of the `%u` substitution at this one call site are not cleanly recoverable
from the decompile. `sub_4518D0` is a cdecl vararg-forwarding helper (its
body takes the address of its own third parameter as the start of the
variadic list, pseudo.c 56872-56883) whose OWN reconstructed 3-parameter
prototype undercounts what this call site actually pushes: a local set to
`dword_4648A0 / 2` is assigned immediately before EVERY `sub_4518D0` call in
this function — including the kind-0-8 branch's single-`%s`
`"flame %s green"`, where it is set to a dead-looking `0` — and never
referenced again in the visible pseudocode, the classic signature of a hidden
4th stack argument that Hex-Rays' 3-param signature dropped from view.
Hex-Rays itself flags the immediately-following code as corrupted (a
"possibly undefined" warning at 426EB9, and the same for four further locals
in that region) — a cascading stack-tracking failure typical of an
arity-mismatched cdecl call. `dword_4648A0` is independently confirmed elsewhere in this same
function's file to be a FIXED tile-geometry constant (`sub_42647A`:
`dword_4648A0 = 36`, one of the field-geometry globals set once at match
setup — CONFIRMED, this is tile height in px, paired with `dword_4648A8` in
`sub_42655F`'s row->Y formula), not a level or burn-progress variable — so
even under the "hidden 4th arg" reading, whatever literal `%u` decodes to
doesn't matter for THIS question: it is provably level/age-independent within
one call, and the file-content check above is the authoritative, decisive
evidence regardless of how that one register is actually populated. Not worth
chasing further given the acknowledged decompiler corruption in this exact
spot.

**Verdict: no fix.** `SequenceSet::resolve_stage`'s architecture (one sequence
resolved once per level) is structurally correct.

### 2. Flame/burn frame pacing — CONFIRMED bug, FIXED

`sub_426D06` drives the DISPLAYED FRAME of both real flame (kind 0-8) and
brick-burn (kind 9) off one per-flame-cell field, `+48` (a `WORD`), via the
same accessor as every other animated entity in the game,
`sub_41DAA7(seq, counter) = counter % statecnt` (already CONFIRMED general
convention, `docs/re/facts.md` "ANI per-step timing", `anim_pace.hpp`).

`+48` is explicitly zeroed at ignition (`sub_426FCC`, the ignite call, writes
0 into the flame cell's **+48** word at pseudo.c 27496 — on EVERY ignite,
fresh or a re-trigger mid-crumble, matching the already-confirmed "the
crumble timer resets" behaviour) and is advanced by a small pacing loop at
the bottom of `sub_426D06` (pseudo.c 27456-27457). That loop is the standard
ms-accumulator idiom used everywhere in the engine, over the flame cell's own
two words:

- **+50** (the ms accumulator, read/written as a signed 16-bit value) is
  first increased by the frame delta `dword_464958`;
- then, for as long as **+50** is still `> 0`, the frame counter **+48** is
  incremented by 1 and **+50** is decreased by the per-tick quantum
  `dword_46494C`.

`docs/re/facts.md`'s own "Ice / input-lag" entry already established
`dword_464958 == dword_46494C == 1000/getvalue(30) == 50` ms/tick at the
locked 20 Hz rate — so this loop's body runs **exactly once per call**: the
frame counter `+48` increments by exactly 1 every tick, for as long as the
cell stays active. It is free-running, decoupled from BOTH the sequence's own
step count AND the cell's total lifetime (`brick_burn_frames`/`flame_frames`)
— `sub_41DAA7` just wraps it `% statecnt`, cycling the art as many times as
the tick count divides into it (e.g. MFLAME's 5-step cycles loop TWICE over a
10-tick flame life).

Our port's `Renderer::timed_step()` did something structurally different: a
linear one-shot rescale, `idx = (total-remaining) * steps.size() / total`,
stretching the WHOLE sequence to play exactly once end-to-end over the cell's
total lifetime. For a 5-step flame cycle over 10 ticks this halved the true
frame rate and never looped (vs. the original cycling twice); for a 9-10-step
brick crumble over a 10-tick burn the step/tick counts are close enough that
the divergence is smaller but still not the same formula (and not something
to assume stays benign if `brick_burn_frames`/art frame counts ever change).

**Fixed:** both call sites (brick-burn draw, flame-arm draw, in
`Renderer::draw_world`) now feed `elapsed = total - remaining` directly into
the existing `anim_step_index()`/`draw_anim` `% statecnt` wraparound — no
rescale — reproducing the `+48` counter exactly (both start at 0 on
ignition/re-ignition, both add 1/tick). `Renderer::timed_step()` is removed
(dead code, no remaining callers) from `renderer.cpp`/`renderer.hpp`.

Presentation-only: `s.flame`/`s.burning` countdown semantics (and everything
that gates on them) are untouched — only how the renderer maps a remaining-
ticks value to a displayed frame index changed.

### 3. Flame arm-shape (tip/mid/center) selection — CONFIRMED divergence, FIXED (sim-side, `main`'s decision)

`sub_42331C`'s per-explosion arm-cast loop (pseudo.c ~25619-25678) decides
each flame CELL's "kind" (the same `off_45BEA0` index `sub_426D06` later
reads) **once, at ignition**, purely from cast-time geometry:

- the epicentre tile always gets kind 8 ("center") — pseudo.c 25625.
- each of the 4 arms (`k` = 0..3, one direction) steps outward tile by tile
  (`m` = 0..reach-1):
  - hits a bomb or a player: chain-queue/reveal, arm STOPS (`break`) —
    pseudo.c 25642-25664.
  - hits a solid wall (cell type 1): arm stops, nothing ignited — pseudo.c
    25663-25664.
  - hits a brick (cell type 2): kind 9 ("brick"), arm stops — pseudo.c 25667.
  - open floor: `if (m == reach-1) kind = k /* a TIP piece */; else kind = k+4
    /* a MID piece */` — pseudo.c 25673-25677 — i.e. the LAST tile of the
    arm's own FULL CONFIGURED reach gets a tip (tipnorth/east/south/west),
    every earlier tile of that SAME arm gets that SAME direction's mid piece
    (midnorth/east/south/west) — never the other axis's symmetric twin.

This is a static, per-arm, per-direction decision fixed at cast time. It does
**not** consult what's currently lit in a neighbouring tile, and each arm
owns its own tiles' mid piece unambiguously (every tile of the west-cast arm
is `"midwest"`, never `"mideast"` — no symmetric choice to make).

Our renderer (`Renderer::draw_world`'s flame-piece selection) instead does a
LIVE, per-frame scan of `s.flame[][]` on the four orthogonal neighbours, and
breaks the tie between the same-axis symmetric pair (`mid_h[0]`=midwest vs.
`mid_h[1]`=mideast, `mid_v[0]`=midnorth vs. `mid_v[1]`=midsouth) with
`(x + y) & 1` — an arbitrary checkerboard parity with no relationship to
which side of the epicentre a tile is actually on. Two concrete, unfixed
consequences:

1. Every mid tile's west/east (or north/south) sprite choice is effectively
   checkerboarded instead of a clean "west half of the epicentre draws
   midwest, east half draws mideast" split the original produces.
2. An arm cut short early by a brick/solid/bomb/player renders its last live
   tile as a MID piece in the original (the tip designation only applies at
   the arm's FULL, uninterrupted configured reach) but our live-neighbour
   scan renders it as a TIP (nothing lit past it) — backwards whenever an arm
   doesn't reach its full length, which is common (any bomb near a wall or
   another bomb).

**Fixed (sim-side, per `main`'s explicit decision — this crosses out of the
original `libs/game`-only scope, so it was escalated rather than done
unilaterally; see the session report).** A faithful port needs the CAST
DIRECTION and tip/mid-ness recorded per flame cell at ignition — a new piece
of per-cell data alongside the already-hashed `flame_owner`. Added
`FlameKind` (`libs/sim/include/bomber/sim/types.hpp`), an enum whose integer
values mirror `off_45BEA0`'s order 1:1 (0-3 tips, 4-7 mids, both in compass
order, 8 = center), and a new hashed `State::flame_kind` grid
(`state.hpp`, next to `flame_owner`). `FlameSystem::ignite_epicentre` sets it
to `Center`; `FlameSystem::spread_to` gained an `is_last_of_reach` parameter
(computed by its one caller, `explode`'s arm loop, as `i == reach` — the
SAME "last tile of the FULL configured reach" test as pseudo.c's `m ==
reach-1`, just 1-indexed instead of 0-indexed) and sets
`godir(from_dir) + (is_last_of_reach ? 0 : 4)` on the "arm continues" path
only — mirroring pseudo.c 25673-25677 exactly, including the cut-short-arm
behaviour (a tile that stops the arm early, or the tile right before an
obstacle, gets whatever the loop naturally assigns it — a MID, since
`is_last_of_reach` is false there — never retroactively upgraded to a TIP).
`Renderer::flame_piece` (`libs/game/src/renderer.cpp`) replaces the live
neighbour-scan with a plain switch from `FlameKind` to the matching
`FlameSet` member — no more `(x + y) & 1` checkerboard.

**Golden impact: hash-layout growth, not a behaviour change.** `flame_kind`
is pure derived data — computed from the already-existing `from_dir`/`reach`
inputs at ignition, no new RNG draw, no new branch that changes what ignites
or when. It's folded into the SAME packed per-tile hash word `cells`/
`hidden`/`floor`/`flame`/`burning`/`flame_owner` already share (bits 48-55,
previously unused padding in that word — `hash.cpp`), so this is a one-time
constant-shift for every scenario with an explosion, exactly like the
`next_bomb_id`/`Bomb::id`/`regen_timer` precedents above. Recaptured
`tests/test_golden.cpp`'s hash constants for the affected scenarios in the
same commit; every non-hash assertion (RNG streams, bounce counts) is
unchanged — see that file's own updated comment for the specific proof run.

### 4. Flame draw offset (position drift) — CONFIRMED bug, FIXED

`docs/formats/ani.md`'s general rule (confirmed 2026-07-04): the per-STAT
FRAM-leaf `offset_x/offset_y` (`assets::ani::SeqStep::dx/dy`) is parsed but
**not** applied by the standard blit (`sub_415920`/`sub_415A9F` take only the
frame index) — "the offset getter `sub_41DB41` is a separate, rarely-used
path."

`sub_426D06` is that rare path's (only confirmed) caller, and only on its
REAL-FLAME branch (kind != 9, pseudo.c 27438-27445). The loop sets a flag iff
the flame cell's kind is NOT 9 (i.e. a real flame-arm piece), and the two
sides of that flag differ exactly as follows:

| branch | offset fetch | X passed to the blit `sub_415A9F` |
|---|---|---|
| real flame (kind != 9) | calls `sub_41DB41(seq, frame, &out_dy, &out_dx)`, where `frame` is the u16 animation counter at flame-cell **+48** | the base tile-column anchor `sub_426524(col)` **plus** the fetched dx |
| brick-burn (kind == 9) | no `sub_41DB41` call at all | the base tile-column anchor `sub_426524(col)` alone |

Both sides call the row anchor `sub_42655F(row)` for Y before the blit; how
the two Y terms differ is the "blit math" below.

(`sub_41DB41(seq, frame, &out_dy, &out_dx)`, pseudo.c 21840-21866, reads the
resolved frame record's offset+4/offset+8 pair — i.e. the file's
`offset_x`/`offset_y` — matching `SeqStep::dx/dy`.) So: **real flame arms are
the one exception that folds the per-STAT `dx/dy` into the blit position;
brick-burn (and every other sequence in the game) ignores it, per the general
rule.**

Empirically confirmed non-trivial: `abtool ani` (extended this pass to print
each step's `dx/dy` — `apps/abtool/commands.cpp`) on the shipped
`MFLAME.ANI` shows every one of its 9 flame-piece sequences carries non-zero
per-step offsets, e.g.:

```
seq 'flame center green'   (5 steps): 0(3,16) 1(2,16) 2(2,16) 3(2,16) 4(2,16)
seq 'flame tipnorth green' (5 steps): 5(-1,16) 6(-1,16) 7(-1,16) 8(-1,16) 9(-1,16)
```

(format: `frame(dx,dy)`) — `dy` sits around 8-16 px on every sequence, against
a 40px-tall tile: not a dormant field, the shipped art actively relies on it.
The dead-art `FLAME.ANI` (pre-2026-07-09-audit source file) carries dx/dy of
similar magnitude (`"flame center green"` dy=17) — so this is **not a
regression introduced by the FLAME.ANI->MFLAME.ANI sequence-source fix**;
it's a pre-existing gap in how flame specifically is drawn that the file
swap didn't touch either way. `XBRICK*.ANI`'s `"flame brick <n>"` steps also
carry non-zero dx/dy (e.g. XBRICK10: `dx=3,dy=-1` on every step) — correctly
never applied, per the kind==9 branch's confirmed no-`sub_41DB41`-call.

**Blit math — RESOLVED by direct disassembly (2026-07-11).** The Y term sits
in the same corrupted stack region as §1's caveat (Hex-Rays flags the locals
that feed it "possibly undefined" at 426F46), so the whole Y computation was
invisible to the decompile. Raw disassembly of the real-flame branch
(`BM95.EXE` 0x426ee7-0x426f46, imagebase 0x400000) recovers it
instruction-for-instruction. In order:

| addr | what happens |
|---|---|
| 426ee7 / 426eed | load the addresses of the two stack out-slots for the offset getter: the slot that will receive the SECOND out value into `ecx`, the slot for the FIRST into `ebx` |
| 426ef8 | frame index ← the u16 at flame-cell **+48** (the free-running anim counter) |
| 426eff | call `sub_41DB41`; it stores the resolved frame record's **+4** into the `ebx` slot (= dx) and its **+8** into the `ecx` slot (= dy) |
| 426f0c / 426f0f | reload the frame handle and the row index `i` |
| 426f12 | call `sub_42655F(i)` → `Y_base` = tile_top + tileH − 1 (i.e. tile BOTTOM) |
| 426f17 / 426f1d | Y accumulator ← dy, then ADD `Y_base` ⇒ `dy + Y_base` |
| 426f1f / 426f2a | load tile height from `dword_4648A0` and halve it (the signed-divide-by-2 idiom) |
| 426f31 | SUBTRACT tileH/2 from the accumulator ⇒ `dy + Y_base − tileH/2` |
| 426f33 / 426f35 | move it into the Y argument register and add this branch's extra local term — which is **0** on the real-flame branch (set at 426e35) |
| 426f38 / 426f3b | load the column index `j`, call `sub_426524(j)` → `X_base` = tile_left + tileW/2 |
| 426f40 | X argument = `X_base` + dx |
| 426f46 | call the blit `sub_415A9F(X, Y, palette, frame)` |

So, before the blit's own hotspot subtraction (dx/dy applied **BEFORE**
hotspot — they are added to the coordinate *passed* to `sub_415A9F`, whose
frame later renders at `pos - hotspot`, exactly like our `draw_sprite`):

```
X_blit = sub_426524(j) + dx            = X_base + dx
Y_blit = sub_42655F(i) - tileH/2 + dy  = Y_base - tileH/2 + dy
```

Two corrections to the earlier symmetry inference: (a) the Y **sign was
right** — dy is *added*, same as dx (the accumulate at 426f1d is an add); but
(b) the inference **missed the `- tileH/2` anchor shift** entirely, because
that block was the corrupted region. `sub_42655F` returns tile-BOTTOM
(`tileH*i + tileH-1 + originY`, == our `sy`); real flames subtract half the Y
tile stride (`dword_4648A0/2`, the same global our `kTileH` mirrors) to
re-anchor to tile CENTRE, *then* add the per-STAT dy. Brick-burn (kind 9,
0x426f4d) uses `Y = sub_42655F(i)` raw — no `-tileH/2`, no dx/dy — matching
its no-`sub_41DB41` path. (Aside: the kind-9 setup computes tileH/2 into a
local at 0x426dac but never uses it in its own blit — a dead assignment; the
live `-tileH/2` is the explicit subtract at 426f31 on the flame branch.)
`sub_41DB41` writes the frame record's **+4** to its `ebx` out-param and its
**+8** to its `ecx` out-param; the call passes the dx slot in `ebx` and the
dy slot in `ecx`, and the X argument consumes the dx slot while the Y
argument consumes the dy slot — so **rec+4 = dx (X), rec+8 = dy (Y)**,
matching `SeqStep::dx/dy`.

**Arithmetic sanity check** (MFLAME.ANI, `abtool ani`; tile top-left at
`(Ox,Oy)`, `kTileW=40`, `kTileH=36`, so `kTileH/2 = 18`):

*Center piece* — frame 0 `C_F_1.LBM` `41x37 hot(20,36)`, seq
`flame center green` step 0 `dx=3, dy=16`. Our anchors: `sx=Ox+20`,
`sy=Oy+35`.
- X = sx+dx = Ox+23 → rect.x = X-hx = Ox+3 → spans `Ox+3 .. Ox+44` (w=41)
- Y = sy + (dy - 18) = Oy+33 → rect.y = Y-hy = Oy-3 → spans `Oy-3 .. Oy+34` (h=37)
- sprite centre ≈ `(Ox+23.5, Oy+15.5)` vs tile centre `(Ox+20, Oy+18)` — a
  deliberate ~3px right / ~2px up bias (flames lean up), landing squarely on
  the 40x36 tile.

Contrast the three states for the *horizontal-arm* case, frame 20
`R_F_1.LBM` `34x22 hot(17,21)`, seq `flame tipeast green` step 0 `dy=8`
(rect.y = `sy + dy - 18 - hy`):
- **pre-dx/dy port** (no offset): `Oy+35-21 = Oy+14`, centre `Oy+25` → **7px
  below** tile centre (the original drift complaint).
- **first dx/dy fix** (dy, no `-tileH/2`): `Oy+35+8-21 = Oy+22`, centre
  `Oy+33` → **15px below** (the user-reported worsened downward shift).
- **this fix** (`dy - tileH/2`): `Oy+35+8-18-21 = Oy+4`, centre `Oy+15` →
  ~3px above centre, correctly on-tile.

**Fixed:** `Renderer::draw_world`'s flame-arm draw (the one call site reading
`sp.dx`/`sp.dy`; `libs/game/src/renderer.cpp`) now passes
`draw_sprite(sp, sx + sp.dx, sy + sp.dy - sim::kTileH/2)`. The `Sprite` dx/dy
fields (`sprites.hpp`, populated by `resolve_sequence` from `SeqStep::dx/dy`)
are unchanged and stay inert for every other sequence — including this same
loop's brick-burn draw — preserving the general "ignore it" rule.

### 5. Draw order/composition — CONFIRMED bugs, FIXED

The original's per-frame update+draw entry point, `sub_42A191` (pseudo.c
29488-29556), issues (relevant subset, in order): actors (`sub_4056CA`),
**bombs** (`sub_4245B9` -> `sub_42331C`, which ticks AND draws each bomb
inline as part of its per-tick update), **powerups** (`sub_424F89`), ...,
**flame/brick-burn** (`sub_426D06`), ..., **players** (`sub_420F07`).

Our renderer's previous order was actors, powerups, then — all inside one
`draw_world` — cells/burn, flames, **bombs**, players: bombs drawn AFTER
powerups and AFTER flame/burn, backwards on both counts relative to the
original. This only produces a visible difference where a bomb spatially
shares a tile with a powerup or an active flame cell in the same frame:

- a bomb dropped on a floor-powerup tile: original draws the powerup ON TOP
  of the bomb; ours drew the bomb on top, hiding the powerup.
- the one-tick window where a bomb sits in an already-flaming tile before a
  deferred chain reaction detonates it (`docs/re/facts.md` "Chain-reaction
  timing" — one link per tick): original draws the flame over the bomb; ours
  drew the bomb poking out over the flame.

**Fixed:** the bomb-drawing loop is extracted out of `draw_world` into its own
`Renderer::draw_bombs`, called from `draw_frame` between `draw_actors` and
`draw_powerups` (actors -> bombs -> powerups -> flame/burn -> players).
`draw_world` keeps cells/burn, flames, players, rovers, deaths, and the
gold-twinkle overlay — the original doesn't re-order any of those relative to
each other, and rovers/deaths/twinkle have no original per-frame-list
equivalent to cite, so they stay at the end as before.

Separately (same investigation): the original's floor-powerup drawer
`sub_424F89` (pseudo.c ~26218-26266) gates its actual sprite blit on TWO
conditions ANDed together — the token record's state field (+0) must read 2
(visible) AND the cell-type read `sub_425FB9(j,i)` must return 0 (blank) —
i.e. the token's "visible" STATE
flip happens at ignition (already confirmed, "Brick crumble timing" above,
`sub_425107`), but the SPRITE is only actually drawn once the CELL reads
blank (`sub_425FB9(j,i) == 0`). Our `draw_powerups` had no such gate — it drew
any `s.floor[y][x] != PowerupType::None` unconditionally, so a token under a
still-crumbling brick would render (through/over whatever the crumble
animation draws on top of it, depending on art opacity) for the entire
`brick_burn_frames` window instead of staying invisible until the tile
actually opens.

**Fixed:** `Renderer::draw_powerups` now skips any tile where
`s.cells[y][x] != sim::Cell::Blank`, matching `!sub_425FB9(j,i)`.

All of §5 presentation-only: `libs/sim`/golden untouched (`s.floor`/`s.hidden`
state-flip timing is unchanged; only when/what the renderer draws changed).

### 6. Brick crumble duration (`brick_burn_frames`, VALUELST id 20) — RE-CONFIRMED, no change

Re-verified per this audit's suspect list (is the crumble-duration TUNING
value actually RE'd, or a leftover guess?): `Tuning::brick_burn_frames`
(`libs/sim/include/bomber/sim/tuning.hpp`) is already `10`, matching the
already-CONFIRMED shipped value from the "Brick crumble timing" entry above
(`getvalue(20)` = 10, the same shipped constant as `flame_frames`/id 10) and
`docs/valuelst-map.md`'s existing `"20 | brick disintegration animation,
frames | 10"` row. It is correctly RE'd already — no change made.

### Verification

§2/§4/§5/§6 and §1 (no code change) are render/asset-layer only —
`libs/sim`/`libs/match` untouched by them. §3 (arm-shape) is the one
deliberate `libs/sim` change in this pass, scoped exactly as described
there: one new hashed field, zero new RNG draws, zero changed branches in
anything that decides WHAT ignites or WHEN — only what gets recorded about a
tile that was already going to ignite. `apps/abtool/commands.cpp`'s `ani`
dump gained a `dx/dy` column (diagnostic only, prints our own already-parsed
struct fields — no exe-derived material). Full `headless` suite green
(37/37) both before and after the `libs/game`-only fixes; re-verified green
again (with recaptured golden constants) after §3; `windows-fetch`
(`libs/game`/`bomber_game`, the actual changed presentation code) built
clean.

Ported: `libs/game/src/renderer.cpp` (`draw_bombs` split out of `draw_world`,
flame/burn pacing, flame `dx/dy`, powerup cell gate, `timed_step` removed,
`flame_piece` replaces the live neighbour scan), `libs/game/include/bomber/
game/renderer.hpp` (`draw_bombs`/`flame_piece` declared, `timed_step`
removed), `libs/game/include/bomber/game/sprites.hpp` (`Sprite::dx/dy`),
`libs/game/src/sprites.cpp` (`resolve_sequence` populates them),
`apps/abtool/commands.cpp` (`ani` dump prints `dx/dy`); `libs/sim/include/
bomber/sim/types.hpp` (`FlameKind`), `libs/sim/include/bomber/sim/state.hpp`
(`State::flame_kind`), `libs/sim/src/systems/flames.{hpp,cpp}`
(`ignite_epicentre`/`spread_to` set it), `libs/sim/src/hash.cpp` (mixed in);
`tests/test_golden.cpp` (recaptured hash constants).

(Provenance: `sub_426D06` pseudo.c 27366-27463; `sub_426FCC` 27478-27504;
`sub_42331C` arm-cast loop pseudo.c ~25619-25678; `sub_41DB41` 21840-21866;
`sub_415A9F`/`sub_415920`/`sub_415B22` 18007-18119 [deferred draw-queue
primitives]; `sub_4518D0` 56871-56884; `sub_42647A` field-geometry init
27008-27029; `sub_42655F`/`sub_426524` 27030-27041/27038-27044; `sub_42A191`
29488-29557 [per-frame entry point]; `sub_4245B9` 25787-25792; `sub_424F89`
26218-26267; `off_45BEA0` piece-name table 2284-2296; `aFlameSU`/
`aFlameSGreen` 1569-1570; `abtool ani` dumps of shipped `MFLAME.ANI`,
`FLAME.ANI`, `XBRICK0/1/5/10.ANI`, 2026-07-10.)

## Walk pose keys off the DISPATCHED godir, not displacement (2026-07-12, `sub_41F29B` anim selection)

User live-comparison report against the real install (running natively, see
memory/original-game-runs-natively.md): in the original, a player (human or
AI) pushing into a wall keeps playing the walking leg animation — visible
"pedalling in place" — while our port froze to the stand pose. Confirmed as a
port deviation, root-caused, and fixed:

- **Pose selection**: the walk-vs-stand pick in `sub_41F29B`'s anim-state
  block (23080-23110; `walkbomb`/`standbomb` variants via +37 at 23088/23099,
  the cosmetic stand-frame pick at ~23086) keys off the player's dispatched
  direction word **+46 (godir)** — set by input/AI/ice-buffer each tick,
  -1 = idle — NOT off whether the position actually changed. The mover's
  budget loop spends 100/iteration **even when every pixel step is blocked**
  (`sub_41EC84`; our `MovementSystem::move` mirrors this), so the engine's
  own model is "walking, just not getting anywhere" and the leg cycle (the
  16.16 walk phase advanced by the tick's disease-scaled speed budget)
  advances regardless of the wall.
- **Conveyor corollary** (already pinned in the ice/stun audit, "+46 == -1
  takes the IDLE movement branch ~23413"): a belt carrying an IDLE player is
  the idle-on-conveyor branch — godir stays -1 → **stand pose sliding
  along**, no leg animation.
- **Cornerhead interplay**: the boxed-in fidget states 20-39 are entered off
  enclosure alone (23006-23013, before input acquisition) and exit only on
  anim-complete or the box opening — held keys do NOT cancel them, so a
  fully-enclosed player mashing into the walls fidgets rather than pedals.

**Port** (presentation-only; events are unhashed, golden untouched): the sim
emits a per-tick `Event::Type::PlayerWalking` (player, tile, data = the
disease-scaled walk budget in px) whenever the walking dispatch runs
(`simulation.cpp` `player_turn`, right after `eff_godir` resolves);
`Renderer::sample_movement` now derives the pose and the leg-phase advance
from that event instead of the previous position-delta approximation (which
froze the wall-pusher AND wrongly pedalled the belt-idle slider), and the
cornerhead draw override no longer requires "not moving". Perceptual side
note: this restores most of the original's characteristic AI "jitter" — its
AI constantly steers into walls/corners while re-deciding, which reads as
frantic leg-buzzing there and read as calm standing in our port. Tests:
`tests/test_move.cpp` "PlayerWalking event follows the dispatch, not
displacement".

(Provenance: the +78 state-machine audit's line cites [23006-23013,
23080-23110, 23413-23453] and the movement stepper's budget-loop fact, both
already in this file; live A/B observation vs the shipped original,
2026-07-12. The exact 16.16 phase-increment line in `sub_41F29B` was pinned
during the original stepper RE — the walk-phase field and its speed-driven
advance predate this entry; this entry corrects only WHERE the port sourced
the advance from.)

## Canonical frame cadence — the per-frame vs 50 ms two-clock model, PORTED (2026-07-12)

The original has TWO clocks, and the port previously collapsed both onto the
20 Hz tick:

1. **The 50 ms quantum** `[0x46494C] = 1000/getvalue(30)`: every anim/state
   counter advances through a per-entity ms accumulator
   (`acc += frameDelta; while (acc > 0) { acc -= 50; ++counter }` —
   sub_41F29B's +82/+80 idiom, sub_426D06's flame/burn pacing, etc.), i.e.
   fuses, flames, diseases, animations all quantize to 20 Hz **on average**
   regardless of the display rate. Our tick-based counters model this layer
   exactly; nothing changed there.
2. **The display frame**: the gameplay driver `sub_42A191` is a per-DISPLAYED-
   frame callback in a DirectDraw flip loop (docs/re/in-match-shell.md), with
   the measured integer-ms delta `[0x464958]`. Input acquisition
   (sub_41E61E), the AI brain (sub_40A1C6 with all its whims/RNG draws and
   its `+= frameDelta` pursuit timers), the movement-budget accruals
   (`speed × frameDelta / 50` — player sub_41F29B, rover/ghost sub_401B5C,
   sliding bomb sub_42331C), and the head-stun `--+58` (22982-22984, a plain
   per-frame decrement, NOT accumulator-quantized) genuinely run at display
   rate — **60-70 fps on period hardware**, ~60 fps on the reference Win11
   install. The original's gameplay is therefore mildly frame-rate-dependent
   by construction; bit-exact parity with a live run is impossible in
   principle, so the port pins a CANONICAL display rate.

**Canonical rate = 60 fps**, expressed as the repeating integer-ms delta
pattern `{17, 17, 16}` (sums to the 50 ms tick; `constants.hpp kSubFrameMs`).
**[SUPERSEDED 2026-07-16 — canonical rate re-pinned at ~180 fps, nine
sub-frames `{6,5,6,5,6,5,6,5,6}`; see the UPDATE block at the end of this
entry. The two-clock model and everything else below stands; re-scale the
worked numbers (921→918 walk units, 267 ms→89 ms stun, 500 ms→~166 ms ice
span, 3×→9× decide rate) accordingly.]**
Ported consequences (all deliberate behaviour changes, goldens recaptured in
the same commit — golden A stayed byte-identical, pinning that the no-input
path is untouched; golden E's bounce-count + veer-RNG assertions passed
unchanged through the recapture):

- **Player pass** (`player_turn`): input decode + AI decide + ice-buffer push
  + budget accrual + per-pixel mover now run as three sub-frames per tick.
  Walk accrual keeps the original's truncation: `923×17/50 + 923×17/50 +
  923×16/50 = 313+313+295 = **921**` per 50 ms — the stock walker is sub-1%
  slower than the old flat 923, exactly as the original at 60 fps.
  **[VERIFY → RESOLVED 2026-07-16]** the molasses ÷3 / hyper ×3/2 factors
  multiply the SPEED before the delta division: pseudo.c 23432-23440 reads
  the speed term is built as `base + skates·gv(90) − clogs·gv(91)`, then
  divided by 3 if molasses is set, then replaced by `3·term/2` if hyper or
  super is set, and only AFTER that scaled by the frame delta
  (`term ← delta·term/50`) — factors first, delta
  scaling second (≤1 unit per frame difference, diseased players only). The
  port now matches (movement.cpp's accrual; test_move's molasses case pins
  the order).
- **AI cadence**: sub_40A1C6 fires per sub-frame — three decisions, three
  whim rolls, three wander re-rolls per tick, restoring the original's
  "frantic" temperature (the user-visible complaint that motivated this).
  Pursuit timers (+12/+28) accrue the ms delta and time out at
  `10 × [0x46494C]` = 500 ms wall clock (brain.hpp). The danger/obstacle
  grids stay cached per tick — the original rebuilds per frame, but bombs/
  flames are static between our sub-frames, so one build is identical.
- **Head stun**: `+58` burns once per frame → 16-frame head hit ≈ 267 ms
  (was 800 ms — 3× too long). Gate-then-decrement order preserved
  (`if (+58>0) { block input; --+58 }`).
- **Ice buffer**: pushed per sub-frame; the 30-slot history spans ~500 ms,
  its original capacity at 60 fps (was 1.5 s at one push per tick).
- **Rovers/ghosts** (`sub_401B5C`): `budget += speed×delta/50 + 100` per
  frame → the flat +100 term triples to +300/tick. Rovers/ghosts now
  visibly outpace a same-speed walker, as in the original (the old port ran
  them at a third of their real pace). Folding the three accruals into one
  pass is exact: the field is static during the rover pass, so the pixel
  sequence (and its centre-tile RNG draws) is unchanged by instalment size.
- **Conveyor term**: `getvalue(190+idx) × delta / 50` per sub-frame (exact
  totals for the shipped belt speeds, which are divisible). NOTE: the belt (and
  kicked) bomb slide has NO flat +100 bonus, unlike the rover above. The
  original's slide-budget block does add `+= 100`, but the two statements
  after it back the
  position off one direction step and the move loop spends that +100 undoing
  it — a wash (confirmed 2026-07-20 against the native oracle; an audit that
  briefly folded a `+100 × kSubFrames` bonus here was reverted). The rover's
  `sub_401B5C` +100 is real ONLY because that function has no such backoff.

**Deliberately still tick-quantized** (each ≤50 ms of phase, invisible, and
kept to bound the blast radius): the bomb-action tail (23277-23380) runs once per
tick with the AI's action-key presses OR-latched across its sub-frames (all
four blocks are edge-gated, so only the auto-drop diseases' intra-tick
attempt density differs); HUMAN direction sampling stays one sample per tick
(the shell's per-frame tap latch already covers the edge-consumed action
keys; a per-sub-frame direction feed would change the TickInputs contract —
possible follow-up); sliding/flying bombs integrate their three accruals in
one pass (exact, same argument as rovers — kicked speed 1000 divides
evenly); and cross-entity interleaving stays entity-serialized within the
tick (the original interleaves whole passes per frame).

(Provenance: in-match-shell.md's sub_42A191 per-frame findings + 14-16 ms tap
measurements; facts.md "Speed = a spent budget" [`speed × frameDelta /
0x46494C`], "Movers advance the counter once per pixel-step" [rover budget
`+= speed*dt/msPerFrame + 100`], the +78 state-machine audit's 22982-22984
stun block and +80/+82 accumulator idiom, ai.md §3.6/§3.5's `+= frameDelta`
timers and `10*dword_46494C` timeouts, "Ice / input-lag"'s per-frame
age/shift/insert; live A/B against the shipped original on the reference
install, 2026-07-12. Port: constants.hpp kSubFrames/kSubFrameMs/frame_budget,
simulation.cpp player_turn, movement.cpp, stage_actors.cpp, ai.cpp/brain.hpp,
rovers.cpp; tests test_move/test_conveyor/test_disease/test_state_machine/
test_placement_diag/test_ai updated to the frame model; goldens B/C/D/E and
the visual goldens recaptured, A byte-identical. ADR-0006.)

**UPDATE 2026-07-16 — canonical rate re-pinned at ~180 fps (nine sub-frames),
and the sub-frame motion now reaches the screen.**

1. **Rate.** The original is NOT vsync-limited on modern hardware: DirectDraw's
   windowed present does not block on vblank under DWM, so BM95.EXE free-runs.
   Measured on the reference Win11 box: a full 180 s draw round rendered
   33146 frames (bmstats "Last Run") ≈ **184 gameplay-driver callbacks per
   second** — the AI brain, input sampling and movement budgets genuinely ran
   ~9× per 50 ms tick, not 3×. The user-visible symptom of the 60 fps pin was
   "our AIs are calmer than the native original" (still, after the danger-map
   audit). `kSubFrames = 9`, `kSubFrameMs = {6,5,6,5,6,5,6,5,6}`; the 30-slot
   ice buffer now spans ~166 ms, which CAPS Hockey Rink's 250 ms lag exactly
   as the original's fixed 30-slot history does at this frame rate (the
   resolve loop runs out and the oldest sample stays in effect —
   MovementSystem::ice_delay, test_ice). A 16-frame head stun is ~89 ms.
   `kSubFrames` remains the single "temperature" lever: 3 ≈ period hardware,
   9 ≈ the reference install. Since the original is frame-rate-dependent by
   construction, matching the user's OWN native session is the fidelity
   target.
2. **Sub-frame presentation trace** (`State::sub_trace`, renderer
   `player_interp`): the sim records every player's position+facing at the
   end of each canonical sub-frame — a derived per-tick output exactly like
   `s.events` (rebuilt every tick, never hashed) — and the renderer plays the
   trace back across the tick interval instead of lerping the two 20 Hz
   endpoints. Without this, EVERY direction change with period < 100 ms is
   mathematically invisible on screen (the endpoint lerp is a low-pass
   filter): a bot zigzagging 9× inside a tick rendered as standing still,
   which was the dominant cause of the "less jitter than the original"
   report — the sim already twitched faithfully, the presentation discarded
   it. The last trace sample is pinned to the tick's true endpoint (run_tick
   step 12) so post-loop relocations (warp/trampoline/head-hit scatter) snap
   cleanly via the per-segment threshold.
3. Goldens: hash constants recaptured 2026-07-16 for the colour-split hash
   layout + the disease-scaling order fix (see test_golden.cpp's UPDATE
   note); the RNG-stream safety net passed unchanged. Visual goldens
   recaptured for the cadence timeline + tile-layer draw order.

## AI danger map — under-population audit, PORTED (2026-07-12, `sub_42331C` tail stamp / `sub_40970B` / `sub_40B20F`)

User live-comparison: the original's AIs are visibly more active/jittery even
after the canonical-frame-cadence port tripled the decide rate. A full
differential re-read of the danger-map WRITERS and the flee path found the
port's danger grid under-populated and under-scaled — danger is the one input
that forces behaviour 2 (the reliable mover), so the original spends more
time skittering while ours idled in the calm wander regime. Six findings, all
ported in `ai.cpp` (goldens carry no AI players; all 42 suites stayed green):

1. **Flying bombs stamp danger.** The per-bomb tail stamp (pseudo.c
   25683-25705) gates only on the ACTIVE flag and the carried-pass parity —
   motion is never tested — so a punched/thrown bomb casts its full blast
   prediction from its instantaneous arc tile every frame. The port's
   `|| b.flying` skip (with a wrong "airborne bombs cast no prediction"
   comment) made AIs stand calmly under a sailing bomb.
2. **Carried bombs stamp danger from the carrier's tile.** The carried pass
   (`sub_42331C(1, ·)` at 25784, called per frame at 29530) runs the same
   tail stamp — enemies scatter around a bomb-carrying player. Our carried
   bombs are deactivated slots (`try_grab`), so the port now stamps them off
   `Player::carrying`/`carried_flame`; the elapsed term is approximated at 0
   (no carry-age counter; the throw restarts the fuse anyway).
3. **The danger value is elapsed MILLISECONDS + 100** (`v = +68 + 100`,
   25685; +68 accrues the per-frame ms delta) — range 100..~2100 over a 2 s
   fuse, so cross-source ordering against the closing walls (110..250) and
   flame (1000) depends on the ms scale. The port's tick-based 100..~140
   ranked every bomb below the walls, flipping flee-route choices whenever
   sources compete (hurry-up especially). Now `(fuse_init - fuse) * 50`; a
   waiting trigger bomb (unbounded +68 accrual in the original) is
   approximated at a full fuse's worth — past the point it outranks flame,
   as an aged trigger bomb genuinely does.
4. **The flee/danger `here <= min` branch never passes down — but the
   fully-boxed-in case DOES.** (CORRECTED 2026-07-16: the 2026-07-12 wording
   folded both cases together and over-reached.) `sub_40970B` inits its
   best-tracker at 10000 (9936) — with any open neighbour it returns a step;
   firstdir is null only when fully boxed in. `sub_40B20F`'s flee branch
   (10816-10829) then splits on whether that step-finder returned a first
   direction at all: when it returned NOTHING, the branch clears the brain's
   target flag (+2 ← 0) and returns 0 — a
   fully-BOXED-IN AI in danger CLEARS the target flag and passes down, so
   behaviours 3-7 DO run that frame (whim draws, drop gates, wander
   re-rolls: the trapped-in-danger fidget). Only past that does it latch
   `+2=1` and, when `here <= min` (no strictly-safer tile), stand (godir -1,
   own tile as target, return 1) with behaviours 3-7 never running. The
   2026-07-12 port froze the boxed-in case calmly (write_move(-1)/return 1)
   — an RNG-stream and jitter divergence, now mirrored exactly (ai.cpp
   behave_walk_path).
5. **Flee BFS ring cap = 20** (the search's 4th argument, 10048-10053),
   best-so-far kept. The
   port's frontier had only the 100-node cap.
6. **The enclosure lookahead burns an iteration per corner turn**
   (27201-27223: the candidate exiting the ring turns the cursor and
   re-stamps the pre-turn tile with the already-decayed value) — near
   corners it covers fewer than 15 distinct future bricks. Ported via a
   direction-change detector over the spiral positions; the ring-advance
   diagonal counts as a turn (documented approximation of the
   ++ring/++x/++y branch).

Verified-faithful in the same audit (no change): the wander behaviour's
exact structure, sub_40A59D/sub_40A76E/sub_424D37, the dispatcher's
hold-still semantics (the caller presets godir -1 and resets the key bytes
per frame — "keeps last direction" is disproven), every behaviour whim/gate
polarity, the BFS RNG contract, and the flame 1000 stamps. Tests:
tests/test_ai.cpp "2026-07-12 danger-map fix" pair (flying + carried).

## Ice buffer flows through flight states — PORTED (2026-07-12 follow-up to "Ice / input-lag")

Same-day audit of the Hockey Rink report: the ice block (sub_41F29B
23058-23078) sits ABOVE the player-state dispatch, so it ages/pushes/resolves
every frame in EVERY alive state — a bouncing/warping player pushes that
frame's -1 godir (offset 46 is reset to -1 at 22980 before the gated input
read). The port froze the buffer during bounce/warp (early-return before
`ice_delay`), so landing on an icy level replayed up to 250 ms of stale
PRE-flight direction — a phantom movement burst the original doesn't have.
Fixed: the bounce/warp branches and the mid-turn flight `continue` now push
-1 per sub-frame (`simulation.cpp`; a no-op on every delay-0 level, keeping
the hashed `ice_history` untouched there).

Confirmed in the same pass: VALUELST 450-460 are wall-clock MILLISECONDS
(the file's own legend; only 452 = Hockey Rink is non-zero, 250), the ages
accrue the real frame delta, and the resolve compares them raw — our
sub-frame model reproduces the designed 250 ms exactly at any 20-120 fps
frame rate. CAVEAT (open observation, not ported): the original's buffer is
30 REAL FRAMES, so on an uncapped fast machine its effective lag collapses to
~30 frames' wall time (min(250 ms, span)) — a natively-running BM95.EXE at
very high fps feels LESS icy than its own design. If A/B feel-parity against
the native Win11 run (rather than the designed 250 ms) is ever wanted,
measure that run's real frame rate first and clamp the delay to the 30-frame
span at that rate.

## In-match colour quantization (shared COLOR.PAL palette) — CONFIRMED + PORTED (2026-07-13)

User report: the in-game map "looks a bit darker / different", and specifically
**Classic Green Acres** (FIELD1) has a BLUE play area in the port but reads
GREENER/more muted in the original. Root-caused to the original's paletted
display pipeline and ported.

**Mechanism (workflow RE, verify-confirmed).** The whole in-match screen runs
on ONE 8-bit hardware palette = COLOR.PAL's 256 master colours. Every decoded
asset pixel is SNAPPED to that palette at load: the type-4 (RGB555) cel
decoder `sub_41C837` line 21309 does `*dst = byte_495390[rgb555]` (an RGB555 →
master-index reverse LUT), and the 8-bit path `sub_41BBBD` (20805-20841)
rebuilds the same per-source-palette LUT from `byte_495390` and rewrites every
pixel to a master index. The palette uploads 6-bit and DirectDraw scales it
`4 * value` (`sub_443608` ~48226-48251), so the brightest displayable channel
is `63*4 = 252`, never 255. `byte_495390` = **COLOR.PAL** (install root, 33536
bytes = **768 master RGB** + **32768 RGB555→index LUT**; frontend-flow.md's
"byte_495390 decoded for real").

**Why it's mostly invisible but visible on FIELD1.** The shipped field/tile
art is AUTHORED in the master palette, so the snap is an exact IDENTITY for
FIELD0/2..10 and to within ~2-3% for the tile/brick cels (measured). The lone
exception is FIELD1's floor, a vivid **blue/green dither NOT in the master
palette**: raw `(23,27,139)`+`(19,143,19)` snap to `(20,40,108)`+`(4,132,0)`.
The port's straight per-asset decode (pcx raw palette; ani.cpp `expand5`,
which reaches 255) showed the raw vivid dither — the reported difference.

**Empirical proof (the arbiter, method.md-style pixel measurement, not
theory).** A live capture of the running original was compared to the raw
assets: FIELD4 border + interior floor matched raw FIELD4.PCX at **scale 1.0,
err 0** (identity — it IS a master-palette field); FIELD1 floor rendered the
`(20,40,108)`/`(4,132,0)` dither while raw FIELD1.PCX is
`(23,27,139)`/`(19,143,19)`. The ported quantizer reproduces BOTH exactly:
`snap(23,27,139)=(20,40,108)` (master idx 57), `snap(19,143,19)=(4,132,0)`
(idx 128), `snap(126,126,126)=(108,116,128)` (border brick, original shows
(109,116,126)). Master palette = COLOR.PAL[0..767] × 4 with entry 0 forced to
black (a white sentinel in the file); LUT at offset 768; index =
`(r>>3)<<10 | (g>>3)<<5 | (b>>3)`.

**Port.** `libs/assets/colorpal.{hpp,cpp}` (`Palette::load` + `snap`/`remap`,
SDL-free, COLOR.PAL is shipped data), loaded once by `AssetStore::load` from
the install root and applied in `load_stage` to the CLASSIC field PCX and the
tile/brick ANIs (`AniTextures::load`'s new snap param) — NEVER the DATA_HD
truecolour overrides or the front-end screens (the original loads those
through the non-snapping `sub_41BDA4`/`sub_41522D` path). A missing COLOR.PAL
leaves the quantizer inert (raw decode, the pre-fix look). Tests:
`tests/test_colorpal.cpp` (synthetic-file mechanics); visual goldens
recaptured. Applied to EVERY match-drawn asset (2026-07-13 follow-up, per the
user's "it's everywhere"): field, tiles/bricks, and all sprites — bombs, duds,
flames, powerups, players, the walk/stand/kick/carry/punch/pickup/cornerhead
poses, and the death animations. Player/bomb sprites snap AFTER the .RMP/
truecolour recolour (make_texture's snap param via AniTextures::recolored), so
the shared-palette constraint applies to the final displayed colour. NOT
snapped: the front-end screens (MISC cursor, EDIT tiles, goldman wheel,
backdrops, fonts) and the DATA_HD truecolour overrides, which the original
loads through its non-snapping path (`sub_41BDA4`/`sub_41522D`). The one large
shift is FIELD1's dither; everything else is a subtle ~2-3%.

**Front-end vs match, verified in the binary (2026-07-13, per "check the
screens too, exactly" — two passes; the first split was WRONG and is corrected
here).** Two decode entry points: `sub_4150F0` = `sub_41BE63` SNAPS (decode +
`sub_41BBBD` remap to `byte_495390`/COLOR.PAL); `sub_415120` = `sub_41BDA4`
does NOT. The distinction is by SCREEN, and there are TWO front-end groups:

- **Own-palette, NOT snapped** (`sub_42A088` @29440 -> `sub_41522D`, which
  decodes with `sub_415120` and UPLOADS the screen's own palette via
  `sub_42C534`): the logos, TITLE, DRAW, and results/victory screens
  (callers 29825/30130/30203-30207). Port blitting these raw is 1:1.
- **SNAPPED to the master palette, no per-screen upload** (`sub_4151CC`
  @17691 -> `sub_4150F0`, blits into the backdrop save page with COLOR.PAL
  master left active): the **main menu** (`sub_42B9CE` 30760, MAINMENU) and
  **every GLUE backdrop** (`sub_4148E5` 17342, the options/setup/level-select
  screens). These were the earlier mistake — the port blitted MAINMENU/GLUE
  raw. Ported: `frontend_pcx` now snaps MAINMENU + GLUE<n>. Measured:
  MAINMENU is IDENTITY (authored in the master palette, so the main menu
  already matched), but GLUE0 shifts 6.6/channel over 66% of its pixels
  (GLUE3 identity) — the pre-match backdrops now match for the non-master
  glue pictures.

Also snapped (`sub_4150F0`): the LEVEL & ROUNDS preview swatch (`sub_406AA3`
7985, field + tiles — `stage_preview` now snaps, FIELD1 reads the muted
dither), the `.BM` viewer's inline images (`sub_41302D` 16370), and WINZ.PCX
the dialog 9-patch (`sub_414DF4` 17546). WINZ measured raw→snap **2.15/channel
mean, border blue (0,91,111) unchanged** (master-palette-authored), and the
`.BM` inline images are rare — both left raw as imperceptible, documented.

## Bomb/flame colour is not the owner — CONFIRMED (2026-07-16, `sub_422EDE`/`sub_42331C`/`sub_426FCC`)

The original keeps a bomb's DRAWN COLOUR and its OWNER as two separate fields
packed into one dword at bomb +60: the low BYTE (+60) is the colour, written
once at creation by `sub_422EDE` from the placer's own colour byte — which
team mode forces to 0/2 at actor init, pseudo.c 23916-23927 — and the WORD at
+62 is the owner id, written at creation from the creator's player-id
argument. Every consumer keeps them separate:

- **Flame ignition** (`sub_42331C`'s epicentre/arm calls at 25625/25677 and
  the brick branch at 25667): the ignite call `sub_426FCC` is passed
  `(x, y, colour, kind, owner)`, where colour is the BYTE at bomb +60 and
  owner is the same dword's UPPER half (the dword at +60 shifted right 16,
  i.e. the word at +62) — colour from the byte, owner from the high word —
  and the flame-cell record stores BOTH (+60 colour / +62 owner,
  `sub_426FCC` 27494-27500). The flame drawer `sub_426D06` blits with the
  record's colour byte; kill credit compares the record's +62 word
  (23316/23327).
- **Chain hits transfer ONLY the owner word**: pseudo.c 25644 copies the
  exploding bomb's owner WORD at +62 into the hit bomb's +62, and nothing
  else — the chained bomb's kill credit
  (and capacity slot, see "Bomb capacity is a derived live-bomb count") moves
  to the chainer, but its colour byte is untouched, so its eventual explosion
  still flames in the ORIGINAL placer's colour. Overlapping/chained
  explosions from different players visibly keep their own colours.
- Grab/punch never rewrite either field (the only +60/+62 writers in the
  binary are creation and the 25644 chain transfer).

**The port bug this fixes:** a single `Bomb::owner`/`flame_owner[][]` carried
both meanings, so a cross-player chain recoloured the chained bomb and its
whole explosion to the chainer's colour (the user-reported "flames lose their
colour when two players' explosions meet"). Ported: `Bomb::colour` +
`Player::carried_colour` + `State::flame_colour[][]` (all hashed — packed
into existing hash words, so zero-valued states digest identically;
test_golden.cpp recaptured in the same commit), set at creation/ignition and
never transferred; the renderer draws bombs, carried bombs and flames from
the colour fields and keeps attribution (kill credit, capacity) on the owner
fields. `tests/test_flame_colour.cpp` pins the split.

## Round-start input freeze — CONFIRMED (2026-07-16, `sub_4214BC`/`sub_420F07`/`sub_41F29B`)

Round init arms `dword_4621E0 = [0x46494C] × getvalue(30)` = 50 ms × 20 =
**1000 ms** (pseudo.c ~23959, right beside the `dword_4621E8` colour-spin
timer). The player-pass entry `sub_420F07` decrements it by the measured
frame delta at the top of every frame (23642-23645, clamped at 0), and
`sub_41F29B`'s acquisition gate at 23028 — "the per-tick new-input flag is
still set AND `dword_4621E0` has already reached 0" — skips BOTH the AI
brain (`sub_40A1C6`) and the human input read (`sub_41E61E`) while the timer
runs; nobody moves or acts for the first second of every round (the sprite
colour-shuffle window). getvalue(30)'s own VALUELST legend is "how many
frames per second are we gonna attempt to get?" — the engine reuses the 20
fps target as "one second's worth of 50 ms frames". The bomb-action block
(23277-23380) still runs (its key bytes just stay at their per-frame reset),
so only the diarrhea auto-drop force could act during the window. NOTE: docs/re/ai.md §7
previously dismissed `dword_4621E0` as a menu/pause freeze with no headless
equivalent — it is actually this round-scoped gameplay timer.

Ported: `Tuning::input_freeze_ticks` (id 30, default 20) →
`State::input_freeze` (hashed), armed by `build_state`, decremented once per
tick AFTER the player pass (run_tick step 1b — the post-pass decrement
reproduces the original's exact t = 1000 ms gate-open boundary at tick
granularity), gating the AI decide + human decode + bomb-action tail in
`player_turn`. `tests/helpers.hpp` and the golden/demo fixtures disarm it to
keep act-from-tick-0 scenarios; `tests/test_freeze.cpp` pins the window.

## Draw order — tile layer addendum (2026-07-16, `sub_425D22`/`sub_425EFC`)

The static solid/brick tiles are NOT drawn per frame at all: they live in the
BACKGROUND surface. `sub_425D22` stamps "tile %u solid"/"tile %u brick" into
the background whenever a cell's type changes (`sub_425E36` writes the type,
`sub_425D22` restores the field patch via `sub_4166BF` then blits the tile),
called from `sub_425EFC`/`sub_425E9B`. Consequently EVERY per-frame sprite —
bombs (including a punched/thrown bomb's whole flight arc), powerups, flames,
players — composites OVER the tiles. Also: `sub_425EFC`'s blank-stamp-revert
dance at brick ignition (26826-26846) means a CRUMBLING brick's background
shows bare floor for the whole burn — the crumble frames (sub_426D06 kind 9)
composite over floor, not over a still-drawn brick, even though the cell TYPE
stays Brick (blocking) until the burn expires.

**The port bug this fixes:** the renderer painted static cells after
draw_bombs, so an airborne bomb crossing a brick/solid tile vanished behind
it (user-reported). Ported: `Renderer::draw_cells` (solid + non-burning
brick) runs right after the field blit, before every sprite pass; the
brick-crumble frames stay in draw_world's flame/burn slot (sub_426D06's
position, after bombs/powerups). Visual goldens recaptured in the same
commit.

## Walk leg-cycle pacing — CONFIRMED (2026-07-19, `sub_41F29B`/`sub_41EC84`)

The walk/stand/carry pose frame is `(u16)player[+48] / 3 % statecnt`
(sub_41F29B pseudo.c 23410 — one shared `sub_41DAA7(seq, +48/3)` site for
walk, walkbomb, stand and standbomb), and the +48 counter advances **once per
PIXEL step** at the tail of the per-pixel mover loop (sub_41EC84, 22718) —
i.e. **one animation frame per three pixels walked**. While standing (godir
-1) the SAME counter instead increments once per displayed frame (23084),
which is invisible for the single-frame stand poses but keeps the phase
continuous across stop/start. The counter is never reset on pose changes;
kick/punch/warp/trampoline states use the separate +80 counter (50 ms
quantized) — not this one — and the death anim reads +48 WITHOUT the /3
(23459), advancing once per 50 ms via its own accumulator.

**The port bug this fixes** (user-reported "walk animation stops after
collecting some skates"): the renderer advanced the leg cycle one FRAME per
pixel (no /3), which both ran 3x fast and — worse — froze the cycle
completely whenever the per-tick pixel budget hit an exact multiple of the
sequence length (`(phase + k*nframes) % nframes` is constant), e.g. a
1-skate 10 px/tick walker against a 10-frame WALK.ANI: the legs stopped
pedalling and the player glided in the stand-still stride. Ported:
renderer.cpp divides `walk_phase_` by 3 at both pose sites (walk and
walkbomb). The visual goldens' walking frame recaptured in the same commit.

(Provenance: native-port transliteration of sub_41F29B/sub_41EC84 — the
pose-tail and pixel-loop are now source-level readable; line cites above.)

## Spawn-pocket clear — NOT PINNED, best-effort widening (2026-07-19)

**The bug.** `libs/sim/src/setup.cpp`'s per-spawn brick clear (present since
the project's first commit, no RE citation) only cleared the spawn tile's 4
orthogonal neighbours (a radius-1 "plus", 5 cells). On a dense scheme like
the shipped `BASIC.SCH` (90% brick density, every non-`(odd,odd)` cell a
brick candidate — verified against the raw file, `DATA/SCHEMES/BASIC.SCH`
has no blank cells authored near any `-S` spawn), a corner spawn's escape
pocket was then only 1 tile deep in each direction — entirely inside a
default flame-2 bomb's blast radius (`Tuning::start_with[Flame] = 2`). The
(separately faithful) AI flee logic could never find a strictly safer tile,
so a computer player dropping its own opening bomb near its spawn reliably
self-killed within the first few seconds of round 1 — a mass-suicide
epidemic on any dense/default scheme.

**The search.** The board's tile array (`dword_46222C`) has exactly ONE
writer in BM95.EXE: `sub_425E36` (pseudo.c 26781-26797), a plain bounds-
checked `cells[y][x] = value` store. Every path that can reach it was
enumerated and read:
- Three thin wrappers, `sub_425E9B` (26802), `sub_425EFC` (26825, unused by
  anything relevant), `sub_425F79` (26851) — all just call `sub_425E36`
  plus a redraw.
- All ~19 call sites of the whole family: bomb-flame burn-through (pseudo.c
  7252, 7266, 25669, 27412 — the standard "flame reaches a brick, ignite it"
  path), netplay tile-sync replication (12149, 12189, 12483, 12637, 22406 —
  gated on `dword_460058`'s netplay flag, corrects a remote player's tile if
  it desyncs onto a brick), the warphole neighbour clear (26537/26541,
  already ported — see the warphole entry elsewhere in this file), and the
  HURRY wall drop (27235, `docs/re/enclosure.md`). **None run at match setup
  or reference the spawn-coordinate arrays** `dword_46460C`/`dword_46465C`.
- The round-init sequence itself, `sub_410B6E` (pseudo.c ~14689-14857): board
  build `sub_4260F5` → field/background load `sub_4165FC` (a `FIELD%u.PLT`
  background BITMAP, unrelated to the tile grid) → tile redraw `sub_42633C`
  → player placement `sub_4214BC` → powerup scatter `sub_4258E5` → rovers
  `sub_40551F` → campaign hazards `sub_40151B`. Read in full: `sub_4214BC`
  (23865-23962) only stores each player's resolved pixel coordinates into
  its own struct (`sub_40F48C`, itself just another struct-field setter,
  UNRELATED to the netplay position-sync arrays of the same name pattern
  found nearby) — it clears no cell. `sub_4260F5`'s non-editor branch
  (26927-26944) rolls brick density from the scheme grid alone, with no
  reference to any spawn coordinate.
- The `.SCH` loader, `sub_403EEE` (pseudo.c 6252-6499): parses `-R`/`-S`/`-P`
  directly into the in-memory grid/spawn arrays, no post-process clear step.
- `sub_4048EB`, the scheme-grid WRITER paired with the `sub_404852` reader
  `sub_4260F5` consumes — its only 2 call sites (pseudo.c 5560, 5610) are
  both inside the interactive scheme EDITOR's mouse-paint handler, not the
  runtime match-setup path.
- No VALUELST id documents a "spawn safe radius" (`docs/valuelst-map.md`,
  `docs/re/id-audit.md`); the nearest relative, id 695, is the UNRELATED
  tile-*regeneration* clear radius (`sub_422351`, "Per-level tile
  regeneration" above), which gates brick REGROWTH near live players during
  the match, not initial spawn placement.

**RESOLVED EMPIRICALLY 2026-07-21 (native ground truth): there is NO
spawn-pocket clear — the original leaves a brick ON the spawn.** The static
search above (no setup-time tile writer, `sub_4048EB` editor-only, `.SCH` 'S'/
'R' directives independent) is now confirmed by running the ORIGINAL's own
board build. The native transliteration's `--oracle <out> 0 pocket` scenario
(`native/src/main.cpp`, gitignored) builds BASIC.SCH's real scheme grid
(15×11, '#' iff `(col&1 && row&1)`, else ':'), sets density 90, and runs the
REAL `sub_4260F5` fill 4000× with varied seeds, reporting each cell's brick
frequency:

- Every non-pillar cell — INCLUDING all ten `-S` spawn tiles — reads **89–90%
  brick**, exactly the density. p0 (0,0)=89%, p1 (14,10)=90%, … no spawn is an
  outlier. The `(odd,odd)` pillars read 0% (they are '#'→solid, never brick).
  So `sub_4260F5` applies the scheme+density with **zero spawn exception**.
- A second probe FORCES a brick onto a spawn tile, runs the real `sub_4214BC`
  placement, and re-reads the tile: it is **still a brick (=2)**. Placement
  does not clear the tile a player lands on.

So the original **places a brick on the spawn ~90% of the time and never
clears it** — a player spawns boxed in and must bomb its way out (the classic
high-density-scheme opening). No emergent mechanism, no out-of-range code: the
pocket simply does not exist in the original.

**Therefore the port's clear is a DELIBERATE DIVERGENCE, not a faithful port.**
Kept as a pragmatic workaround (below), but re-labelled honestly: the true
root cause of the "first-round AI mass-suicide" is the port's own AI-flee
logic failing at the boxed-in bomb-your-way-out opening that the ORIGINAL's AI
handles — the pocket clear MASKS that AI bug rather than reproducing an
original behaviour. The faithful fix is to make the clean-room AI survive a
spawn brick box the way `sub_40A1C6`'s does; until then the clear stays. The
"BM95 screenshots show ~2-tile pockets" observation that motivated the widen
is now suspect (different scheme/density, or post-opening bricks already
bombed) and should NOT be treated as evidence of a setup-time clear.

**The port.** `libs/sim/src/setup.cpp` widens the cleared shape from the old
radius-1 "plus" (5 cells) to a radius-2 orthogonal "plus" (9 cells: the
spawn tile + 2 tiles in each of the 4 cardinal directions — NOT a diamond,
NOT diagonals). This is the smallest shape that (a) matches live observation
of the running original — BM95.EXE screenshots taken this session show each
corner spawn opening with a cross/plus pocket whose arms reach ~2 tiles, not
1 — and (b) is actually sufficient: a flame-2 bomb dropped on the spawn tile
no longer has its blast seal every cell of the pocket, giving the AI's flee
logic room to reach a tile outside its own blast before the fuse expires.
Empirically verified in `tests/test_spawn_pocket.cpp`: on a golden-B-shaped
dense 4-corner board with all 4 slots AI-controlled and no human input at
all, every player survives the first 200 ticks (10 s) under the fix; reverting
to the old radius-1 shape drops `alive_count` from 4 to 2 in the same window
(checked by hand while pinning the test, not left in the suite).

The clear draws no RNG (a deterministic cell-array write keyed off already-
resolved spawn coordinates), so this is a DELIBERATE but RNG-neutral
behaviour change — see `tests/test_golden.cpp`'s 2026-07-19 UPDATE note for
the golden-hash recapture this forced (goldens B and C, whose boards have
real Brick cells within a spawn's new radius-2 reach; A/D/E are byte-
identical, proven before recapturing).

**No residual uncertainty about the mechanism** (as of the 2026-07-21 native
probe): it is proven absent. The open item is now a PORT question, not an RE
one — replace the divergent pocket clear with a faithful AI-flee fix that lets
a clean-room AI escape a spawn brick box, then delete the clear and recapture
goldens B/C. Tracked as the spawn-pocket follow-up, not a "still guessed"
citation.

## Brick-reveal cure roll (empty hook, RNG-count only) — CONFIRMED (2026-07-20, `sub_425107`/`sub_42BE0B`)

`sub_425107` (the brick-reveal/relocate routine the flame arm calls on every
brick ignite, pseudo.c 26274) opens with, as its very FIRST statement, a
1-in-30 roll: draw `rand_()` once, take it modulo 30, and call `sub_42BE0B`
only when the remainder is 0. Nothing else in the statement — no state read,
no state write.

`sub_42BE0B` is an EMPTY function (pseudo.c 30942, an empty body — a
dead/stubbed debug hook; its only other caller sits behind an equally-inert
path). So the roll has **no gameplay effect** — it merely CONSUMES one
`rand_()` draw per brick ignite. That still matters: the RNG stream is the
determinism contract, so a port that skips this draw runs one step short per
brick reveal, and every downstream random outcome (which powerup a *later*
reveal shows, a disease roll, a head-hit or death scatter tile) drifts out of
step with the original on any brick-bearing match — the classic "small
differences everywhere."

**Ported** in `FlameSystem::spread_to`'s brick branch as `(void)random_below(s,
30)`, unconditional and BEFORE the relocate/reveal work (exactly where
sub_425107 does it, also firing on a re-hit of an already-crumbling brick,
matching the per-ignite call). `tests/test_flame_colour.cpp` pins that a lone
brick ignite advances `State::rng` by exactly one xorshift step (two identical
sims, one blast into a plain brick, one into blank floor). Golden hashes are
unaffected — every golden scenario's blasts stay inside the radius-2 spawn
pocket and reach no intact brick — but the demo match destroys bricks, so the
three post-brick visual-golden shots (t65/t68/t76) were recaptured while
t10/t40 (pre-brick) stayed byte-identical.

(Provenance: fidelity-audit batch-2 flagged this as a pre-existing systematic
RNG-count omission; native-port transliteration of sub_425107/sub_42BE0B made
the empty hook visible at source level. docs/re/fidelity-audit.md.)

## Network setup screens — CONFIRMED (2026-07-25, `sub_42B0CE`/`sub_42B47D`)

Full spec: `docs/re/network-screens.md`. Raw-byte verified (capstone), because
Hex-Rays dropped five load-bearing register arguments in these two functions.

**Net role polarity — CORRECTED.** `dword_460058` is **`1 = GUEST`,
`2 = HOST`**, not the `1 = host, 2 = guest` recorded in `multiplayer.md` §1.1,
`frontend-flow.md` and `audit/multiplayer-deep.md` §3. Four proofs:
`sub_40C035` gives *only* mode 2 its own node id as session authority
(`dword_4600D4 = HIWORD(dword_46013C)`); `sub_40CD1C` stamps mode 2's own id
but mode 1's *host's* id into the datagram header; `sub_4105D2` has mode 1
copy the match clock while mode 2 computes and broadcasts it (`sub_40FCA1`);
and the announce / start / options senders (`sub_40EBC1`, `sub_40ED08`,
`sub_40FE88`) are gated `== 2` only.

**Screen assignment.** Main-menu row 1 → `sub_42B0CE` @0x42B0CE, head
`mov eax,2 ; call 0x40C839` = **START NET GAME (host)**; row 2 → `sub_42B47D`
@0x42B47D, head `mov eax,1` = **JOIN NET GAME (guest)**. VALUELST's own section
comments agree (765-778 "START NET GAME SCREEN" = the ids `sub_42B0CE` reads;
750-763 "JOIN NET GAME SCREEN" = `sub_42B47D`'s). This settles the conflict
`audit/multiplayer-deep.md` §2 flagged: the labels were right, the mode table
was inverted.

**What the screens list — CORRECTED.** Neither is an options pane and neither
assigns controllers.

- `sub_42B0CE`'s `for i in 0..3` walks the **host's connected-client table**
  `word_45FFA4[4]` (node id) / `dword_45FFFC[4]` (41-byte name). Occupied row =
  `getstring(71)` with `%s` name + `%u` node id; empty row = `getstring(72)`,
  no args.
- `sub_42B47D`'s `for i in 0..9` walks the **guest's discovered-server table**
  `word_4600D8[10]` / `dword_45FF04[10]` (name) / `dword_45FF54[10]` (that
  server's current client count). Occupied row = `getstring(62)` with **three**
  args in order `%s` name, `%u` client count, `%u` node id; empty row =
  `getstring(63)`, no args. `sub_40F1BD(i) >= 4` = "server full"
  (`getstring(7)`), NOT "a controller index".

**Session caps.** 1 host + **4** guests. `sub_40D175` seats a join request in
`word_45FFA4[0..3]`; `sub_40ED08` assembles `word_460130[5]` (`[0]` = host);
`sub_40E765` discards any datagram whose sender is not one of those 5; boot
(`sub_40C74C`) allocates `sub_418511(1004, 5)` = five per-node dedupe rings.
Players stay at 10 — a machine uploads every one of its local slots
individually (`sub_40EE16`, kind 40) from the shared roster screen.

**Timings and geometry.** `getvalue(13) = 3` s is the "minimum seconds to wait
at screens so other computers can catch up" settle gate (`sub_4148AC` returns
it in net mode, `1` locally) — the host cannot START until its client set has
been unchanged that long, and the same gate guards Enter on the shared roster
and level screens. The host beacons kind 0 at **1 Hz**; START sends kind 14
**five times, 100 ms apart**, then kind 49. A guest's join retries kind 3 once
per second for **3000 ms**. Both screens are pixel-identical: title (120, 80,
clip 400), header (120, 120, clip 250), rows (120, 140 + 20·i, clip 400);
guest cursor at `sub_413BD6(100, 156 + 20·sel)` — the `+16` half-row offset
was Hex-Rays-dropped. **The 4th VALUELST column (753/758/763/768/773/778) is
the CLIP WIDTH, not a colour** — same correction the 2026-07-12 pixel pass made
for the player-input screen.

**Inks (COLOR.PAL LUT-decoded).** New value: `byte_497F8F` = LUT offset 0x2BFF
→ index 97 → **(96, 252, 252)**, the title ink on both screens (previously only
a nearest-search "cyan-ish" estimate in `results-and-options.md`). Rows/header
use `byte_49D38F` (240,248,252) with a `byte_495390[0]` black outline. All five
net modals go through `sub_414340(line1, line2, ink=EBX, outline=ECX)` with
ink `byte_49A390` **(164,0,0)** and outline `byte_49D37A` **(252,248,88)** —
traced `sub_414340` → `sub_4172BA` → `sub_41696C`, whose 6th argument is the
ink and 7th the outline.

**`sub_41696C` signature re-confirmed by raw bytes:** `(EAX surface, EDX text,
ECX x, EBX max_width, [stack] y, ink, outline)`, matching `frontend-flow.md`.

**`unk_4632CC` is NOT a player-config store — CORRECTED.** `setup-screens.md`
called it "404 bytes per player × 10". It is the Win32 joystick-capabilities
array: `sub_42965C` does `joyGetDevCapsA(i, (LPJOYCAPSA)(&unk_4632CC + 404*i),
0x194u)` and `0x194 = 404 = sizeof(JOYCAPSA)`; `sub_429A61(i)` returns
`+404*i+4` = `szPname`. Neither net screen touches it.

**Net identity.** `sub_40FE34()` returns `&unk_460140`, the machine's node
name (≤40 chars), shown on both screens' titles and in every lobby row. Loaded
from install-root **`NODENAME.INI`** (first line, `sub_40C08C`, from the boot
init `sub_40C74C`); absent → a random default
`getstring(500 + rand() % getvalue(47))` with `getvalue(47) = 49`. Written back
by `sub_40C140` from the shutdown hook `sub_40C4DB` (registered with
`sub_410EBF`, the same registrar `options.ini`'s `sub_405DE3` uses), so a
randomly-assigned name becomes permanent after the first run.

**Packet version.** `dword_45BAC4 = 21356` is stamped into every datagram
header (`sub_40CD1C`) and checked on receive (`sub_40E765`) — the original's
equivalent of the port's ADR-0011 `build_hash`. Both net screens print it every
frame via `getstring(55)` at (x=0, y=430, clip 320).

**`netprotocol` ordinals — RESOLVED.** VALUELST 1100-1104 (per-protocol
retransmit ms, with the file's own comments) and MESSAGES 630-634 agree:
**0 = none/cancel (200 ms), 1 = IPX (200), 2 = modem (750), 3 = serial (400),
4 = TCP/IP (400)**. `getvalue(1110) = 4` is the picker's item count
(`sub_42FEF0(list, count)` iterates `i < count`), so TCP/IP is never drawn, and
`sub_40C839` accepts only `1..3` anyway — the shipped exe is IPX / modem /
serial only.

**Entry gate.** `sub_40C839(role)` is the whole net bring-up, not a setter: it
sets the mode, requires **`CFG.INI` `netonoff` != 0** (else modal 96/340 and an
immediate bounce back to the menu), picks/validates the protocol, binds the
transport (`sub_43B81D` fills a 60-byte vtable at 0x4600F0), and runs a
cancellable connect-wait. A non-zero return aborts the screen before it draws.

**Net game forces `diseases_destroyable` on.** `sub_42B0CE` sets
`dword_464990 = 1` at entry; both screens save the 8 net-synced option globals
with `sub_40FF57` and restore them with `sub_40FFBC` on exit, and save/restore
the 10 slots' input type/sub bytes with `sub_4224E2`/`sub_422552`.

**Where the map and the AI come from in a net game.** Both screens commit into
`sub_42A3F6()` — the *same* handler main-menu row 0 (PLAY) uses — so a net
match runs the **shared** local pre-match flow: `sub_410F81` (roster, where CPU
slots are set; a guest blocks at its head, then uploads its local slots as kind
40; remote seats become input type **4**) then its tail `sub_406DDE` (level and
round count). Every edit is `sub_40C06A() != 1` gated, so guests are read-only
and get the SFX-40 buzz; the host broadcasts level index (kind 43,
`sub_40FA66`), round count (kind 44, `sub_40FAD5`), team flag (kind 58) and the
screen advances (kind 32, `sub_40F064(901)`/`(902)`). There is **no** net-only
map or AI UI.

## Still guessed — not yet extracted from the binary

| Constant | Current value | Status |
|---|---|---|
| _(none — the last entry, the spawn-pocket clear, was RESOLVED 2026-07-21)_ | — | The spawn-pocket clear is no longer a "guess": a native `sub_4260F5` fill probe PROVED the original places a brick on the spawn ~90% of the time and never clears it (see "Spawn-pocket clear" above). The port's radius-2 clear is a proven DIVERGENCE (a workaround for a clean-room AI-flee bug), not an unextracted citation. Follow-up is a port AI fix, not an RE extraction. |

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
