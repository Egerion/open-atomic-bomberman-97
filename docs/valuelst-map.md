# VALUELST.RES — gameplay value map

`DATA/RES/VALUELST.RES` is a commented, plain-text `id,value` list containing the game's tuning constants. This is our own condensed map of the ids the sim consumes or will consume; open the original file for the full authored commentary.

Units: speeds are hundredths of a pixel per frame; probabilities are 1-in-N; frame counts assume the nominal frame rate below.

See `docs/re/id-audit.md` for the full data-driven cross-check this file was
reconciled against 2026-07-09 (every `getvalue` call site vs every id vs
every port consumer), including the SOUNDLST.RES side and the fixed
post-death-taunt range bug.

## Consumed by `sim::Tuning` today

Verified 2026-07-09 against `libs/sim/include/bomber/sim/tuning.hpp`'s actual
`Tuning::apply` switch/range table (`docs/re/id-audit.md`) — this table had
drifted from the code (it was missing several ids `apply` already handles,
and wrongly implied 25/30/31 route through `apply` when they're hardcoded
mirrors instead; both fixed below). Since 2026-07-16 id 30 DOES also route
through `apply` — not as a frame rate, but as the round-start input-freeze
tick count its round-init consumer derives from it (see its row).

| id | meaning | original value |
|---|---|---|
| 30 | nominal frames/second target ("how many frames per second are we gonna attempt to get?") — hardcoded-mirrored as the 20 Hz tick (see below), AND consumed by round init as the round-start input freeze: `dword_4621E0 = 50ms × getvalue(30)` ≈ 1 s of dead input/AI at every round start (`Tuning::input_freeze_ticks`, `docs/re/facts.md` "Round-start input freeze") | 20 |
| 41 | bomb fuse length, frames | 40 (= 2 s) |
| 42 | starting walk speed | 923 |
| 90 | speed added per skate | 150 |
| 91 | clogs speed penalty — the file's legend calls it the "special roulette power-down"; subtracted per clogs count in the SAME walk-speed term as id 90 (mirror image, `sub_41F29B` pseudo.c 23430-23440), before disease scaling. Inventory slot 13, the Goldman wheel's booby prize — wheel-only, never a normal pickup (`docs/re/goldman-roulette.md` §9) | 150 |
| 300 | kicked bomb speed | 1000 |
| 301 | punched bomb speed | 1300 |
| 100 | match length, seconds | 150 |
| 95 | post-death taunt chance, 1-in-N (`sound_director.cpp`'s `PlayerDied` handler; SOUNDLST 700-999 group — id-audit.md fixed the range, was wrongly 500-999) | 5 |
| 101 | in-round "hurry" threshold, seconds remaining | 60 |
| 102 | window (seconds, from round start) during which a brick-hidden Punch/Grab/SuperDisease ("over-powerful powers", the file's own comment) token relocates instead of revealing when its brick burns. `FlameSystem::relocate_overpowered_here`; disassembly-resolved 2026-07-10 (`sub_425107`'s early gated branch, previously flagged "possibly undefined") — `docs/re/facts.md` "Overpowered-powerup relocation" | 40 |
| 27 | default enclosement depth (0 none / 1 = 2 rows / 2 = 4 rows / 3 = all the way) | 1 |
| 46 | closing enclosement wall lands on a grounded bomb: 1 = detonate (queued full explosion), 0 = silently destroy (`sub_426818` ~27260). The DEFAULT seed of the "Stomped Bombs Detonate" option: `sub_41095A` sets dword_464940 = getvalue(46), then options.ini `stomped_bombs_detonate=` / the Options row override it — `docs/re/facts.md` "Options toggles" | 1 |
| 20 | brick disintegration animation, frames | 10 |
| 10 | flame lifetime / regular-flame animation cycle, frames | 10 |
| 660, 661 | punched-bomb arc: initial three-tile bounce height / subsequent one-tile hop height, px | 65 / 20 |
| 665 | movement pause when grabbing a bomb, ticks | 2 |
| 667 | flying jelly veers ±90° at a 0,0 intersection, 1-in-N | 3 |
| 670, 671 | powers lost on a head hit: minimum / additional-random-modulus | 1 / 3 |
| 320–323 | dud-bomb gate: base SECONDS / additional random SECONDS / 1-in-N chance / fizzle duration frames. 320/321 are converted ×`kTicksPerSecond` at consumption (`BombSystem::place` re-arm, `setup.cpp` initial arm) — one dud opportunity per 3–6 **minutes**; the re-arm `+=`s the previous deadline (`sub_422C13`). Corrected 2026-07-10 (`docs/re/facts.md` "Dud bombs" — the old port read them as ticks, duds ~20× too frequent) | 180 / 180 / 3 / 120 |
| 50–62 | starting inventory per powerup kind | 1 bomb, 2 flame, rest 0 |
| 400–412 | powerups hidden under bricks per match (negative N = \|N\| tries at 1-in-10) | 10,10,3,4,8,2,2,1,−2,−4,1,−4,−2 |
| 550–562 | per-player accumulation caps (0 = uncapped) | 8,8,0,1,4,1,1,1,1,1,1,0,0 |
| 189 | number of conveyor speeds (count) | 3 |
| 190–192 | conveyor belt speeds low/med/high (1/100 px, same budget units as id 42); selected by the "Conveyor Speed" game option (default 1=medium; this install's options.ini=2) | 250 / 350 / 450 |
| 680 | trampoline bounce length, frames ("how many frames do you bounce"; `sub_41F29B` ~23160) | 30 |
| 900 | number of predefined AI personalities (brain-init spread `rand()%900`; =1 ⇒ every brain is personality 0) — `docs/re/ai.md` §1/§6 | 1 |
| 910 | closing-wall ("fire-god") danger look-ahead, tiles — the AI danger grid marks the next 910 spiral bricks with a decaying threat (`docs/re/ai.md` §4.3) | 15 |
| 915 | blast-bricks drop chance, 1-in-N (behaviour 3 `sub_40AD8D`, Stage 4) — `docs/re/ai.md` §3.3 | 5 |
| 920 | powerup-seek range/BFS depth (behaviour 5 `sub_40BAF5`, Stage 3) — how close a powerup must be for an AI to chase it | 4 |
| 120 | "can diseases be blown up like all other powerups?" (`gbl_diseases_can_be_destroyed`). DEFAULT seed of the "Diseases Can Be Destroyed" option (dword_464990 = getvalue(120), overridden by `diseases_destroyable=`); 0 ⇒ a destroyed floor skull relocates via `sub_4255B2(2)` instead of being lost — `docs/re/facts.md` "Options toggles" | 1 |
| 123–125, 129, 130–138 | disease behavior flags, cure chance, freshness, per-disease durations (`docs/re/facts.md` "Disease system") | 1/1/10/10/300×9 |
| 121 | **DEAD IN THE ORIGINAL — the port no longer consumes it** (diagnosed 2026-07-28, PORT FIXED 2026-07-30; `docs/re/facts.md` "VALUELST id 121 is dead in the original"). `Tuning::diseases_time_limited` survives as an INERT field because it is on the `MatchConfig` wire, but nothing reads it. The evidence, unchanged: `getvalue(121)` is read in exactly two places: `0x410A56` in the init `sub_41095A`, which stores it to `dword_464988`; and `0x4047CE`, walking a 79-entry id table at `0x45B7D4` that packs `(id, value)` pairs for peers — replication, not behaviour. **`dword_464988` appears exactly once in the whole image — that write. It has no reader.** Meanwhile disease expiry is UNCONDITIONAL: the per-frame ager (`0x41F671`-`0x41F697`) advances `player+0x78` whenever it is non-zero and clears the disease via `sub_41DF4C` once it passes `player+0x7C`, testing no global at all. So `121,0` changes nothing in the original. The port used to gate its expiry on this flag (`libs/sim/src/systems/diseases.cpp`), which invented permanent, permanently-infectious diseases the original cannot produce; that gate is gone | 1 |
| 1200 | campaign rover/ghost 1-in-N chance to turn at an open intersection | 3 |
| 1300, 1310, 1320 | campaign-only kill scores: AI / rover / ghost | 250 / 15 / 25 |
| 650, 651 | "Fire In The Hole"/"Clear" taunt (id-audit.md item 1, `sound_director.cpp`'s `BombPlaced` handler): 651 gates the carrying player's bomb-count powerup level ("many" bombs), 650 = 1-in-N roll once they place the LAST bomb of that allotment. `sub_41F29B` pseudo.c ~23362-23368, plays SOUNDLST 1200-1299 (corrected range — see that id below). Presentation-side roll (`AudioEngine`), never `State::rng`; see the handler's own comment for the register-provenance caveat on the 651 comparison | 4 / 4 |
| 340–350 | per-level tile-regen ATTEMPT interval, seconds (0 = never); one value per stage, indexed by `Tuning::level_index`. Only level 7 (Haunted House, "cemetary/mortuary") is non-zero. `TileRegenSystem`, `docs/re/facts.md` "Per-level tile regeneration" | 0×10, 4 (idx 7) |
| 695 | tile-regen clear radius, tiles (Manhattan) — no live player may be within this of a candidate regrow tile. `TileRegenSystem` | 4 |
| 450–460 | per-level ice/input-lag, ms (0 = none); one value per stage, indexed by `Tuning::level_index`. Only level 2 (Hockey Rink) is non-zero. `MovementSystem::ice_delay`, `docs/re/facts.md` "Ice / input-lag" | 0×9, 250 (idx 2) |

Powerup kind order (matches scheme `-P` rows and the id blocks above): extra bomb, flame, disease, kick, skate, punch, grab, spooger, goldflame, trigger, jelly, super-disease, random.

**25 / 30 / 31 are NOT routed through `Tuning::apply`, despite an earlier
version of this table implying they were** (`docs/re/id-audit.md` A(ii)).
25/30 (nominal/target frame rate, both = 20) are mirrored as the hardcoded
`sim::kTicksPerSecond = 20` (`constants.hpp`); 31 (elapsed-ms-per-frame
clamp, 150) has no consumer at all — the port's fixed-tick lockstep loop
doesn't need a wall-clock disk-hit clamp the way the original's real-time
loop did. Values match, but a modified VALUELST changing these ids would
have no live effect on the port. Not a functional gap (id 25's own comment
says "do not change this value"), just a doc-accuracy fix.

## Consumed directly by `libs/game` (not via `Tuning`)

Ids read live from the parsed `ValueList` by the presentation layer, outside
`sim::Tuning` (`docs/re/id-audit.md` method step 3; `grep -rn "VALUELST\|
getvalue" libs/game`):

| id(s) | area |
|---|---|
| 8 | **concurrent SFX voice cap** (the file's own comment: "how many concurrent sounds do we want to allow?"), authored **5**. `sub_427859` opens with `if (getvalue(8) < active_voices) return;` — over-cap sounds are DROPPED, never stolen from a playing voice. `AudioEngine::voice_cap_`, read at init; `docs/re/sound-engine.md` §5 |
| 12 | boot/title/logo dwell timeout, seconds |
| 32 | round-start **own-colour reveal** window, in nominal frames. Round init arms `dword_4621E8 = (1000/getvalue(30)) × getvalue(32)` = 50 ms × 40 = 2000 ms; while it runs, each player's BODY blits in that player's own slot colour instead of the +60 draw-colour byte, so everyone can find their bomberman before the team colours take over. Visually a **Team-Play-only** effect (with team play off both branches yield the same slot index). `Renderer` reads it straight from the `ValueList` (`renderer.cpp` `at_or(32, 40)`) and deliberately **NOT** via `Tuning::apply` — it is presentation-only and putting it in `sim::Tuning` would drag a render value into hashed-sim territory. `docs/re/facts.md` "Round-start own-colour reveal" |
| 15 | "is the online manual enabled?" — gates the help browser glob |
| 16 | GLUE\<n\> backdrop count for pre-match screens |
| 35 | number of built-in levels |
| 92 | main-menu attract-mode idle timeout, seconds (gated > 5) |
| 110–112 | in-round MM:SS clock HUD position + digit spacing |
| 200–247 | player-colour `.RMP` percent-RGB fallback table |
| 310 | default "wins to win a match" |
| 330 | cornerhead fidget-duration spread (presentation RNG) |
| 600–619 | editor default start positions (10 players × x,y) |
| 700–702 | main-menu cursor anchor (x, y, y-step) |
| 705, 710, 711, 715, 720–722 | PLAYER INPUT TYPE screen layout (heading/list/joystick-pane tuples) |
| 730–733 | Options screen sample-block preview geometry |
| 735–738 | LEVEL & ROUNDS screen layout |
| 745–748 | Options/Settings screen row layout (x, y0, ystep, colour) |
| 780–788 | RESULTS scoreboard header + per-player row layout |
| 800–803 | RESULTS outcome-line layout |
| 810–818 | hidden scheme/map editor menu layout |
| 1000, 1002, 1004, 1006 | Goldman wheel centre / radii / circle resolution / lissajous params — confirmed live at `game_app.cpp:1979-1984` |
| 1150–1160 | random-stage-rotation enable flags |
| 500, 502, 504, 506 | Bomb-PICKUP arc (id-audit.md item 4), 4-point curve `(12,10)/(25,20)/(25,30)/(12,40)` — `Renderer`'s carried-bomb draw in `draw_world`, read via `ValueList::column_or`. Pinned consumer: `sub_42331C`'s bomb state-3 ("carried") branch, pseudo.c ~25488-25497, gated on the carrier's player-state field +78 == 4; same curve shape as the already-ported 660/661 punch arc but with its own forward/vertical offset math (see the draw site's comment for the full formula). **The curve covers the PICKUP ANIMATION ONLY** — once +78 leaves 4, the else-arm drops it for a hardcoded 10 px forward / 40 px up, so these ids do not describe where a held bomb spends most of its time (facts.md "Grab / carry / throw presentation") |
| 1010 (VALUELST sense — id-namespace collision with SOUNDLST's own 1010, MENU.RSS music, unrelated) | Gold-player "twinkle" duration, seconds; 0 = indefinite (id-audit.md item 2). `Renderer::update_gold_sparkles`, read via `ValueList::at_or`. Pinned consumer: `sub_420D4E` (spawn) / `sub_420E39` (age+draw) / `sub_420F07` (per-tick dispatch), `docs/re/goldman-roulette.md` §6 |

## Mapped, not yet consumed

Trimmed 2026-07-09 (`docs/re/id-audit.md`): the previous version of this
table listed several ids as unconsumed that `Tuning::apply` or `libs/game`
already handle (92, 110-112, 660/661/665/667, 670/671, 15, 27/28, 745-748,
780-788, 1000-1010, 810/815) — moved to the two "Consumed" tables above.
What's left is the **genuine** open list, plus newly-found gaps from the
id-audit pass:

| id(s) | area |
|---|---|
| 3, 4, 6, 7 | sound-cache policy, all read in `sub_428A??`/`sub_428AEF`: 3 = selective pre-caching (**0**), 4 = total pre-caching (**0**, the file itself calls it "a bad idea"), 6 = cache byte budget (**7000000**), 7 = seconds between voluntary cache clears **and re-choosing/re-loading of SOUNDLST** (**1800**). Deliberately not ported: the port loads clips lazily and culls once at init, so ids 3/4/6 have no analogue and id 7's 30-minute re-roll of the random voice subsets is skipped. `docs/re/sound-engine.md` §3 |
| 40 | "do we randomize player starting positions?" — the DEFAULT seed of the Random Start option (dword_464AE8 = getvalue(40) = 1 at `sub_41095A`, overridden by options.ini `random_start=`); consumed by the game layer as the absent-key default, `docs/re/facts.md` "Options toggles" |
| 101 | **= 60 (CONFIRMED)**: in-round "hurry" threshold, seconds remaining — when the round clock enters the (getvalue(101)−5, getvalue(101)) window the tick callback one-shots SFX 2700 and flashes the "hurry" ANI at screen centre on alternating `frame & 4` ticks (`sub_42A191` ~29531-29549, `docs/re/in-match-shell.md` "hurry flash"). (id 101's OWN threshold-seconds value already feeds `Tuning::hurry_seconds` — see the Consumed table above; this row is the still-unported SFX/ANI presentation detail only.) |
| 110, 111, 112 | in-round countdown-clock HUD (**CONFIRMED**, `sub_4105D2` @ 0x4105D2, drawn every tick): 110/111 **= 525/36** = x/y of the MM:SS digits (drawn glyph-by-glyph with the `numeric font` ANI, message 281 `"%u:%02u"`; an "∞" glyph when the round is untimed), 112 **= 4** = extra px between digits (the file's own comment). Ink switches to the warning colour at ≤30 s remaining — the 30 is hardcoded, not a VALUELST id (`docs/re/in-match-shell.md` "in-round HUD") |
| 113–119 | in-round "player row" HUD (**CONFIRMED**, `sub_420F07` @ 0x420F07, drawn every tick): 113/114 **= 6/26** = the two Y rows (file comment "two vertical (Y) coordinates of each player row across the top"); 115–119 **= 10/110/210/310/410** = the five X columns (file comment "left (X) coordinates of each player column across the top") — a 5-column x 2-row grid, column = playerIndex/2, row = playerIndex&1. Each active slot draws "S:\<wins\> K:\<kills\>" (message 37) in that player's colour; a round-dead slot gets the `MISC.ANI` "xxx" sprite overlaid. `docs/re/in-match-shell.md` "The player row" |
| 330 | **= 13 (CONFIRMED).** File comment: "how many cornerhead animations there are" — id 330 is BOTH the number of cornerhead sequences AND the idle "cornerhead" fidget duration spread (`sub_41F29B` ~23011 rolls `20 + rand()%getvalue(330)`, guarded so the modulus ≥1). Presentation-only (renderer `panic_lcg_`, never `State::rng`); renderer's `kPanicSpread` now = 13 (was the 40 stub). Equals `kCornerheadVariants` by construction, not coincidence |
| 120–138 | disease behavior flags and durations mostly consumed (see table above). **Two of them are dead in the ORIGINAL, not merely unported, and the distinction matters: consuming them is a divergence, not a fix.** 122 (`diseases_will_recycle`, authored 0) is compared at `0x41DF97` inside `sub_41DF4C` and the result is never branched on — the very next instruction (`add eax, [ebp-0x4]` @`0x41DFA1`) overwrites the flags and the disease byte is cleared unconditionally, so the compiler kept a test with no body. The port correctly does not map it. 121 (`diseases_time_limited`) was worse: it IS read, into a global nothing ever reads back, and the port used to CONSUME it. Fixed 2026-07-30 — the field is parsed and carried on the wire but read by nothing; see its own row above |
| 324 | dud-bomb fizzle duration's random-additional-frames component (320-323 are consumed; 324 is a leftover 5th value in the same VALUELST block, unconfirmed whether the original even reads it — no `getvalue(324)` call site found) |
| 681 | trampoline hop arc height (px/frame) — presentation-only; id 680 (bounce frames) is now consumed above |
| 905 | reserved/unused AI slot — no `getvalue(905)` call exists in the binary and VALUELST has no `905,<n>` line; only the editor's label writer touches it (`docs/re/ai.md` §9.5) |
| 1100–1110 | net protocol retransmit timing / count — netplay, out of scope (ADR-0003) |
| 790, 795 | RESULTS screen "press F1" / "continue with same net game?" coordinates |
| 805 | Goldman Roulette Wheel "title at top" legend row — **no `getvalue(805)` call exists in the binary**; the title is presumably baked into ROULETTE.PCX (`docs/re/goldman-roulette.md` §7) |
| 1100–1140 | key-remap UI (`sub_407B9D`) labels: screen header, per-slot "press key for", action names, bound-key display |
| 43 | default bomb type (0 regular/1 trigger/2 jelly) — no `getvalue(43)` call site found; likely superseded by per-player powerup-driven kind selection, unconfirmed dead vs. gap |
| 106 | death anim #9 ("the angel") upward px/frame — port discovers the DIE\*.ANI pool from the install directly rather than needing this id; unresolved whether it's redundant with the asset's own frame data or a real gap |

## Not in VALUELST (hardcoded in BM95.EXE, now confirmed — no open tunables)

Both former "our own tunable" guesses here are resolved: flame linger
duration (`flame_frames`) is CONFIRMED = 10 frames, read directly from the
per-tick flame-grid update at `sub_426d06` (facts.md "Flame lifetime —
CONFIRMED = 10 frames"). The old `corner_threshold` guess (900 = 9 px) was
deleted outright — corner-assist is not a distance threshold at all; it
resolves per-pixel via `sub_41EC84`'s movement budget (facts.md "no distance
threshold" note, verified by `tests/test_move.cpp`).

Parsing notes: `;` starts a comment; the file ends with a DOS EOF byte (0x1A); a few ids hold coordinate pairs (`id,x,y`) — the current parser keeps the first value only, which is fine for the ids the sim reads.
