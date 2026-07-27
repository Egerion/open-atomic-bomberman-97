# Campaign mode — REACHABLE (not vestigial)

Answers the open question left by `coverage-audit.md`'s `.CAM` row: is
campaign mode reachable from any code path at all? **Yes.** It is a genuine,
if minor, feature — gated behind an undocumented secret key combo on the
local (non-net) PLAYER INPUT TYPE SELECTION screen, not exposed through any
menu row or button. This explains why RE passes reading only the menu
dispatch (`sub_42B9CE`, `docs/re/frontend-flow.md`) never surfaced it, and
why the two weak string refs (`aTotalOfUCampai`, `aCouldnTOpenCam`) looked
isolated — the loader they belong to (`sub_401085`) is real and wired up,
just reached from an unlabelled trigger deep inside a different screen's key
handler (`sub_410F81`), not from the string refs' own neighbourhood.

**Do not confuse this with frontend-flow.md's "campaign easter egg" label**
(main menu, Ctrl+E ×6, `sub_40330E`) — that was a mislabelling, corrected in
`docs/re/results-and-options.md` §5: that trigger is the **map/scheme
editor**, unrelated to campaign. The real campaign trigger (below) is a
different key, on a different screen, feeding a different subsystem.

## Trace: string refs → loader → trigger → entry point

1. **String refs** (pseudo.c 1309-1310): `aTotalOfUCampai` = `"Total of %u
   campaigns loaded.\n"`, `aCouldnTOpenCam` = `"Couldn't open campaign
   definition file '%s'.\n"`. Two more campaign-adjacent string constants
   exist that the earlier weak-ref pass missed: `aCam` = `"*.cam"` (pseudo.c
   1311) and `aSSSCam` = `"%s/%s/%s.cam"` (pseudo.c 1452, used at 15650 for
   building a save/level path, not the picker itself).
2. **Loader — `sub_401085` @ 0x401085** (pseudo.c 4325-4416): `fopen(path,
   "rt")`; on success, two-pass text parse (pass 1 counts entries, pass 2
   fills `dword_45E010` = `malloc(112 * count)`, one 112-byte record per
   campaign stage), then prints `aTotalOfUCampai` and sets
   `dword_45E018 = 1`. On failure, prints `aCouldnTOpenCam` and sets
   `dword_45E018 = 0`. Each line's fields are comma-split (up to 9, matching
   the `.CAM` format below) and only accepted if the line starts with the
   two-byte marker `-C` (pseudo.c 4374 tests three things, all of which must
   hold: the first character equals 45 = `'-'`, the second uppercases to
   67 = `'C'`, and the line's field count is exactly 9).
3. **Picker — `sub_4015C6` @ 0x4015C6** (pseudo.c 4550-4598): the actual
   caller of `sub_401085`. Globs `*.cam` (`aCam`) via `sub_411D17` +
   `sub_41404B`, shows a list dialog (`sub_41485A`, getstring 1250 header) of
   the matched filenames, and on a selection: calls `sub_401085(path)` to
   parse the chosen file, shows a confirmation overlay (getstring 1210 + 95
   via `sub_414340`), **sets `dword_46489C = 1`** (the campaign-active
   flag), and initialises all 10
   player slots (`sub_42288C(i)` for `i` in 0..9 — sets up the
   rover/ghost/AI roster per the parsed campaign record). On failure to find
   any `.cam` file it shows an error dialog (getstring 1215/97) instead.
4. **Trigger — inside `sub_410F81`, the PLAYER INPUT TYPE SELECTION screen**
   (pseudo.c 15357-15365, part of the screen's key-dispatch switch fully
   catalogued in `docs/re/setup-screens.md`):
   The branch, in order: the raw key code is tested `<= 0x43` (0x43 = 67 =
   ASCII `'C'`); if that holds, then `sub_40C06A()` must be false (not net
   mode) AND a consecutive-press counter, PRE-incremented in the same test,
   must equal 5 — the 5th consecutive `'C'`; only then is `sub_4015C6()`
   called, opening the campaign picker.

   i.e. pressing **'C' five times in a row** (any other key resets that
   counter to 0, same pattern as the confirmed `results-and-
   options.md` §5 Ctrl+E×6 counter) while on the player-setup screen, in a
   **local (non-network)** game — `sub_40C06A()` is the same net-mode guard
   used throughout the codebase for local-only branches (see
   `coverage-audit.md` §4 netplay boundary). This key is **not** in
   `setup-screens.md`'s existing key table (Up/Down/Right/Left/'0'/'T'/
   Enter/Esc/Space/'1'/288/315) — that table's "EXHAUSTIVE" claim needs a
   footnote pointing here; the campaign trigger was outside the range that
   pass covered by design (it treats the raw key as a fixed lookup rather
   than the fall-through relational chain the decompiler actually emits, so
   the `<= 0x43` fallthrough from the '0' check was easy to miss).
5. **Entry point.** `sub_410F81` is the already-confirmed-reachable
   PLAYER INPUT TYPE SELECTION screen: main menu row 0 ("Play") →
   `sub_42A3F6` → `sub_410F81` (`docs/re/setup-screens.md`, "COMPLETE RE" /
   "CORRECTION 2026-07-05"). No hidden precondition beyond "not a network
   game" — single-player and local-multiplayer both qualify. So the full
   chain is:

   **Main menu → Play → player-setup screen → press 'C' ×5 (local game
   only) → file picker over `*.cam` → pick a campaign → `dword_46489C = 1`.**

## What campaign mode actually does once active

`dword_46489C` (the campaign-active flag) is read at ~12 sites across the
binary, not just the loader — this is wired-through game logic, not a stub:

