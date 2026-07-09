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
   two-byte marker `-C` (pseudo.c 4374: `v13 == 45 /* '-' */ && toupper() ==
   67 /* 'C' */ && j == 9`).
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
   ```
   if (v107 <= 0x43)          // key code <= 'C' (0x43 = 67 = ASCII 'C')
     if (!sub_40C06A() && ++v115 == 5)   // not net mode, 5th consecutive 'C'
       sub_4015C6();          // open the campaign picker
   ```
   i.e. pressing **'C' five times in a row** (any other key resets the
   counter `v115` to 0, same pattern as the confirmed `results-and-
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

```c
if (dword_46489C) {
  v6 = (int*)(112*dword_4648B0 + dword_45E010);   // this stage's record
  dword_464894 = 0;                                // reset round-phase flag
  for (i = 0; i < 10; ++i) sub_42288C(i);           // clear per-slot UI latch (NOT roster seed)
  for (j = 0; j < v6[26]; ++j) sub_422928();        // v6[26] = ai_count (field 7)
  sub_401994(this);                                 // reset round-timeout: dword_464820=1, dword_4646C0=0
  sub_401B05(v6[24], v6[25], v1);                   // v6[24]/[25] = ghosts/ghost_speed (fields 5/6)
  return sub_401AAE(v6[22], v6[23], v2);            // v6[22]/[23] = rovers/rover_speed (fields 3/4)
}
```

`sub_42288C(i)` (pseudo.c 24755): `if (byte_461BD4[152*i]==1) byte_461BD4[152*i]=0;`
— a per-slot "dialog already shown" latch reset, reading NOTHING from the
campaign record. It does not seed the roster. The 112-byte record's field
layout is confirmed from the loader `sub_401085`'s own write offsets
(`v20[13]`=levelno@byte52, `v20[22..27]`=rovers/rover_speed/ghosts/
ghost_speed/ai_count/ai_difficulty@bytes 88/92/96/100/104/108).

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

Both take `(count, speed)` and loop `count` times calling `sub_4019C2`, which
claims a free slot in `dword_45E020` (`sub_401914`) and places it at a random
WALKABLE tile (`rand()%board_w/h`, retried up to 200 times, rejecting
non-walkable via `sub_425FB9`/`sub_422351`). The claimed slot's `+1` byte is
set to the caller's type constant (`sub_401AAE`→1=rover, `sub_401B05`→2=ghost)
and `+28` (dword) is set to the caller's SPEED argument — i.e. `.CAM` fields 4
and 6 (rover_speed/ghost_speed) ARE consumed, just not by anything sim-side:
they set the spawned actor's own per-tick move-budget increment.

### Per-tick mover — `sub_401B5C` (pseudo.c 4839-4977), driven by `sub_401F76`

`sub_401F76` (pseudo.c 4993-5021, called every campaign tick from
`sub_4016DA`) walks all 100 particle-table slots; for any live entry whose
type is 1 or 2 (rover/ghost) it calls `sub_401B5C(entry)` — the mover — and
counts it into `dword_464820` (live-rover/ghost count, consumed by the round-
pacing clause below). Any OTHER stray live type is cleared to 0 (defensive;
in practice only 1/2 are ever written by the spawn functions above).

`sub_401B5C` is a full per-pixel stepper (same `+112`=speed, `+116`=move-
budget, "100 units per pixel" shape as the player/bomb steppers, `docs/re/
stage-actors.md` §3's budget arithmetic):

1. **Wander with human-avoidance bias.** At each tile-centre crossing it
   walkability-tests (`sub_4017FA`) the tile straight ahead; if blocked (or,
   even when clear, with probability `1 - 1/max(1,getvalue(1200))`, i.e.
   `getvalue(1200)=3` ⇒ 2-in-3 chance to turn anyway) it randomly turns ±90°
   (`rand()%2` ? `+44 += 1` : `+44 -= 1`, masked `&3`). VALUELST
   1200/1205 (install values 3/3) are titled, verbatim, "chance that a ghost
   or rover will change directions at an intersection" / "chance that the
   direction change will NOT [be] towards a human" — confirming a human-
   seeking bias exists as a documented mechanism, though the exact use of
   id 1205 is not disassembled further here (out of scope: no sim consumer
   exists to wire it into).
2. **Dies on stepping into active flame.** After moving, `sub_42708D(tileX,
   tileY)` probes the FLAME grid (`dword_46224C`, the SAME array
   `docs/re/facts.md`'s "Flame is NEVER checked" note and the arm-stop fix
   both cite) — if the new tile is currently on fire, the actor is killed
   (`+8 = 1`, reaped next `sub_401F76` pass) and the flame's OWNER (stored at
   the flame record's `+62` word when the flame was written, `sub_426FCC`) is
   awarded points via `sub_421C71`: `HIWORD(dword_461BC4[38*owner+26]) +=
   points`. VALUELST 1310 (rover, 15 pts) / 1320 (ghost, 25 pts) — both
   explicitly commented "(in campaign mode only)". (id 1300 = 250 pts "for
   killing an AI" is the same field, presumably written by the NORMAL
   player-kill path elsewhere, not traced further here — out of scope.)
3. **Knocks players off its landing tile.** After moving, any OTHER actor
   found at the new tile via `sub_421CB5` (a live, non-team-locked PLAYER
   lookup) whose slot type (`+16`) isn't exactly 1 (COMPUTER) is punched
   (`sub_41DE63(actor, -1)` — the same head-stun/knockback helper used
   elsewhere, `getvalue(105)` stun-duration roll) — i.e. rovers/ghosts shove
   HUMAN and NETWORK players out of their tile on contact, but pass through
   AI-controlled players without incident.
4. Draws itself via `sub_4518D0(buf, aGhostS/aRoverS, personality_byte)` —
   `aGhostS="ghost %s"`/`aRoverS="rover %s"` — then a sequence lookup
   (`sub_41D957`/`sub_41DAA7`), i.e. it IS an on-screen animated sprite, not
   an invisible stat modifier.

### AI difficulty (field 8) — CONFIRMED dead, not a guess

Grepped the WHOLE decompile for every read of the campaign record's field-8
slot (`v20[27]`/`v6[27]` at the loader's own write offset, byte 108): the
ONLY write is the loader (`sub_401085` pseudo.c 4394); there is no other read
of `dword_45E010`'s field-8 offset anywhere in the binary (the other
`v6[27]`/`v20[27]` hits found by a raw grep, pseudo.c 21573/21582/35364, are
unrelated locals in an ANI-frame-count loop, confirmed by inspecting their
surrounding function — not the campaign record). This is a **confirmed
negative**, not "we didn't find a consumer": the `.CAM` format's own header
comment "(unused at present)" is exactly right, and there is no dormant AI-
personality link either (VALUELST 900 = 1 in the shipped file, so
`rand()%getvalue(900)` always yields personality 0 regardless of any campaign
input — `docs/re/ai.md` §1 — and nothing feeds the campaign record's field 8
into that seed). **Port status: correctly left unapplied; no TODO remains
open on this field.**

### Port status

**Applied:** the AI COUNT (field 7) roster seed now matches `sub_422928`'s
RANDOM-slot-pick shape (`bomber::game::seed_campaign_ai_slots`, `libs/game/
include/bomber/game/results.hpp`, driven by the existing presentation-only
`setup_lcg_`, never `State::rng`) instead of the prior sequential fill.
**Deliberately NOT ported (scope call, not an oversight):** rovers/ghosts
themselves — a real `libs/sim` feature (a new actor kind: spawn, wander-AI,
flame-death, player-knockback, hashed state, golden recapture, art). This is
substantially larger than "wire up a speed number" and is out of scope for
this doc-and-port pass; flagged as follow-up work rather than silently
punted. `rover_speed`/`ghost_speed` (fields 4/6) have no effect until that
actor kind exists — they are real inputs to the ORIGINAL (an actor's move-
budget increment), just with no sim-side consumer in this port yet.

## Round pacing — PINNED (`sub_4016DA`, pseudo.c 4612-4651, 2026-07-09)

```c
int sub_4016DA() {
  sub_401F76();                      // drive every rover/ghost's mover 1 tick; sets dword_464820 = live count
  if (sub_410578() <= 1)             // survivor-SIDE count (same query the normal round-end uses)
    dword_464894 = 2;                // force round-over phase
  if (dword_464820) {
    dword_4646C0 = 0;                // rovers/ghosts still alive: hold the "all clear" timer at 0
  } else {
    dword_4646C0 += dword_464958;    // no rovers/ghosts left: accumulate wall-clock ms
    if (2*dword_46494C*getvalue(25) < dword_4646C0)
      dword_464894 = 1;              // ~2s grace elapsed with the map clear -> "stage clear, pending"
  }
  for (i = 0; i < 10; ++i) {         // early-out guard: is any NON-COMPUTER slot still alive?
    sub_421DD2(i, &type, 0);
    if (type != 1 && type && sub_4228C4(i)) return;   // yes -> bail, nothing below runs this tick
  }
  --dword_4648B0;                    // all humans/network players dead: undo the pending stage-advance
  dword_464894 = 2;                  // and force immediate round-over instead
}
```

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
   pseudo.c 29793-29799). **Not ported**: this clause only makes sense once
   rovers/ghosts exist sim-side (its whole purpose is "wait a couple seconds
   after the last monster dies before ending the stage") — see the roster
   section's port-status note.
4. **The `for i in 0..9` early-out** re-checks EVERY non-COMPUTER (human or
   network) player slot; if ANY is still alive, the whole function returns
   immediately, skipping clause 5 entirely. This means clause 5 below can
   only fire once every human/network player is already dead.
5. **`--dword_4648B0; dword_464894=2`**: reached only when all humans/
   network players are dead (mutual wipeout or similar) — decrements the
   stage index (undoing the NEXT stage-advance's `++dword_4648B0`, i.e. this
   replays the SAME stage) and forces immediate round-over. This is a
   "campaign doesn't skip a stage just because everyone died" safety net.

**Port status.** Clause 2 (survivor count) is already exactly what our port's
existing best-of-N `sides_remaining(s)<=1`/`ticks_left==0` round-end check
implements — no new code needed, it was already right for the reason stated
here rather than the reason the pre-2026-07-09 TODO guessed. Clauses 1 and 3
require rovers/ghosts to exist sim-side and are deliberately deferred with
them (see previous section's port-status note) — reusing best-of-N clinch as
"stage done" remains the correct stand-in until then, now for a PINNED
reason instead of an unpinned guess. Clause 5 (mutual-wipeout stage replay)
is a small independent behaviour that COULD be ported without rovers/ghosts
(it only needs "did every human/network slot die"), but is left for a
follow-up alongside the rest of this function rather than split out, since
on its own it's an edge case (mutual wipeout while the map is already clear
of monsters) with no test coverage pressure yet.

## Stage banner — CONFIRMED (`sub_40133F`, pseudo.c 4443-4504) and PORTED 2026-07-09

`sub_40133F` (called from `sub_410B6E`, the same end-of-round/stage-advance
handler `dword_464894` feeds into) is the stage-transition banner:

```c
if (dword_46489C && ++dword_4648B0 < dword_45E014) {
  v8 = 112*dword_4648B0 + dword_45E010;         // this stage's record
  dword_464998 = *(int*)(v8 + 52);              // levelno (field 1) -> a global, no further traced consumer
  sub_4518D0(buf, getstring(1235), v8);         // getstring(1235) = "(%s)"; v8's first bytes = stage NAME (field 0)
  sub_414340(getstring(1230), ..., byte_49D38F);// getstring(1230) = "Prepare to begin Campaign!"; blocking dialog
} else if (dword_46489C) {                      // stage list exhausted
  sub_414340(getstring(1220), ...);             // "Congratulations!" / "You made it through..." (1225)
  dword_464A68 = 10;
}
```

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
