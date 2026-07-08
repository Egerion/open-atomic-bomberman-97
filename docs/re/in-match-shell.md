# BM95.EXE — in-round input shell, HUD, and round-end sequence

Facts distilled from static analysis of `BM95.EXE` (Watcom C, imagebase
0x400000) and the shipped install. **Facts only — addresses and observed
behaviour, never copied code.** Companion to `docs/re/frontend-flow.md` (the
menu/boot/results screen flow this document's round loop is nested inside).

Scope: what happens **during** a live round (`GameApp::run_match`'s
territory) — the auxiliary (non-movement) keys the original polls every
frame, the in-round HUD, and the round-end shell between "one side left" and
the results tier. This has never been RE'd before; our port currently
handles only Esc (bail to menu) and gameplay input in `run_match` — this
document says exactly how that compares.

## The round driver — `sub_42A3F6`, nested inside the Play handler

`sub_42A3F6` (@ 0x42A3F6) is the **entire Play flow**: player setup →
round → results tier → repeat-or-exit. It is reached from the main menu's
row 0 (`docs/re/frontend-flow.md` "main-menu items") and from its own tail
(loops back to itself for the next round of a multi-round match). Structure,
top to bottom:

1. `sub_42741E(0x3FC)` — starts the looping "win" music track (1020) at
   handler entry (`docs/re/frontend-flow.md` "Results MUSIC").
2. `sub_410F81()` — player/level setup screens (goldman wheel, player slots,
   LEVEL & ROUNDS); returns early for attract mode.
3. `sub_410B6E()` — level load + round init (chooses the stage, resets the
   round timer via `sub_4104C2`/`sub_410494`).
4. **`sub_43A6FC((int)sub_42A191)`** — registers `sub_42A191` (@ 0x42A191) as
   a **per-frame tick callback**. This is the actual gameplay driver: it
   advances `dword_464994` (the frame/tick counter), calls the movement/AI/
   physics chain, the per-player render-and-cornerhead pass (`sub_420F07`,
   which is what calls the per-player mover `sub_41F29B` — the function named
   in this task's brief, confirmed at pseudo.c 23659), the clock/HUD draw
   (`sub_4105D2`, below), and the "hurry" flash. When the round ends it is
   first **suspended** (`sub_42A16F(1)` at pseudo.c 29821 sets the guard
   `dword_4646AC`, making the callback a no-op) for the duration of the
   DRAW/RESULTS/VICTORY screens — re-enabled (`sub_42A16F(0)`, 30113) if a
   multi-round match loops back into the next round — and only
   **unregistered** (`sub_43A74C((int)sub_42A191)`, `LABEL_204`, pseudo.c
   29815-29818) when the Play flow exits for good. Either way, gameplay
   ticking and its render pass genuinely stop the instant the round-driver
   loop exits, before the outcome screens run their own bespoke wait loops.
5. **`while (1) { v72 = sub_43A508(...); switch-on-v72; ...standard-frame-
   work... }`** (pseudo.c 29705-29819) — the loop this task calls "the
   in-round main loop". `sub_43A508` polls one already-remapped virtual key
   code per iteration (same primitive used by the menu/boot code, chained
   through `sub_43A56C`/`sub_43A624`/`sub_43DF28`; `sub_43DF28` applies the
   player's key-remap table, so the numeric codes below are **post-remap
   virtual key ids**, not raw scancodes — consistent with `docs/re/
   results-and-options.md`'s `sub_407B9D` key-remap UI using the same id
   space, e.g. **328/336/331/333 = Up/Down/Left/Right, 315 = F1**
   everywhere else in the front end).
6. Round-end shell: DRAW or RESULTS-tally wait loop, then VICTORY/loop-back
   (below).

(Provenance: `sub_42A3F6` @ 0x42A3F6, pseudo.c 29610-30152; `sub_42A191`
@ 0x42A191, pseudo.c 29488-29557; `sub_410B6E` @ 0x410B6E, pseudo.c
14689-14848 [level/round init, not the tick loop — an earlier task
assumption that `sub_410B6E` contains a per-frame loop calling `sub_41F29B`
is corrected: `sub_410B6E` runs once at round setup, not per frame]; per-
player mover call site pseudo.c 23659 inside `sub_420F07` @ 0x420F07, itself
called once per tick from `sub_42A191` at pseudo.c 29527.)

## The auxiliary key table — EXHAUSTIVE, CONFIRMED

Reading the exact nested-if chain at pseudo.c 29709-29788 (not paraphrased —
every comparison in the chain is accounted for below):

| key (v72) | gate | action | notes |
|---|---|---|---|
| `1` | `sub_413D01()` (debug/skip-logos flag) | `sub_42A325()` — dumps a debug text file | **debug cheat**, see below |
| `4` | none | `sub_413BB0()` — arms the DOS text-mode debug overlay (`dword_45BCD4=1`, `sub_42C098(0)`) | **debug cheat**, see below; no visible in-game effect without the mono/color text-page overlay actually being read by a debugger/second monitor — effectively inert on a normal VGA session |
| `0x11` = **17** | none | `dword_46492C=-1; dword_464A68=2;` — **abort round, forfeit, back to menu** | the SAME target the menu's Escape handler jumps to (`docs/re/frontend-flow.md`'s `v8>=17 && (v8<=17\|\|v8==27)` Escape/Quit dispatch uses the identical 17/27 pairing) — see "Esc negative finding" below for why only 17 fires here |
| `18` | `sub_413D01()` | `sub_4165D2()` — toggles a display mode (`dword_460BA4` ? `sub_416591` : `sub_4162F0`) | **debug cheat**, gated |
| `0x111` = **273** | none | `dword_4646B4 = 1` | **not an abort** — arms a flag consumed only by the DRAW/RESULTS wait loops' 6 s auto-advance test (`(sub_42247A()\|\|dword_4646B4) && time>t0+6000`, pseudo.c 29838/30062); reset to 0 at round entry (29704). Effectively a "skip the outcome screens quickly" latch, not a live pause/skip of the round itself |
| `288` | none | `sub_413D45()` — opens the **"Internal debugging information window"** (MESSAGES.TXT id 400) | **debug cheat**, NOT gated by `sub_413D01()` — reachable in retail; shows total/audio mem, local net id, BOMBER_ID, critical-retrans rate, audio cache hit % (msg ids 405/410/411/415/420); dismissed by Enter/Esc. Same handler the main menu binds to raw code 288 (`docs/re/frontend-flow.md`'s table) |
| `[0x112, 0x131]` = **274..305** | none | `sub_40C678()` | **network-only stats dump** — gated internally on `sub_413D01()`, writes a per-tick statistics text file (`aStatisticsForA` header, 200 `sec/byte-in/byte` lines) via `fopen(..., "at")`; a no-op with no visible effect in local play beyond the file I/O |
| `0x13B` = **315** (F1) | `!sub_40C06A()` (local only) | `sub_41431C()` — opens the generic `.BM` help browser **without leaving the round** | same routine the main menu's row 5 opens (`docs/re/frontend-flow.md` "main-menu items", corrected entry) |
| `324` | mixed | network ready/chat message send (`sub_40FB90`/`sub_40CE27`) + (if `sub_413D01()` and a pending gold player) commits the gold pick | **network-only**, N/A locally |
| `27` (Esc) alone | — | **NOTHING** — falls through both `< 0x1B` and `> 0x1B` branches untouched | see "Esc negative finding" below |
| anything, while `dword_464938` (attract mode) | — | `dword_46492C=-1; dword_464A68=2;` (same as key 17) | any key aborts an attract-mode demo round back to the menu — already documented in `docs/re/frontend-flow.md` "Attract mode" |

All codes not listed above (including ordinary movement/gameplay keys) fall
through to `LABEL_54` and are consumed by the normal per-tick input
collection elsewhere in the tick callback (`sub_42A191`'s chain) — outside
this loop's switch, which only handles the auxiliary set above.

(Provenance: nested-if chain pseudo.c 29709-29788, transcribed range by
range above; `sub_42A325` @ 0x42A325 pseudo.c 29577-29603; `sub_413BB0`
@ 0x413BB0 pseudo.c 16683-16688; `sub_42C098` @ 0x42C098 pseudo.c
31004-31020 [installs/clears a debug render callback; `0x42C184`/`0x42C1E8`
are literal `0xB0000`/`0xB8000` — the MDA/CGA text-mode video segments,
confirming this is a DOS-text-page debug overlay]; `sub_4165D2` @ 0x4165D2
pseudo.c 18408-18415; `sub_413D45` @ 0x413D45 pseudo.c 16752-16832
[`sub_43C734`/`sub_41696C` modal, msg ids 400/401/405/410/411/415/420,
dismiss on Enter(13)/Esc(27) — pseudo.c 16822-16829]; `sub_40C678`
@ 0x40C678 pseudo.c 11415-11439; MESSAGES.TXT ids `400,"Internal debugging
information window"` / `405,"Total mem: %u, Audio mem: %u"` /
`410,"Our local net id: %u"` / `411,"BOMBER_ID: %u"` /
`415,"Critical retrans rate:%6.3f"` / `420,"Audio cache hits: %u%%"`.)

### Pause negative finding — NO PAUSE KEY EXISTS (CONFIRMED)

**The original has no player-facing pause.** Every comparison in the round
loop's key chain is enumerated in the table above; none of them is a pause.
'P' (80) and 'p' (112) both land in the `> 0x1B` branch below `0x111`,
where the chain does nothing; no other code freezes the game either. Two
things LOOK like a pause and are not, for the record:

1. **The automatic round-timer freeze** — `sub_421969() <= 1` (one side
   left) → `sub_410522()` clears the timer-running flag `dword_4601B4`
   (pseudo.c 29791-29792). Game state keeps ticking (death animations play,
   the tick callback stays registered); only the clock stops. Not
   key-triggered. (Details under "Timer auto-pause" below.)
2. **The modal tick suspension** — `sub_42A16F(x)` @ 0x42A16F (pseudo.c
   29479-29484) is a bare setter of `dword_4646AC`, and `sub_42A191` (the
   tick callback) is a complete no-op while that guard is set (pseudo.c
   29504). Opening the F1 help browser mid-round is bracketed
   `sub_42A16F(1)` … `sub_41431C()` … `sub_42A16F(0)` (pseudo.c
   29769-29771), so **the whole game freezes under the help overlay** —
   sim, rendering, HUD, everything. This is the closest thing to a pause
   the shipped game has, and it is a side effect of the modal, not a pause
   feature. Note the **round clock still burns wall-clock time under it**:
   `sub_4105D2` accumulates `dword_4601B8 += now - dword_4601A0` per call
   while the timer-running flag is set, and nothing clears that flag around
   the F1 bracket — so the first tick after the modal closes adds the
   entire modal duration to the round clock in one lump. Reading the manual
   mid-round costs you round time.

So: our port having no pause handling in `run_match` is **faithful** — a
pause key would be an invention. If we ever port the mid-round F1 help, the
faithful behaviour is "freeze the sim, let the clock keep running".

### Esc negative finding — CONFIRMED, and it changes what our port's Esc should do

**Raw Esc (27) alone does NOT abort an in-round match in the original.**
Tracing the exact comparison chain: the outer dispatch is
`if (v72 < 0x1B) {...} else if (v72 > 0x1B) {...}` (pseudo.c 29709/29743) —
**27 (`0x1B`) satisfies neither branch** and falls straight through to
pseudo.c 29788's `if (dword_464938) goto LABEL_34;`, which only fires during
attract mode. In a real, player-controlled round, pressing Esc reaches
`LABEL_54` having done **nothing** — no abort, no pause, no visible effect.

**The key that actually aborts/forfeits mid-round is `0x11` = 17 (CONFIRMED
Ctrl+Q).** This is the same numeric-control-code pattern already established
elsewhere in the binary: `docs/re/frontend-flow.md`'s main-menu Ctrl+E-×6
map-editor trigger uses raw code 5 = the ASCII control code for Ctrl+E; by
the identical convention, ASCII control code 17 = **DC1 = Ctrl+Q**. The menu
loop's own Escape/Quit dispatch (`v8>=17 && (v8<=17||v8==27)`,
`docs/re/frontend-flow.md` line ~30861) treats 17 and 27 as synonyms **only
there** — in the round loop the two codes are NOT synonyms: only 17 is
wired to the abort branch (`LABEL_34`), 27 is not reachable by that branch at
all. So in-round, **Ctrl+Q forfeits/aborts to the menu; Esc is inert**.

This directly changes the answer to the task's framing question ("what
should our Esc-to-menu really do — instant quit, confirm prompt, or
forfeit?"): the original's answer is **none of those for Esc** — Esc is not
bound to anything mid-round; the real abort key is Ctrl+Q, and when it
fires it is an **instant, unconfirmed forfeit** (`dword_464A68=2` with no
dialog, immediately followed by the standard teardown at `LABEL_200`/
`LABEL_204`: `sub_40FB44` [network-notify, N/A locally] → a 10×100 ms drain
loop pumping `sub_40EA1E`/`sub_413CB0` → `sub_43A74C` unregisters the tick
callback → return to the menu). There is no confirm prompt anywhere in this
path (contrast the main menu's Quit row, which does pop one).

**Port status (documented gap, not fixed by this doc — DOCS-ONLY task):**
`GameApp::run_match` currently maps SDL's Esc key to `AppInput::MatchOver`
(`libs/game/src/game_app.cpp:1555-1556`), i.e. our port's Esc behaves like
the original's **Ctrl+Q** (instant, unconfirmed forfeit) rather than like
the original's literal Esc (inert). Functionally the port's Esc gives
players a working "quit to menu" affordance the original also has — just
bound to a different key. Of the auxiliary keys, **F1 is now wired**
(2026-07-08): `run_match` opens the generic `.BM` help browser
(`GameApp::present_help_browser_modal`) with the sim tick loop suspended for
the modal's duration, mirroring this doc's `sub_42A16F(1)/(0)` bracket — with
ONE deliberate deviation: on close the frame accumulator is reset, so the
round clock does NOT absorb the modal's wall-clock duration (the original's
documented lump-sum clock burn under "Pause negative finding" point 2 is a
bug we chose not to reproduce). The 288 debug window and the other
debug-gated keys remain unwired. Faithfully matching the original key-for-key
would mean binding forfeit to Ctrl+Q and leaving Esc inert, which would be a
worse player experience than what we have; this is flagged as a fact, not a
recommendation to regress the binding.

## The in-round HUD — CONFIRMED: a live countdown clock IS drawn (refutes "bare field")

The original draws a **numeric round-timer HUD every tick** — the task's
framing question ("does the original draw a clock/score line, or is the
field bare?") is answered: **there is a clock; there is no score/kill
line.**

`sub_4105D2` (@ 0x4105D2) is called once per tick from `sub_42A191`
(pseudo.c 29518), unconditionally (no debug gate, no options toggle found).
Its body:

1. Advances the round clock (`dword_4601A4`/`dword_4601BC` = seconds
   elapsed/remaining), driven off `sub_43ACF8()` (the same millisecond clock
   used throughout) — this is the SAME timer `sub_41087D()` reads to decide
   "time up" and `sub_410522()`/pseudo.c 29792 pauses while ≤1 side is alive
   (see "timer auto-pause" below).
2. **Untimed rounds (`dword_4601A8 == 1001`)** draw the literal glyph string
   `aInfinity` ("∞") at `getvalue(110)`, `getvalue(111)` instead of digits —
   so the HUD still appears, just showing infinity, when a round has no time
   limit.
3. **Timed rounds** format `MM:SS` via message **`281 = "%u:%02u"`**
   (`v13/60`, `v13%60`), then draw it **digit by digit** using the
   **`aNumericFont`** glyph set (a dedicated font resource distinct from the
   FONT6 UI font used by `.BM` screens), one `sub_415920` blit per character
   at `x = getvalue(110)` (**525**), `y = getvalue(111)` (**36**), advancing
   `x` by each glyph's width plus **`getvalue(112)`** (**4** px — the
   VALUELST file's own comment: "extra pixels of space between the timer
   font digits") per digit.
4. **Colour changes at ≤30 s remaining** — `byte_49D38F` (the normal ink
   colour) is swapped for `byte_49A390` (a distinct, presumably warning-red,
   palette index) once `v13 <= 30`. This is a separate, permanent colour
   change (not the blinking "hurry" flash below, which is a separate
   late-round text warning).

**Timer auto-pause while the round is already decided (CONFIRMED, distinct
from any key).** `sub_42A3F6`'s main loop calls `sub_421969()` (alive-player
or alive-side count) every iteration and, once it drops to `≤1`, calls
`sub_410522()` — which clears `dword_4601B4` (the "timer running" flag), so
`sub_4105D2` stops advancing `dword_4601AC`-derived elapsed time. This is
NOT a player-triggered pause; it is the game freezing its own clock once a
side has already won, presumably so the linger-and-death-animation window
doesn't visibly burn round time. No key toggles this.

**No score/kill/lives HUD element exists in `sub_4105D2` or anywhere else in
the tick callback chain** — the per-tick chain (`sub_4105D2`,
`sub_415CA4`, `sub_42641F`, `sub_4056CA`, `sub_4245B9`, `sub_424F89`,
`sub_41B961`, `sub_426D06`, `sub_426818`, `sub_420F07`, `sub_42459A`,
`sub_410578`) was read function-by-function for this task; only
`sub_4105D2` (clock) and the "hurry" block (below) draw any text/HUD
element. Per-player win tallies only appear later, on the separate RESULTS
screen (`docs/re/frontend-flow.md` "RESULTS tally tier").

### The "hurry" late-round flash — CONFIRMED, separate from the clock colour change

Still inside `sub_42A191` (pseudo.c 29531-29549), after the clock draw:
reads **`getvalue(101)` = 60** and compares it against the round's total
length; when the **remaining time is within 5 seconds of `getvalue(101)`**
(`v6 > v7 - 5`, i.e. within the last ~5 s of crossing the 60 s-remaining
threshold — practically: fires once, near the 55-60 s-remaining mark, not
continuously for the rest of the round) it plays **SFX 2700** once
(`dword_464984` latch guards the one-shot) and, **on alternating frames**
(`dword_464994 & 4`, i.e. flashing at roughly a quarter of the tick rate),
blits the string `aHurry` ("hurry") centred at `dword_464A70/2,
dword_464A6C/2` (screen centre). This is a one-time warning flash near
60 s remaining, independent of the clock-colour change at ≤30 s.

**Port status: DONE — no longer a gap.** ROADMAP "In-round shell — RE'd +
ported 2026-07-08" reconciled this: `game_app.cpp` now draws the MM:SS clock
HUD (`getvalue(110)/(111)/(112)` for position/spacing, message 281 for the
format, showing infinity when untimed, warning ink at ≤30 s) and the "hurry"
flash + SFX 2700 at the `getvalue(101)` = 60 s threshold, driven off
`sim::State::ticks_left`. This document pinned the facts the port pass used;
they are now implemented, not just recorded.

(Provenance: `sub_4105D2` @ 0x4105D2, pseudo.c 14456-14552; `sub_41087D`
@ 0x41087D pseudo.c 14565-14572; `sub_410522` @ 0x410522 pseudo.c
14419-14425; `sub_421969` @ 0x421969 pseudo.c 24048-24056; hurry block
pseudo.c 29531-29549; VALUELST `101,60` / `110,525` / `111,36` / `112,4`
[file's own comment: "extra pixels of space between the timer font
digits"]; MESSAGES.TXT `281,"%u:%02u"`.)

## The round-end shell — from "one side left" to the results tier

The task's ported "~3 s linger" and the original's actual shell, precisely
matched up:

1. **Round loop exit.** Every iteration of the `while(1)` auxiliary-key
   loop ends with `if (!dword_464AEC && sub_421947() > 1 && !sub_41087D())
   goto LABEL_200;` (pseudo.c 29810). `LABEL_200` (pseudo.c 30139) doubles
   as the **bottom of the loop**: with `dword_464A68 == 0` it does nothing
   and the `while(1)` iterates again — so that guard reads "more than one
   alive (`sub_421947`) and clock not expired (`sub_41087D`) → keep
   looping". (`dword_464AEC` is only ever set by the network key 324 —
   always 0 locally.) When the survivor count drops to ≤1 **or** the clock
   runs out, the guard fails and execution falls INTO the round-end shell
   (29812+). There is **no fixed "3 s linger" inside the loop** — no
   dedicated post-game-over grace period exists the way our port's
   `over_ticks = 3 * kTicksPerSecond` window does; whatever visual
   "settling" happens (death animations finishing, the auto-frozen clock —
   see the timer auto-pause note) happens because the tick callback keeps
   running normally right up to the instant the shell unregisters it.
2. **Music switch — the 1130 track underlies the WHOLE outcome tier
   (CORRECTS `docs/re/frontend-flow.md`).** `sub_42741E(0x46A)` (track 1130,
   SOUNDLST label "draw") starts on loop exit at pseudo.c 29820 —
   **unconditionally, BEFORE the survivor test at 29823**. So DRAW, the
   RESULTS tally, AND the VICTORY/TEAM screen all play under **1130**;
   nothing ever re-starts 1020 in the outcome tier. The full Play-flow
   music model, from the now-exhaustive call-site list of both by-id music
   wrappers (`sub_42741E`: pseudo.c 29696/29820/30200/30277/30502/30783;
   `sub_4274AB`: 28922/28942/28947 — no other callers exist):
   - **Play entry** (pseudo.c 29696): track **1020** ("win") — this is
     actually the **setup-screens music** (player select / LEVEL & ROUNDS),
     not victory music.
   - **Round init** (`sub_410B6E` LABEL_48, pseudo.c 14847-14850): if the
     Options toggle **"Disable music during gameplay"** (`dword_4648C0`,
     options.ini `disable_game_music=` — `docs/re/results-and-options.md`
     §3 row 13) is set, `sub_427342()` frees the music → the round is
     silent; otherwise **`sub_4293E5()` @ 0x4293E5 starts the PER-LEVEL
     stage track: SOUNDLST id `1100 + level`** (it reads the id table at
     `dword_463094 + 4*(1100+level)`), falling back to **id 1120 (0x460)**
     when the level has no entry (or network / level==-1). These are
     SOUNDLST ids, unrelated to the VALUELST 1100-1110 net-timing rows.
     Immediately after, the round clock is armed: `sub_410494(dword_464948)`
     (total seconds) → `sub_4104C2()` (reset elapsed) → `sub_4104F0()`
     (set the timer-running flag).
   - **Round end** (29820): **1130** replaces the stage track and carries
     through DRAW/RESULTS/VICTORY; on a multi-round loop-back the next
     `sub_410B6E` replaces it with the next stage track.
   `docs/re/frontend-flow.md`'s "Results MUSIC — 1020 (win) played under
   the VICTORY screen" is therefore **wrong on the VICTORY half** (its DRAW
   half — 1130 under DRAW.PCX — is right but incomplete): the port's
   `start_music(1020)` on VICTORY does not match the binary; faithful is
   1130 under all three outcome screens, with 1020 as the setup-screens
   track. Flagged as a port follow-up, not fixed here (DOCS-ONLY task).
3. **DRAW branch (`sub_4219B0(...) == -1`, no survivor):** draws DRAW.PCX
   (cut, no wipe — `docs/re/frontend-flow.md`'s `sub_42A088` cut semantics),
   plays sting group **1700** once, then runs a **bespoke wait loop**
   (pseudo.c 29830-29881, NOT the generic `sub_42A088` primitive) with:
   - nav-blip (SFX 20) on any real key;
   - `sub_40C06A()==1` (network client) forces Esc — N/A locally;
   - **auto-advance after `sub_43ACF8() > v74 + 6000`ms — a literal
     **hardcoded 6000, not a `getvalue()` id** — gated on
     `sub_42247A() || dword_4646B4` (all-AI roster OR the in-round 273-key
     latch, see the aux-key table above);
   - accept keys 13/32/904 (904 = a network-specific code, N/A locally) play
     SFX 10 and exit; anything ≥32 and ≠904 loops; codes <27 other than -1/
     -2/13 also just loop (i.e. most keys are ignored — only Enter/Space/the
     net code accept);
   - **SFX 40 ("enrt1", "can't do that here")** fires if `sub_40C06A()==1`
     and a key was pressed — network-only, matches `docs/re/frontend-flow.md`.
4. **RESULTS branch (a survivor exists):** loads RESULTS.PCX, computes the
   winner-so-far index (`v73`, -1 if nobody has clinched yet) and prints
   tallies (`docs/re/results-and-options.md` §1 territory) — then runs an
   **almost identical bespoke wait loop** (pseudo.c 30051-30105) with the
   same 6000 ms hardcoded auto-advance gate, the same accept-key set (13/32/
   903 this time — a different net code, still N/A locally), and the same
   SFX-40 network buzz.
5. **Loop-back or VICTORY.** After the RESULTS wait:
   - **No clinch yet (`v73 == -1`):** if `v76 >= 2` (at least 2 players
     have a score entry — i.e. the match isn't degenerate), calls
     `sub_410B6E()` again — **starts the next round of the same match**,
     looping the whole `while(1)` structure from step... this is the
     multi-round match structure `docs/re/frontend-flow.md`'s "RESULTS
     tally tier" section flags as still needing a port. If `v76 < 2`
     (fewer than 2 players ever scored — a degenerate/aborted setup), shows
     message 47 ("Too many players have left the game!", header 95 "NOTE!")
     and sets `dword_464A68 = 2` (quit to menu) — **no forfeit-vs-quit
     distinction here; this is a hard bailout**, not a normal round loss.
   - **Clinched (`v73 != -1`):** formats `aTeamU`/`aVictoryU` (already
     documented in `docs/re/frontend-flow.md`), draws it via the generic
     `sub_42A088(name, 0)` (cut, no wipe, no wait — `wait=0`), then **a
     literal `sub_413CB0(3000)`** — a **hardcoded, blocking 3000 ms
     `Sleep`-loop** (100 ms chunks, pumping the message loop each chunk;
     `sub_413CB0` is a real OS sleep, not a frame-count linger — see its
     body, pseudo.c 16728-16743), no `getvalue()` id backs this 3 s. Only
     after the sleep does it set `dword_464A68 = 1` (clean match-win flag,
     the ONLY site in the whole binary that assigns 1 to this variable —
     every other assignment is 0/2/10) and fall through to teardown.
6. **Teardown (`LABEL_200`/`LABEL_204`, shared by every exit path):** if
   `dword_464A68` is nonzero, calls `sub_40FB44` (network-notify;
   `sub_40FCE6()` locally — a local-only stub call, not further RE'd here as
   out of scope), then a **10-iteration × 100 ms drain loop**
   (`sub_40EA1E`+`sub_413CB0(100)` — 1 s total, pumping input/message loop
   so any queued network/UI events flush), then `sub_43A74C` unregisters
   the `sub_42A191` tick callback and the function returns — control goes
   back to `sub_42B9CE` (the menu, `__noreturn` loop) or to `sub_42A3F6`'s
   own caller if it was itself re-entered for a repeat round.

**Corrected constant for our ported "~3 s linger":** the task asked us to
find the "real constant/getvalue id" backing the ~3 s post-round linger our
port has (`GameApp::run_match`'s `over_ticks = 3 * sim::kTicksPerSecond`,
`libs/game/src/game_app.cpp:1588`). **There is no `getvalue()` id — it is a
hardcoded `sub_413CB0(3000)`, i.e. a literal 3000 ms**, and critically it
happens **AFTER** the RESULTS screen's own wait-for-input step, immediately
before showing VICTORY/TEAM — not as a grace period tacked onto the
moment-of-death the way our port's `over_ticks` currently works (our port's
3 s starts counting the instant `sides_remaining() <= 1`, runs concurrently
with normal simulation ticking, and then hands off to the Results screen;
the original has no such grace window inside the round loop at all — see
point 1 above — and instead sleeps 3 s later, after RESULTS, right before
VICTORY). The numeric value (3000 ms / 3 s) our port already uses happens to
match the original's `sub_413CB0(3000)` exactly, which is a correct constant
even though it is wired into a different point in the sequence — a
structural gap (multi-round loop-back, RESULTS tally, and the two 6 s
bespoke wait loops are the still-missing pieces per `docs/re/frontend-flow.md`
"RESULTS tally tier — RE'd, port still DEFERRED"), not a constant-value bug.

**End-of-round jingle/sting ids** (stings were pinned in
`docs/re/frontend-flow.md`; the music placement is corrected by step 2
above): draw sting = SOUNDLST **1700** (`sub_427BFB(1700)`, one-shot group,
GUMP1.RSS), "we have a winner" voice = SOUNDLST **2000** (`sub_427BFB(2000)`,
PROUD.RSS group, played right after the RESULTS wait exits — pseudo.c
30027, i.e. BEFORE the 1500 ms/3000 ms sleeps, not after), outcome-tier
music = SOUNDLST **1130** (DRAW.RSS — under DRAW **and** RESULTS **and**
VICTORY, per step 2), setup-screens music = SOUNDLST **1020** (WIN.RSS,
started at `sub_42A3F6` entry, replaced by the per-level stage track —
SOUNDLST `1100+level`, fallback 1120 — at each round init).

(Provenance: full round-end shell pseudo.c 29788-30152; `sub_413CB0`
@ 0x413CB0 pseudo.c 16728-16743 [confirmed a real Sleep(100) loop, not
frame-driven]; `dword_464A68` write sites — `grep`'d exhaustively across the
whole decompile, 22 sites, only pseudo.c 30132 assigns 1; VALUELST has no
id for the 1500/3000/6000 ms constants used in this shell — all three are
literal immediates in the disassembly, confirmed absent from
`docs/valuelst-map.md`'s existing tables and re-checked against the raw
VALUELST.RES text.)

## Debug/cheat keys — summary (all CONFIRMED, all facts worth keeping)

| trigger | gate | effect |
|---|---|---|
| in-round key `1` | `sub_413D01()` (the `-nologo`/debug global) | writes a debug text dump (`sub_42A325`) — one line per something in a `v6[0]`-sized buffer, format `"%s\n"`, opened `"wt"` |
| in-round key `4` | none (always live) | arms a DOS text-mode debug overlay pointer (mono/color text page, `0xB0000`/`0xB8000`) — invisible under normal VGA graphics mode, a leftover text-mode debug hook |
| in-round key `18` | `sub_413D01()` | toggles a display mode via `sub_4165D2` (`dword_460BA4` ? `sub_416591` : `sub_4162F0`) — not further characterized, out of scope for this pass |
| in-round key `288` | none (always live, also live in the main menu per `docs/re/frontend-flow.md`) | opens the "Internal debugging information window" — mem/audio-mem/net-id/BOMBER_ID/retrans-rate/cache-hit stats |
| in-round keys `274..305` | `sub_413D01()` (internal to `sub_40C678`) | dumps a 200-line per-tick statistics file, `"at"` append mode |
| main-menu Ctrl+E ×6 | none | the map editor (`docs/re/results-and-options.md` §5, already fully RE'd — not re-covered here) |

None of these are gated behind a documented "debug build" flag distinct from
`dword_460260` (`sub_413D01`, the same global the boot logo-skip and title
sting/timeout logic already uses) — the retail EXE ships every one of these
live, and two (`4`, `288`) have no gate at all. They are genuine,
shippable-binary developer cheats, not decompiler artifacts.

## Cross-reference

- `docs/re/frontend-flow.md` — the menu/boot/results screen flow this
  round loop is nested inside; its "RESULTS tally tier" section documents
  the multi-round match structure and per-round scoreboard, which shipped
  via ROADMAP "Multi-round best-of-N loop + RESULTS tally 1:1 — DONE
  2026-07-08" — this document's round-end shell section is the connective
  tissue between that tier and `sub_42A3F6`'s full control flow.
  Its "Results MUSIC" paragraph and the 1020/1130 tunables rows were
  CORRECTED by this pass (1130 under all outcome screens; 1020 = setup
  music; per-level stage music `1100+level` discovered) and are now ported.
- `docs/valuelst-map.md` — new ids `101`, `110`, `111`, `112` added by this
  pass (clock HUD); id `101` was already loosely noted as "hurry timing", now
  pinned to an exact value and formula.
- `libs/game/src/game_app.cpp` — `run_match` (`~line 1545`) is the port's
  in-round loop; this document's Esc finding, HUD, and hurry-flash facts are
  all implemented there (ROADMAP "In-round shell — RE'd + ported
  2026-07-08").