- **Skips the normal LEVEL & ROUNDS screen.** `sub_406DDE` (setup-
  screens.md's "Screen 2") starts with `if (!dword_46489C) { ...normal
  level/rounds UI... }` (pseudo.c 8087) — when campaign is active this
  entire screen's body is skipped, because the campaign file supplies its
  own scheme/level per stage instead of a player-chosen one.
- **Advances through campaign stages automatically.** `sub_401312`/
  `sub_40133F` (pseudo.c 4430-4508), each gated `if (dword_46489C)`, step
  `dword_4648B0` (the current stage index within the loaded campaign array)
  and load stage `dword_4648B0`'s scheme/rover-count/ghost-count/AI-count
  from the 112-byte record via `sub_4124A4(1235)`/`sub_4518D0` (formats the
  stage's display name) and `sub_404833`/`byte_49D38F`.
- **Drives the per-round game loop.** The round's per-frame tick callback
  `sub_42A191` @ 0x42A191 (registered by the Play handler `sub_42A3F6` via
  `sub_43A6FC` at pseudo.c 29701, after `sub_410F81` returns) calls
  `sub_4016DA()` every frame while `dword_46489C` is set (pseudo.c
  29528-29529). See "Round pacing — PINNED" below for the full breakdown
  (CORRECTED 2026-07-09: the original bullet here undersold what the
  survivor-count/rover-ghost clauses actually gate).
- **Gates round-loop exit/continuation.** Two more sites (pseudo.c 21936,
  24317) check `dword_46489C` alongside a player-slot AI-type test and the
  net-mode check `sub_40C06A()==1`, i.e. campaign mode is treated as a
  parallel "controlled" mode alongside netplay for round-continuation
  purposes (both keep looping under external/scripted control rather than
  the normal local win-condition path alone).
- **Roster/level auto-fill — CORRECTED 2026-07-09.** The seeder is
  `sub_40151B` (gated `dword_46489C`), NOT `sub_4015C6`/`sub_42288C` as this
  section previously said — `sub_42288C(i)` only clears a per-slot UI latch
  (`byte_461BD4[152*i]`, a "just showed a dialog for this slot" flag), it
  reads nothing from the campaign record. `sub_40151B` seeds the PLAYER
  roster from ONLY the AI count (field 7, via `sub_422928` — a RANDOM slot
  pick, not sequential) and separately spawns rovers/ghosts (fields 3-6) as
  autonomous map-hazard actors in an entirely different table, NOT as player
  slots. See "Rover/ghost/AI roster — CORRECTED" below for the full
  breakdown.

In short: campaign mode is a real, connected sequence of pre-scripted single-
stage matches (name + scheme + monster/AI composition per stage, read from a
`.CAM` file) that auto-advances through `dword_4648B0` stages, replacing the
normal manual level-pick and roster-pick screens for the duration. It reads
as a lightly-used developer/QA or hidden-bonus feature (no menu discovery
path, keyboard-mash trigger, only 3 shipped `.CAM` files with joke names —
"Just One Ghost", "Just One Dude" — rather than a marketed mode) but it is
functionally complete code, not a stub.

## sub_40151B — the REAL per-stage starter (2026-07-09)

Read while RE'ing `sub_4016DA`'s consumers. `sub_40151B` (gated
`if (dword_46489C)`) is called once per stage (both the freshly-armed stage 0
and every stage advance) and is the true seeder this doc's earlier "Roster/
level auto-fill" bullet mis-attributed to `sub_4015C6`/`sub_42288C`:

Its whole body sits inside that `dword_46489C` gate. With
`record = dword_45E010 + 112 * dword_4648B0` (this stage's own 112-byte
record), it does exactly this, in order:

1. `dword_464894 ← 0` — reset the round-phase flag.
2. For `i` = 0..9: `sub_42288C(i)` — clear the per-slot UI latch (NOT a
   roster seed).
3. Call `sub_422928()` exactly N times, where N is `record`'s dword index 26
   — that field is **ai_count** (field 7).
4. `sub_401994(…)` — reset the round timeout: `dword_464820 = 1`,
   `dword_4646C0 = 0`.
5. `sub_401B05(record[24], record[25], …)` — dword indices 24/25 =
   **ghosts / ghost_speed** (fields 5/6).
6. `sub_401AAE(record[22], record[23], …)` — dword indices 22/23 =
   **rovers / rover_speed** (fields 3/4). Its result is the function's own
   return value.

`sub_42288C(i)` (pseudo.c 24755) tests `byte_461BD4[152*i]` and, only if it
currently reads 1, writes 0 back to it
— a per-slot "dialog already shown" latch reset, reading NOTHING from the
campaign record. It does not seed the roster. The 112-byte record's field
layout is confirmed from the loader `sub_401085`'s own write offsets: dword
index 13 = levelno @ byte 52, dword indices 22..27 = rovers / rover_speed /
ghosts / ghost_speed / ai_count / ai_difficulty @ bytes
88 / 92 / 96 / 100 / 104 / 108.

## Rover/ghost/AI roster — CORRECTED 2026-07-09

**Rovers and ghosts are NOT player/AI slots.** They are autonomous roaming
map-hazard actors, spawned into a THIRD, separate 100-entry×38-dword table
(`dword_45E020`, allocated/managed by `sub_401914`/`sub_401F76`) — distinct
from both the 10-slot player array (`dword_461BC4`, 38 dwords/152 bytes,
`sub_421DD2`) and the `EXTRA<N>.RES` stage-actor registry (`dword_45E0A8`,
152 bytes×100, dirarrow/warphole/conveyor/trampoline — `docs/re/
stage-actors.md`). The prior port note that folded rover/ghost counts into
COMPUTER player slots ("libs/sim has no separate rover/ghost archetype... every
non-empty count is folded into COMPUTER slots") was a mislabelling based on
`sub_42288C`'s misattributed role above; it has been removed from the port
(`GameApp::load_campaign_stage`, `libs/game/src/game_app.cpp`).

### Spawning — `sub_401AAE`/`sub_401B05` (pseudo.c 4793-4847)

Both take `(count, speed)` and loop `count` times calling `sub_4019C2`
(pseudo.c 4763-4791), which claims a free slot in `dword_45E020`
(`sub_401914`) and places it at a random tile: a candidate
`(cx, cy) = (rand()%W, rand()%H)` (two draws, X first),
accepted when `sub_425FB9(cx, cy) != 1` (rejects SOLID only, code 1 — bricks,
code 2, ARE an acceptable spawn tile; this check is NOT type-dependent the
way the mover's per-tick `sub_4017FA` is, since `dword_45E01C` is only ever
set by `sub_401F76`'s per-tick drive loop, not during spawn) **AND**
`sub_422351(3)` is truthy, retried up to 200 times. The claimed slot's `+4`
dword is set to the caller's type constant (`sub_401AAE`→1=rover,
`sub_401B05`→2=ghost) and `+112` (dword index 28) is set to the caller's
SPEED argument — i.e. `.CAM` fields 4 and 6 (rover_speed/ghost_speed) ARE
consumed, just not by anything sim-side prior to this port: they set the
spawned actor's own per-tick move-budget increment (`+116 += +112 *
frameDelta/frameRef + 100`, the same budget arithmetic as the mover, docs
below).

**`sub_422351(candidateX, candidateY, threshold)` — CONFIRMED 2026-07-09 from
raw disassembly**
(same method as the `sub_4245DA` column-guard re-pin, `docs/re/ai.md` §9.3:
BM95.EXE mapped VA->file offset from its own PE section table — Watcom-
linked, no `.text`/`.data` names, `BEGTEXT`/`DGROUP`/etc — and disassembled
with capstone; no dump retained, `CLAUDE.md`). The decompiler had lost the
function's true 3-argument Watcom register-convention signature (EAX/EDX/
EBX — the same convention that produced the identical class of loss on
`sub_401B5C`'s own register-passed arguments and on the original `sub_4245DA`
misreading) and showed a single EBX argument only; the raw prologue resolves
it fully. `sub_422351` spills all three incoming registers to its own frame
straight away, in this order:

| spilled, in order | from register | is the argument |
|-------------------|---------------|-----------------|
| frame slot `ebp-0x1c` | EAX | candidate tile X |
| frame slot `ebp-0x18` | EDX | candidate tile Y |
| frame slot `ebp-0x14` | EBX | threshold (callers pass 3) |

and the call site inside `sub_4019C2`'s spawn-candidate loop confirms the
same three registers are loaded with the CANDIDATE tile immediately after
the `sub_425FB9` solid check, not with anything player-derived: the three
instructions immediately before the call load EBX with the literal **3**,
EDX with the candidate tile Y (the loop's own `rand()%H` local) and EAX with
the candidate tile X (its `rand()%W` local) — the latter two read straight
back out of the spawn loop's own frame slots.

Inside, the loop over the 10 player slots (`dword_461BC4`, stride 0x98)
computes, per slot: `tileX = sub_42665C(player.+0x1C)`,
`tileY = sub_4266A3(player.+0x20)` (pixel->tile conversions, confirming
these ARE the player's own tile position, unchanged from the earlier
reading), then the Manhattan distance
`d = |candidateX − tileX| + |candidateY − tileY|` — measured from the
CANDIDATE tile (arguments 1 and 2), not the literal
`|tileX| + |tileY|` a single-register misreading might suggest — and
rejects (returns 0) the first slot where `d <=` the threshold argument;
accepts (returns 1) only
if all 10 slots clear the gate. **This pins reading (b) definitively — the
prior "AMBIGUOUS" hedge is resolved, not merely reaffirmed by plausibility.**

**Also newly confirmed: the 10-slot loop has NO presence/liveness guard.**
The disassembly shows no test of any "slot type"/"present"/"alive" field
(the kind `sub_421DD2` exposes elsewhere, e.g. the round-pacing early-out at
`campaign.md` line ~436) before a slot's `+0x1C`/`+0x20` are read and
distance-checked — EVERY one of the 10 `dword_461BC4` records is checked
unconditionally, empty/COMPUTER/dead slots included. An empty or
never-populated slot's raw `+0x1C`/`+0x20` bytes (never written by the
campaign loader for slots beyond the roster) are whatever the array's
static/zeroed backing holds, which tile-converts to a fixed low-magnitude
tile (consistent with a (0,0)-ish origin), so in practice unused slots
contribute a constant, roster-independent near-origin exclusion zone rather
than a real per-player check — an authentic, if minor, engine quirk, not a
port bug to chase further.

### Per-tick mover — `sub_401B5C` (pseudo.c 4839-4977), driven by `sub_401F76`

`sub_401F76` (pseudo.c 4993-5021, called every campaign tick from
`sub_4016DA`) walks all 100 particle-table slots; for any live entry whose
type is 1 or 2 (rover/ghost) it stores the type into `dword_45E01C` (read by
`sub_4017FA` below — the walkability test branches on it) and calls
`sub_401B5C(entry)` — the mover — and counts it into `dword_464820`
(live-rover/ghost count, consumed by the round-pacing clause below). Any
OTHER stray live type is cleared to 0 (defensive; in practice only 1/2 are
ever written by the spawn functions above).

**Struct layout** (offsets used by the mover, decoded from the DWORD-array
writes; the same 38-dword/152-byte stride as the spawn table): `+0`=live
flag, `+4`=type (1 rover/2 ghost), `+8`=dead flag (reaped next
`sub_401F76` pass, mirrors the player `+8` death flag `sub_41DCB2` sets),
`+20/+24`=spawn-tile pixel pos (written once at spawn, read back only by the
one-shot rover init below), `+28/+32`=CURRENT pixel x/y (the position the
mover advances every tick — mirrors the player stepper's `+0x1c/+0x20`),
`+42`=godir in the HIGH WORD (bits 16-31, values 0-3) of that dword, with the
SAME word also addressable directly as a word at `+44` (a little-endian
overlay of the high half of the `+42` dword; confirmed because the
direction-name lookup at the end of the function reads byte 2 of the `+42`
dword, which is the very byte that sits at `+44`), exactly the
16.16 godir field `docs/re/facts.md`'s player-stepper section documents at
player `+0x2c`, `+48`=per-tick step counter (word, feeds the draw-frame
index), `+112`=speed (the spawn call's `speed` arg), `+116`=move budget
(spent 100 units/pixel, same contract as `Player::move_budget`), `+146`=a
ONE-SHOT "already initialised" latch (rover-only, see step 0 below).

0. **One-shot rover spawn cleanup (rover only, NOT ghost).** On the actor's
   very first `sub_401B5C` call (`type==1 && !+146`), BEFORE any movement: set
   the latch, then call `sub_42583B(spawn_tx, spawn_ty, 0)` — for the spawn
   tile and each of its 4 orthogonal neighbours that is walkable
   (`sub_425FB9 != 1`), `sub_425704(tx, ty, 0)` (third argument 0) checks the FLOOR-POWERUP
   grid (`dword_462214`, `sub_42542D`'s own backing store) for a live record
   at that tile; if one exists, it draws up to 200 random tiles
   (`rand()%W,rand()%H` per attempt) looking for one that is a BRICK
   (`sub_425FB9==2`) with no visible powerup there yet, and relocates the
   record there verbatim (a straight 0x98 = 152-byte block copy) — i.e. **a rover's
   landing tile and its neighbours get any powerup silently teleported under
   a random brick** so the rover doesn't spawn standing on one. This is a
   real, RNG-drawing, gameplay-affecting one-shot per rover (never per
   ghost) — up to 5 tiles × up to 200 draw-pairs each, though in practice 0
   draws unless a powerup happens to already occupy the spawn footprint (rare
   at match start, since powerups are hidden under bricks, not on the floor,
   until a wall burns). **Port status: DEFERRED** (see Port status below) —
   documented here in full so a future pass can wire it without re-RE'ing;
   left out of the v1 port because it only fires when a powerup is ALREADY
   sitting exposed on the rover's spawn footprint at spawn time, which no
   existing scenario produces (powerups start hidden, not floored) and
   campaign spawn happens at round start before any brick has burned.
1. **Walkability test is TYPE-DEPENDENT — ghosts phase through bricks,
   rovers do not.** `sub_4017FA(tx,ty)`: blocked by a grounded bomb
   (`sub_422E48`) always; otherwise if `dword_45E01C==2` (current mover is a
   GHOST) the tile is passable unless the collision grid reads exactly `1`
   (solid) — code `2` (brick) is NOT blocking, so **ghosts walk straight
   through bricks**; for a ROVER (or anything else) the tile must read
   exactly `0` (fully blank) — bricks DO block rovers, same as a player. This
   is a genuine, previously-undocumented rover/ghost behavioural difference,
   not a guess: confirmed from the collision-grid fill (`sub_404852`, 0=
   blank/1=solid/2=brick, `docs/re/facts.md` "0=blank, 1=solid, 2=brick
   candidate") and the two distinct comparisons in `sub_4017FA`.
2. **Wander with human-avoidance bias.** At each tile-centre crossing (the
   "along/perp" rotation matches the player stepper's centring math; the
   turn-decision block is jumped over entirely — a single forward branch past
   it, to `LABEL_20` — whenever the
   actor is off-centre on the perpendicular axis, so turns are ONLY decided
   exactly at a tile centre) it walkability-tests the tile straight ahead via
   `sub_4017FA`; if blocked (or, even when clear, with probability
   `1 - 1/max(1,getvalue(1200))`, i.e. `getvalue(1200)=3` ⇒ 2-in-3 chance to
   turn anyway — ONE `rand() % max(1, getvalue(1200))` draw) it randomly turns ±90° (ONE
   `rand()%2` draw: nonzero → `+44 += 1`, zero → `+44 -= 1`, then masked
   `&3`), then re-tests the (now different) ahead tile and if STILL blocked
   zeroes the move budget (stops early, doesn't overshoot into a wall this
   tick — no further draw). VALUELST 1200/1205 (install values 3/3) are
   titled, verbatim, "chance that a ghost or rover will change directions at
   an intersection" / "chance that the direction change will NOT [be]
   towards a human" — confirming a human-seeking bias exists as a documented
   mechanism, but id 1205 has **no read anywhere in `sub_401B5C`** (grepped
   the full function body) — it is read nowhere in the mover at all; the
   "human-avoidance" the VALUELST comment promises is NOT implemented in
   this function (possibly dead/aspirational tuning, or implemented
   elsewhere and never wired to the turn roll — out of scope to chase
   further, but the id is NOT a live input to `sub_401B5C`, correcting the
   earlier hedge here). Draw count per intersection: 1 (turn-chance) or 2
   (turn-chance + coin-flip direction) or 0 (off-centre tick, no roll at
   all) — NOT a fixed count; see the sim port's RNG-order note.
3. **Dies on stepping into active flame.** After moving, `sub_42708D(tileX,
   tileY)` probes the FLAME grid (`dword_46224C`, the SAME array
   `docs/re/facts.md`'s "Flame is NEVER checked" note and the arm-stop fix
   both cite) — if the new tile is currently on fire, the actor is killed
   (`+8 = 1`, reaped next `sub_401F76` pass) and the flame's OWNER (stored at
   the flame record's `+62` word when the flame was written, `sub_426FCC`) is
   awarded points via `sub_421C71`, which adds them into the HIGH WORD of the
   score dword at byte 104 (dword index 26) of that owner's own 152-byte
   player record in `dword_461BC4`.
   VALUELST 1310 (rover, 15 pts) / 1320 (ghost, 25 pts) — both
   explicitly commented "(in campaign mode only)". (id 1300 = 250 pts "for
   killing an AI" is the same field, written by the NORMAL player-death path
   `sub_41DCB2` — pseudo.c 21913-21962, the shared death routine BOTH normal
   flame-death and the rover/ghost kill below funnel through — not traced
   further here beyond confirming it is the SAME award mechanism, just a
   different VALUELST id per killer-kind.) No RNG draw on this branch.
4. **KILLS players on its landing tile — CORRECTED 2026-07-09, this is not a
   punch.** After moving, any live player found at the new tile via
   `sub_421CB5` (`present && !dead(+8) && tile match` — exactly
   `grid::player_at`'s predicate) whose slot type (`+16`) isn't exactly 1
   (COMPUTER/AI) is passed to **`sub_41DE63(actor, -1)`** — traced end to
   end (pseudo.c 21989-22016) and confirmed to be the SAME function the
   normal per-tick flame-probe calls on a walking player
   (pseudo.c 22690-22706, `sub_41DE63(i, flame_owner)`) and the enclosure
   crush calls (`docs/re/facts.md` "stomped_bombs_detonate", `sub_421D3F`→
   `sub_41DE63`) — **it is the player-death entry point, not a stun/
   knockback helper.** Body: early-outs while state-gated (network-spectator
   byte `+16==4`, or mid-bounce/warp states 5/6/7 — `return 0`, no death, no
   RNG); otherwise picks a RANDOM DEATH ANIMATION —
   `rand() % max(1,getvalue(105)) + 1` stored at victim `+4` — **VALUELST 105
   is titled "how many different death animations do we have? (die 1 through
   die 24)", = 24 in the shipped file**, confirming this is a cosmetic
   death-anim index draw, NOT a stun-duration roll as an earlier reading of
   this doc guessed (id 105 was never checked against VALUELST.RES's own
   comment before now) — then calls `sub_41DCB2(victim, killer_or_-1, ...)`,
   the shared death routine (deaths tally `dword_4642B0`++, per-killer kill
   tally `word_461C30[76*killer]`++ when killer != victim slot, the id-1300
   campaign AI-kill-score award when `dword_46489C` is set and the killer
   isn't the victim, sound 300 + an anim trigger, `+8=1` dead flag,
   `+48=0`). **So a rover/ghost stepping onto a HUMAN or NETWORK player's
   tile KILLS them outright** (with owner = -1, i.e. no kill-tally credit —
   the `0 <= killer < 10` guard in `sub_41DCB2` excludes owner `-1` from both
   the per-killer tally and the id-1300 award, so a rover/ghost kill is
   scoreless for the rover/ghost itself; only a FLAME killing the rover/
   ghost awards points, per clause 3, and that's the flame owner's score,
   not the rover's own). AI-controlled players are completely immune — the
   rover/ghost passes through them without incident, no death, no anim roll,
   no RNG. The random-death-anim roll (when it fires) draws exactly ONE
   `rand()`; `sub_41DCB2` itself draws none.
5. Draws itself via `sub_4518D0(buf, aGhostS/aRoverS, personality_byte)` —
   `aGhostS="ghost %s"`/`aRoverS="rover %s"` — then a sequence lookup
   (`sub_41D957`/`sub_41DAA7`), i.e. it IS an on-screen animated sprite, not
   an invisible stat modifier. The personality/direction byte
   (byte 2 of the `+42` dword, i.e. the low byte of the godir high-word —
   masked `&3` at
   the lookup site `sub_413AED`) selects a direction NAME (`off_45BCC4` =
   `{"north","east","south","west"}`) substituted into the format string,
   producing sequence names like `"ghost north"` / `"rover south"`. **No
   ANI file in the shipped install (`D:\...\BOMBRMAN`) contains sequences by
   these names** — grepped every `.RES`/`.ANI` in the install (`BM95.RES`,
   `DATA/RES/*.RES`) for "ghost"/"rover": the only hits are the three
   `.CAM` files themselves (campaign definitions) and the `GHOSTS.CAM` name
   — there is no `GHOST.ANI`/`ROVER.ANI` and no sequence-name string match
   anywhere in the install's binary assets. **The art was never shipped** —
   this is confirmed cut content at the asset level, not just "we didn't
   find it": `resolve_sequence` (`libs/game/src/sprites.cpp:126`) already
   returns an empty `Anim` (no steps) for an unmatched name and callers
   already no-op on an empty `Anim`, so the port's fallback path (a static
   marker sprite / colored tile, see Presentation below) is not a guess
   patched over a gap — it is the correct behaviour for content that has no
   art in ANY known install.

### AI difficulty (field 8) — CONFIRMED dead, not a guess

Grepped the WHOLE decompile for every read of the campaign record's field-8
slot (dword index 27 = byte 108, the loader's own write offset): the
ONLY write is the loader (`sub_401085` pseudo.c 4394); there is no other read
of `dword_45E010`'s field-8 offset anywhere in the binary (the other hits a
raw text grep for that same index expression turns up — pseudo.c
21573/21582/35364 — are
unrelated locals in an ANI-frame-count loop, confirmed by inspecting their
surrounding function — not the campaign record). This is a **confirmed
negative**, not "we didn't find a consumer": the `.CAM` format's own header
comment "(unused at present)" is exactly right, and there is no dormant AI-
personality link either (VALUELST 900 = 1 in the shipped file, so
`rand()%getvalue(900)` always yields personality 0 regardless of any campaign
input — `docs/re/ai.md` §1 — and nothing feeds the campaign record's field 8
into that seed). **Port status: correctly left unapplied; no TODO remains
open on this field.**

### Port status — PORTED 2026-07-09

**Applied:** the AI COUNT (field 7) roster seed now matches `sub_422928`'s
RANDOM-slot-pick shape (`bomber::game::seed_campaign_ai_slots`, `libs/game/
include/bomber/game/results.hpp`, driven by the existing presentation-only
`setup_lcg_`, never `State::rng`) instead of the prior sequential fill.

**Now ported:** rovers/ghosts as a `libs/sim` actor kind — `State::rovers`
(hashed `std::vector<Rover>`), `RoverSystem` (`libs/sim/src/systems/
rovers.{hpp,cpp}`), spawn/wander/flame-death/kill/score, wired from
`MatchConfig` (rover/ghost count+speed, threaded through `libs/match` and
`GameApp::load_campaign_stage`). See "Sim port" below for the exact
placement and the deliberate simplifications:

- **Spawn placement**: `RoverSystem::spawn` mirrors `sub_4019C2` exactly —
  a candidate tile is drawn (`rand()%W, rand()%H`, TWO draws), accepted when
  it is not SOLID (code 1; bricks/code 2 are an acceptable SPAWN tile for
  BOTH rover and ghost — the type-dependent brick rule is a MOVER-only
  distinction, clause 1 above, not a spawn-time one) and the distance-3
  gate (`sub_422351(candidateX, candidateY, 3)`, CONFIRMED above) passes,
  retried up to 200 times. Draws 2 per attempt; a spawn that never finds a
  legal tile draws 400 and silently spawns nothing for that slot, matching
  the original's `return 0` (`sub_401AAE`/`sub_401B05` don't check the spawn
  result before incrementing `i`, so a full board just yields fewer live
  rovers/ghosts than requested — ported the same way). **Discrepancy found
  by the CONFIRMED re-pin**: `sub_422351`'s 10-slot loop has no presence/
  liveness guard (see above), but `RoverSystem::spawn`
  (`libs/sim/src/systems/rovers.cpp`) skips `!p.present || !p.alive` slots
  before computing the distance. Practical effect is negligible (an unused
  original slot contributes only a fixed near-origin exclusion, not a real
  per-player check, per the note above) and the port's filtered version is
  arguably the more sensible behaviour, but it is a literal deviation from
  the disassembly; left as-is here per this task's scope (docs-only —
  flagging for the sim-owning agent rather than editing `libs/sim`
  concurrently).
- **NOT ported**: the one-shot rover-spawn powerup-relocation cleanup
  (mover step 0, `sub_42583B`/`sub_425704`) — deferred, see that step's own
  note; it is a rare, currently-unreachable-in-practice branch (no golden
  scenario spawns a rover onto an already-floored powerup) and adding it
  means extra RNG draws that would need golden coverage to prove are dormant
  — better done as a follow-up with a dedicated test that actually floors a
  powerup under a rover spawn.
- **VALUELST 1205** ("human-avoidance bias") is confirmed NOT read by
  `sub_401B5C` (see clause 2) — correctly left unconsumed; there is no
  "avoid humans" steering to port, despite the tuning id's name.
- `rover_speed`/`ghost_speed` (fields 4/6) ARE now consumed — they set
  `Rover::speed`, the actor's own per-tick move-budget increment, exactly as
  in the original.

## Round pacing — PINNED (`sub_4016DA`, pseudo.c 4612-4651, 2026-07-09)

Per tick, in this exact order:

1. `sub_401F76()` — drive every rover/ghost's mover one tick; this is what
   sets `dword_464820` = the live rover/ghost count read two steps below.
2. If `sub_410578() <= 1` — the survivor-SIDE count, the same query the
   normal round-end uses — then `dword_464894 ← 2`, forcing the round-over
   phase.
3. Branch on `dword_464820`:
   - **non-zero** (rovers/ghosts still alive): `dword_4646C0 ← 0` — hold the
     "all clear" timer pinned at 0;
   - **zero** (none left): `dword_4646C0 += dword_464958` — accumulate
     wall-clock ms — and then, if
     `2 * dword_46494C * getvalue(25) < dword_4646C0`, set
     `dword_464894 ← 1` (~2 s grace elapsed with the map clear → "stage
     clear, pending").
4. Early-out guard, over slots `i` = 0..9 in order: query the slot via
   `sub_421DD2(i, &type, 0)`; if `type != 1` AND `type` is non-zero AND
   `sub_4228C4(i)` — i.e. any NON-COMPUTER, non-empty slot is still alive —
   **RETURN** at once; nothing below runs this tick.
5. Reached only when that loop runs to completion (all humans/network players
   dead): `--dword_4648B0` — undo the pending stage-advance — then
   `dword_464894 ← 2`, forcing immediate round-over instead.

Five distinct clauses, not one:

1. **`sub_401F76()`** is the rover/ghost mover DRIVER (see previous section)
   — this is why `sub_4016DA` must run every tick while campaign is active,
   not just at round boundaries.
2. **`sub_410578()<=1`** is the SAME "at most one survivor side" check the
   normal (non-campaign) round already uses — this is not new logic, just
   applied here too. Our port's existing `sides_remaining(s) <= 1` (
   `run_match`, `game_app.cpp`) already covers this exactly.
3. **The `dword_4646C0` grace timer** ADDS a campaign-only extra condition:
   once every rover/ghost is dead (`dword_464820 == 0`), wait `2 *
   dword_46494C * getvalue(25)` wall-clock ms (== `2 * 50ms * 20` = a FIXED
   2000ms at the shipped VALUELST defaults — id 25's own comment, "how many
   frames... do you have to out-survive the other guy? ...only a nominal
   frame rate used as a REFERENCE for other frame-count-based values", i.e.
   this is not itself a live round timer, just a units constant) before
   flagging `dword_464894 = 1` ("stage clear, pending" — distinct from `=2`,
   "round over now", but both reach the SAME `sub_410B6E()` dialog/advance
   call at the round-tick level, `=2` additionally showing the getstring
   1245/1240 "Campaign unsuccessful!"/"Oh Well!" message per the consumer at
   pseudo.c 29793-29799). **PORTED 2026-07-09** now that rovers/ghosts exist
   sim-side — `State::hazard_clear_timer` (ticks, not wall-clock ms: at the
   locked 20 Hz the original's `2 * dword_46494C(50ms) * getvalue(25)(20)`
   is a fixed 2000ms == 40 ticks, so the port counts ticks directly instead
   of reproducing the ms/frame-delta indirection — see "Sim port" below).
4. **The `for i in 0..9` early-out** re-checks EVERY non-COMPUTER (human or
   network) player slot; if ANY is still alive, the whole function returns
   immediately, skipping clause 5 entirely. This means clause 5 below can
   only fire once every human/network player is already dead.
5. **`--dword_4648B0; dword_464894=2`**: reached only when all humans/
   network players are dead (mutual wipeout or similar) — decrements the
   stage index (undoing the NEXT stage-advance's `++dword_4648B0`, i.e. this
   replays the SAME stage) and forces immediate round-over. This is a
   "campaign doesn't skip a stage just because everyone died" safety net.

**Port status — PORTED 2026-07-09, all 5 clauses.** Clause 2 (survivor count)
was already exactly what our port's existing best-of-N
`sides_remaining(s)<=1`/`ticks_left==0` round-end check implements — no new
code needed there. Clauses 1, 3, 4, 5 are now driven by `RoverSystem` and a
small campaign-pacing helper:

- Clause 1 (`sub_401F76` mover drive) = `RoverSystem::tick`, called from
  `run_tick` — see "Sim port" below for the exact placement.
- Clause 3 (grace timer) = `State::hazard_clear_timer`, incremented by
  `RoverSystem::tick` once `state.rovers` has no live entries, compared
  against a fixed 40-tick threshold (the ms→tick simplification above);
  exposed via `RoverSystem::hazards_just_cleared()` for the campaign layer
  to raise its own "stage clear, pending" flag — kept OUTSIDE `libs/sim`'s
  own win-condition logic (sim has no concept of "campaign stage"), mirrored
  in `GameApp`'s round loop exactly like the original's `dword_464894`
  hand-off to `sub_410B6E`.
- Clauses 4/5 (mutual-wipeout stage replay) are campaign-layer logic (need
  "is this campaign mode", not a sim concept) — the predicate itself is
  `bomber::game::campaign_round_needs_replay` (`results.hpp`, SDL-free,
  doctested in `test_frontend.cpp`: all-COMPUTER-alive replays, a single live
  human/joystick slot blocks it, a dead or absent human slot does not, and an
  empty roster replays vacuously), wired into `GameApp::run_app` via the
  `campaign_no_human_survivor()` wrapper (gathers `sim::State::players[]`
  present/alive plus `setup_type_[]` into the arrays the predicate needs). On
  round end, `w = round_winner()` is overridden to `-1` (a plain draw) the
  instant `campaign_no_human_survivor()` holds — even when `round_winner()`
  itself returned a "winner" (a COMPUTER-only survivor) — so the round is
  routed into the SAME draw/replay branch a mutual total wipeout already
  used. `campaign_stage_index_` is only ever incremented in the `w>=0`
  match-over branch, which the override already excludes, so "replay a
  round" on this path is exactly "replay the same campaign stage" for free —
  matching `--dword_4648B0` before the immediate round-over without a
  separate decrement.

Clause 2 needed no change. See "Sim port" and "Presentation" below for file-
level detail.

## Stage banner — CONFIRMED (`sub_40133F`, pseudo.c 4443-4504) and PORTED 2026-07-09

`sub_40133F` (called from `sub_410B6E`, the same end-of-round/stage-advance
handler `dword_464894` feeds into) is the stage-transition banner:

In order:

1. **Gate**: campaign active (`dword_46489C`) AND, with the stage index
   PRE-incremented as part of the test, `++dword_4648B0 < dword_45E014` (the
   loaded stage count). The increment is part of the condition, so the index
   advances even on the run where the comparison then fails.
2. **If a next stage exists** (both halves true), in order:
   - `record = dword_45E010 + 112 * dword_4648B0` — this stage's record;
   - `dword_464998 ← record[+52]` — levelno (field 1), stored to a global
     with no further traced consumer;
   - `sub_4518D0(buf, getstring(1235), record)` — getstring(1235) = `"(%s)"`;
     the record's own first bytes are the stage NAME (field 0);
   - `sub_414340(getstring(1230), …, byte_49D38F)` — getstring(1230) =
     `"Prepare to begin Campaign!"`, a blocking dialog.
3. **Else, if campaign is active** (stage list exhausted), in order:
   - `sub_414340(getstring(1220), …)` — `"Congratulations!"` /
     `"You made it through…"` (1225);
   - `dword_464A68 ← 10`.

String table (`MESSAGES.TXT`, confirmed against the install):
`1235="(%s)"`, `1230="Prepare to begin Campaign!"`, `1220="Congratulations!"`,
`1225="You made it through the whole campaign!"`, `1240="Oh Well!"`,
`1245="Campaign unsuccessful!"`. `sub_414340` is a generic modal two-line
message-box builder (blocks for a keypress; no fixed on-screen duration in
the original) — the SAME dialog family `sub_4015C6`'s own post-pick
confirmation overlay uses (getstring 1210+95), which this port's
`present_campaign_picker` already stands in for with a sound sting.

**Ported**: `GameApp::present_campaign_banner()` (`libs/game/game_app.hpp`/
`.cpp`) shows `"(<stage name>)"` over `"Prepare to begin Campaign!"`
(`campaign_banner_`, set by `load_campaign_stage`) as a blocking-with-dwell
two-line overlay (any key dismisses immediately; a 2s dwell auto-advances so
an unattended stage-transition doesn't stall). Called both when the picker
arms stage 0 (`present_campaign_picker`) and on every subsequent stage
advance (`run_app`'s Results handler, replacing the prior "no on-screen
reproduction" gap). The stage-exhausted variant (getstring 1220/1225,
"Congratulations!") is NOT ported — our port's existing "stage list
exhausted -> clear campaign, fall through to the menu" path (`campaign_
active_ = false`) has no dedicated dialog of its own either, consistent
with how the port already treats that transition; flagged as a smaller
follow-up alongside `sub_4016DA`'s remaining round-pacing gap rather than
addressed here.

## Campaign-activation confirmation dialog — PORTED 2026-07-09 (`sub_4015C6`)

`sub_4015C6` (pseudo.c 4550-4598, the same function that opens the `*.cam`
picker and sets `dword_46489C = 1`) shows a two-line acknowledgement dialog
right after a successful pick/parse, BEFORE the stage-0 banner:

The sequence, in order:

1. `sub_401085(picked_path)` — load the chosen `.CAM` file.
2. `sub_4124A4(1210)` — getstring(1210) = `"Campaign Mode Activated!"`. Its
   result comes back in EAX and is parked in EDX, surviving the next call.
3. `sub_4124A4(95)` — getstring(95) = `"NOTE!"`, returned in EAX.
4. `sub_414340(<the EDX:EAX string pair>, byte_49D38F)` — the two-line modal,
   white ink. Its first parameter is one 64-bit value carried in the register
   PAIR EDX:EAX (low half in EAX, high half in EDX).
5. `dword_46489C = 1` — campaign active.

**Line order — CONFIRMED from `sub_414340` itself** (pseudo.c 17004-17108;
its first parameter is the EDX:EAX pair described above): the two-line branch
draws the pair's LOW half FIRST, at `y = fontheight + 32` (the TOP line), then
its HIGH half SECOND, at `y = <that top y> + fontheight + 2` (the BOTTOM
line) — two consecutive `sub_4172BA` text draws, low-half-at-top-y followed by
high-half-at-bottom-y. Since `sub_4015C6` explicitly puts `getstring(95)` in
the LOW half (unambiguous — no register-loss hedge needed here, unlike the
`.CAM`-record cases elsewhere in this doc), **getstring(95)="NOTE!" is the TOP
line**; the HIGH half is whatever the compiler carried over from the PREVIOUS
`sub_4124A4(1210)` call (the standard two-sequential-calls-into-two-registers
pattern — the first call's EAX is preserved into EDX before the second call
clobbers EAX), so **getstring(1210)="Campaign Mode Activated!" is the BOTTOM
line**.

Geometry is the SAME `sub_43C734` chrome primitive as the quit-confirm dialog
(`DialogRect`/`draw_dialog_chrome`, `game_app.cpp`), sized from `sub_414340`'s
own two-line branch: `width = max(max(measure(top), measure(bottom)), 80) +
64`, `height = 4*fontheight + 64 + 2*fontheight`, centered on both axes
(`(640-width)/2`, `(480-height)/2` — `sub_41456C`, the sibling one-line
variant, computes the same centering explicitly at pseudo.c 17197, confirming
the pattern this two-line sibling shares). Ink is `byte_49D38F` (white) for
both lines, the same ink every other `sub_414340`/`sub_41456C` call site in
this file uses.

**Dismiss keys** — traced from `sub_414340`'s own key loop (pseudo.c
17085-17106): every real key event plays the nav-blip (`sub_427961(20)`);
only `Enter(13)`/`Space(32)`/`Escape(27)` close the dialog; any OTHER key
(arrows, letters) just loops, waiting for another key. No Yes/No choice — a
plain acknowledgement modal, identical shape to `sub_41456C`'s Escape/Space/
Enter handling the quit-confirm dialog already ports.

**Ported**: `GameApp::present_campaign_confirm()` (`game_app.cpp`), called
from `present_campaign_picker` right after `campaign_active_ = true` and
before `present_campaign_banner()` — replacing the former accept-sting
stand-in this dialog previously approximated (`coverage-audit.md` TODO(RE)
crumb, now closed).

## Campaign-exit key — CONFIRMED negative (no dedicated key exists)

`docs/re/campaign.md`'s prior text (and `ROADMAP.md`'s campaign entry) left
open whether the original has a dedicated key/action that explicitly clears
`dword_46489C` when leaving campaign mode early (e.g. Escape on the
player-setup screen after a `*.cam` pick but before a match starts). Grepped
**every** read and write of `dword_46489C` across the whole decompile
(pseudo.c): it is read at ~12 sites (gates in `sub_40133F`, `sub_4016DA`,
`sub_406DDE`'s map-select skip, `sub_41DCB2`'s campaign kill-score award,
etc. — see "What campaign mode actually does once active" above) but
**written in exactly two places, total**:

1. `sub_4015C6` (pseudo.c 4585): `dword_46489C = 1` — set on a successful
   `*.cam` pick.
2. `sub_42A3F6` (pseudo.c 29692): `dword_46489C = 0` — the LITERAL FIRST
   executable statement of the function, unconditionally, every time it
   runs. `sub_42A3F6` is the already-confirmed "Play" entry point (calls
   `sub_410F81`, the PLAYER INPUT screen, right after this reset — pseudo.c
   29693-29697).

**There is no third write anywhere** — no Escape handler, no dedicated
"leave campaign" key, no cleanup path in the round-loop or results screen
clears `dword_46489C` directly. The original's own Escape-on-setup (and any
mid-match abort) just routes `dword_464A68` (a menu-return sentinel written
at dozens of other screens' Escape handlers, e.g. pseudo.c 6092/6564/7156)
back to the menu; `dword_46489C` is left stale until the NEXT "Play" click
resets it unconditionally at `sub_42A3F6`'s entry — which is behaviourally
invisible, since that stale value is never read before being overwritten by
the same function that would read it next.

**Port status**: the port's Esc-on-setup clear (`campaign_active_ = false`,
`present_setup`, `game_app.cpp`) and the equivalent unconditional reset now
added at the Menu -> StartMatch transition (`run_app`'s `AppState::Menu`
case, mirroring `sub_42A3F6`'s entry reset exactly) together reproduce the
identical observable behaviour via explicit resets instead of the original's
implicit "next entry overwrites stale state" — a faithful convenience, not a
guess dressed up as a fact. The entry-point reset closes a real port gap the
Esc-only clear did not cover: aborting mid-campaign from `run_match`'s own
Esc/Ctrl+Q (which, matching the original, does not touch `campaign_active_`
either) previously left `campaign_active_`/`campaign_stages_`/
`campaign_stage_index_` armed for the next "Play", silently resuming the
abandoned stage instead of starting a normal game.

## `.CAM` format — confirmed against RE-NOTES.md

RE-NOTES.md's asset-format table already marks `.CAM` "✅ Campaign/stage
info, commented" (plain text). Checked directly against
`DATA/RES/SIMPLE.CAM` in the install and the loader body (`sub_401085`):
matches exactly. Header comments in the file self-document the 9
comma-separated fields per stage line:

```
; Field descriptions:
;	0. campaign name
;	1. levelno
;	2. scheme to use
;	3. number of rovers
;	4. rover speed
;	5. number of ghosts
;	6. ghost speed
;	7. number of AIs
;	8. AI difficulty (0-100) (unused at present)

-C,Just One Ghost,           1,basic,    0,  0, 1,150, 0, 50
```

Each stage line starts with the literal marker `-C` (checked byte-for-byte
by the loader: `'-'` then case-insensitive `'C'`), then the 9 fields above,
comma-split, trailing/leading whitespace trimmed per field (`sub_4516C1` —
matches `strncpy`-style field copy at pseudo.c 4379-4394). No binary
sections, no versioning, exactly as trivial as RE-NOTES.md said. The
install ships 3 files (`CROUTON.CAM`, `GHOSTS.CAM`, `SIMPLE.CAM`), each a
short handful of stage lines — small, hand-authored content, consistent
with a hidden/unadvertised feature rather than a built-out campaign mode.

## Verdict

**REACHABLE.** Confirmed end-to-end from the main menu through a real,
non-trivial subsystem: `sub_410F81` (player-setup screen, `docs/re/
setup-screens.md`) → 5× 'C' keypress in a local game → `sub_4015C6` (`*.cam`
file picker) → `sub_401085` (loader) → `dword_46489C` gates level-select
skip, automatic stage advance, round-pacing, and roster auto-fill for the
rest of the session. Not a constant-false-gated dead branch, not an unused
string — a hidden, fully-wired feature reachable only via an undocumented
key-mash easter egg. The earlier "may be vestigial" hedge in
`coverage-audit.md` is resolved: the two weak string refs were a red
herring for scope (they're the loader's user-facing messages, and the
loader is genuinely called), not evidence of dead code.

**Port implication:** low priority is still the right call (3 shipped
files, no menu discovery path, joke-named content, no player-facing
documentation of the feature anywhere in the original's own UI) but it is
now correctly classified as a small **feature to port**, not a **stub to
ignore**. If/when picked up: reproduce the 'C'×5 trigger on the player-setup
screen (local games only), the `*.cam` picker dialog, the `sub_401085`
9-field parser (trivial — a `libs/assets` text parser, same tier as
MESSAGES.TXT), and the `dword_4648B0` stage-advance/round-pacing loop as a
small campaign-mode state machine sitting alongside (not inside) the sim —
none of `sub_4016DA`'s logic touches deterministic gameplay state, it only
sequences which match config runs next, so it belongs in `libs/game` or a
thin campaign layer above `libs/match`, not in `libs/sim`.

## Sources

- `sub_401085` @ 0x401085 (pseudo.c 4325-4416) — `.CAM` parser/loader.
- `sub_4015C6` @ 0x4015C6 (pseudo.c 4550-4598) — `*.cam` file picker,
  campaign-active flag setter, roster seeder.
- `sub_410F81` @ 0x410F81 (pseudo.c 14924; trigger at pseudo.c
  15357-15365) — PLAYER INPUT TYPE SELECTION screen, hosts the 'C'×5
  trigger; full screen RE in `docs/re/setup-screens.md`.
- `sub_401312`/`sub_40133F` (pseudo.c 4430-4508) — campaign stage-advance
  hooks; `sub_40133F` also formats/shows the stage banner (getstring
  1235/1230), called from `sub_410B6E` (pseudo.c 14688).
- `sub_4016DA` (pseudo.c 4612-4651) — per-tick campaign round-pacing,
  called from the round-tick callback `sub_42A191` @ 0x42A191 (pseudo.c
  29528-29529; registered via `sub_43A6FC` at 29701) while `dword_46489C`
  is set. Full 5-clause breakdown: "Round pacing — PINNED" above.
- `sub_40151B` (pseudo.c 4529-4544) — the REAL per-stage roster/actor
  seeder (gated `dword_46489C`), calling `sub_42288C` (per-slot UI-latch
  clear, NOT a roster seed — pseudo.c 24755), `sub_422928` (random AI-slot
  activation, pseudo.c 24776), `sub_401994` (round-timeout reset, pseudo.c
  4749), `sub_401B05`/`sub_401AAE` (ghost/rover spawn, pseudo.c 4816/4793).
- `sub_4019C2`/`sub_401914` (pseudo.c 4763/4722) — particle-table slot
  claim + random walkable-tile placement for a spawned rover/ghost.
- `sub_401F76` (pseudo.c 4993-5021) — per-tick rover/ghost mover driver,
  sets `dword_464820` (live rover/ghost count).
- `sub_401B5C` (pseudo.c 4839-4977) — rover/ghost per-tick mover: wander +
  human-avoidance bias (VALUELST 1200/1205), flame-death + kill-score award
  (VALUELST 1300/1310/1320, `sub_42708D`/`sub_421C71`), player knockback
  (`sub_41DE63`).
- `dword_45E020` — the rover/ghost particle-actor table (100×38 dwords),
  DISTINCT from the player array `dword_461BC4` (`sub_421DD2`) and the
  `EXTRA<N>.RES` stage-actor registry `dword_45E0A8` (`docs/re/
  stage-actors.md`).
- VALUELST ids: 25/30 (frame-rate reference, `docs/valuelst-map.md`), 1200/
  1205 (rover/ghost direction-change chance / human-avoidance bias), 1300/
  1310/1320 (campaign kill-score: AI/rover/ghost), 900 (AI personality
  count, confirmed =1 in the shipped file ⇒ no live personality variance —
  `docs/re/ai.md` §1). All confirmed against `DATA/RES/VALUELST.RES`'s own
  comments in the install.
- `dword_46489C` (campaign-active flag) read sites: pseudo.c 4433, 4458,
  4527, 8087, 15315, 21936, 22877, 24051, 24317, 29528, 29776, 29793.
- String constants: `aTotalOfUCampai` (pseudo.c 1309), `aCouldnTOpenCam`
  (pseudo.c 1310), `aCam` (pseudo.c 1311), `aSSSCam` (pseudo.c 1452),
  `aGhostS`/`aRoverS` (pseudo.c 1312-1313, "ghost %s"/"rover %s").
- Format cross-check: `DATA/RES/SIMPLE.CAM` (install), `docs/RE-NOTES.md`
  asset-format table (`.CAM` row), `MESSAGES.TXT` ids 1210/1215/1220/1225/
  1230/1235/1240/1245/95/97 (install).
- Distinguished from the unrelated "campaign easter egg" mislabel in
  `docs/re/frontend-flow.md` (main menu Ctrl+E×6 → map editor,
  `sub_40330E`), corrected in `docs/re/results-and-options.md` §5.
