# BM95.EXE — front-end boot & screen-flow facts

Facts distilled from static analysis of `BM95.EXE` (Watcom C, imagebase
0x400000) and the shipped install. **Facts only — addresses and observed
behaviour, never copied code.** Provenance cited per entry. Companion to the
Phase 3 spine (`docs/adr/0004-frontend-screen-flow.md`).

## There is no FMV — the whole front-end is the assets we already parse

The install ships **no** `.smk` / `.avi` / `.mve` / `.flc` / `.fli` — confirmed
by an exhaustive `find` over `BOMBRMAN/`. Every front-end screen is one of the
formats the asset pipeline already decodes:

| Screen art | File | Format | Pipeline status |
|---|---|---|---|
| Interplay logo | `DATA/RES/IPLOGO.PCX` | PCX | loads today |
| High Score / studio logo | `DATA/RES/HSLOGO.PCX` | PCX | loads today |
| Title | `DATA/RES/TITLE.PCX` | PCX | loads today |
| Main menu | `DATA/RES/MAINMENU.PCX` | PCX | loads today |
| Draw / no-winner | `DATA/RES/DRAW.PCX` | PCX | loads today |
| (dead asset — see BONUS.PCX note) | `DATA/RES/BONUS.PCX` | PCX | loads today, but UNREFERENCED by the binary |
| Credits image bar | `DATA/RES/CREDBAR.PCX` | PCX | loads today |
| Head-to-head wipe | `DATA/ANI/HEADWIPE.ANI` | ANI | loads today |
| Boot/title music | `DATA/SOUND/TITLE.RSS` | RSS (id 1000, loop) | plays today, continuous across logos+title |
| Menu music | `DATA/SOUND/MENU.RSS` | RSS (id 1010, loop) | plays today, from menu entry |
| Menu-exit / accept sting | `DATA/SOUND/MENUEXIT.RSS` | RSS (id 10, one-shot) | plays today |
| Nav blip | `DATA/SOUND/LETTER1.RSS` | RSS (id 20, one-shot) | plays today |
| Title intro sting | `DATA/SOUND/GEN8A.RSS` | RSS (id 2800, one-shot) | plays today, before the title |

The text screens (`OPTIONS.BM`, `NETWORK.BM`, `CREDITS.BM`, `INPUT.BM`,
`MANUAL.BM`, `README.BM`, …) are **plain ASCII** with inline `<IMGxxx>` tags
that name a PCX to embed (e.g. `CREDITS.BM` opens `Atomic Bomberman
Credits\r\n…<IMGCREDBAR>…`). This `.BM` markup is the **one front-end format
the pipeline cannot parse yet** — but it only backs the deep menu leaves
(options/credits/manual), which are out of scope for the spine. (Provenance:
`head -c 200 CREDITS.BM`; `find` for codec extensions.)

## Top-level flow — `sub_42B060` then `sub_42B9CE`

The two calls that make up the whole app entry sit back-to-back
(`sub_42B060(); sub_42B9CE();`, decompile ~30951):

1. **`sub_42B060` — the boot presentation.** Body (paraphrased):
   ```
   sub_42741E(0x3E8)              ; START THE BOOT MUSIC (1000), FIRST OF ALL
   if (!sub_413D01())            ; skip-logos gate (returns global dword_460260)
       show("iplogo", wait=1)    ; sub_42A088(aIplogo, 1)
       show("hslogo", wait=1)    ; sub_42A088(aHslogo, 1)
   sub_427BFB(2800)              ; play SOUNDLST group 2800 (title intro sting)
   show("title", wait=1)         ; sub_42A088(aTitle, 1)
   ; then teardown, return to caller
   ```
   So the boot order is **IPLOGO → HSLOGO → TITLE**, each a full-screen image
   that **waits for a keypress or the `getvalue(12)` = 7 s timeout** (below), and
   the two logos are skipped entirely when the skip-logos flag (`dword_460260`, a
   `-nologo`-style global set at startup, `sub_413D01` — confirmed a bare `return
   dword_460260;`) is set. The title sting fires *before* the title image is
   drawn. (Provenance: `sub_42B060` @ 0x42B060; `sub_413D01` @ 0x413D01.)

   **The title sting is a GROUP random pick, not a fixed clip (FIXED).**
   `sub_427BFB(2800)` picks a random member of the **contiguous** SOUNDLST run
   starting at 2800 — the "ATOMIC BOMBERMAN!" intro group **2800..2810** (GEN8A,
   GEN8B, GEN8C, GEN8C2, ZAI08A..ZAI08G; the file's `; 2899 is the last intro`
   comment bounds it). Each boot can therefore voice a different take. The port's
   `run_boot_attract` now uses `play_random_in_range(2800, 2899)` (one-shot SFX
   voice, over the still-running boot music), which hits exactly the loaded ids in
   that span — matching the group pick. (An earlier port played the single id 2800
   via `play(2800)` — correct clip, no variety; widened to the group.)

   **The boot flow is STRAIGHT-LINE — there is NO attract loop.** `sub_42B060` is
   a single pass: start music → (skip-gate) IPLOGO, HSLOGO → sting → TITLE →
   **return**. It does not re-show the logos/title; after the title it returns to
   its caller, which enters the menu (`sub_42B9CE`, `__noreturn`, starting the
   1010 menu music). Each screen advances on a key OR the 7 s timeout, and the
   **title's timeout synthesizes Enter and proceeds to the MENU** — it does NOT
   restart the intro. The port's `run_boot_attract` therefore presents IPLOGO →
   HSLOGO → TITLE once and returns `Advance` (key or 7 s timeout) so `run_app`
   drops into `present_menu`; Back/Quit short-circuit out. (An earlier port
   looped the chain on the title timeout — removed; it was never in `sub_42B060`.)

   **The boot MUSIC model (corrected).** The very first line is *not* misc init:
   it is `sub_42741E(0x3E8)` — **the music player** (`sub_42741E` @ 0x42741E)
   starting SOUNDLST **1000** (`0x3E8` = TITLE.RSS). It is called **once**, before
   any screen, and the track then **plays CONTINUOUSLY across IPLOGO → HSLOGO →
   TITLE** — the logos are *not* silent. `sub_42741E` is the ONLY music call in
   `sub_42B060`; nothing restarts it per screen. `sub_42A088` (the screen
   primitive, below) never touches music. A naive port that starts music inside
   the screen primitive would (re)start the track on every screen, leaving the
   logos silent until the title — the bug this note exists to prevent. The menu
   (`sub_42B9CE`) later replaces it with the menu track (1010); see below.

   **Music is a LOOPING channel, distinct from one-shot SFX (CONFIRMED).**
   `sub_42741E` builds `<name>.rss` from the SOUNDLST id table
   (`dword_463094 + 4*id`), loads it (`sub_411D17`), and plays it via
   `sub_4273A4`, which **frees the currently playing music first**
   (`sub_427342`: releases the single music handle `dword_463064`) and starts the
   new one with **loop count `0xFFFF`** (`sub_41A3C9(handle, 0xFFFF)`) — i.e. an
   infinitely looping track, and starting a new one *replaces* the old. Contrast
   the SFX players `sub_427961` / `sub_427BFB` (the sting/blip): identical bodies
   that pick a random group member, but play through `sub_427859` / `sub_427B36`
   which set **loop count `0`** — one-shot voices on a separate pool, mixed via
   `rand_()` (C `rand`, presentation-side; never the sim RNG). So: **music = one
   looping channel that new tracks replace; stings/blips = one-shot voices** —
   exactly the split `AudioEngine::start_music` (looping music stream, re-queued
   on drain) vs `AudioEngine::play` (one-shot SFX pool) implements.
   (Provenance: `sub_42741E`/`sub_4273A4`/`sub_427342` @ 0x42741E/0x4273A4/
   0x427342; `sub_427961`/`sub_427BFB`/`sub_427B36` @ 0x427961/0x427BFB/0x427B36.)

2. **`sub_42B9CE` — the main menu (`__noreturn`).** Never returns; it *is* the
   application loop the logos/title fall through into. It draws `MAINMENU.PCX`
   (palette `mainmenu.plt`, loaded at ~30760) and drives the menu; entering a
   sub-screen dispatches to routines like `sub_41302D(aCreditsBm)` (a `.BM`
   text-screen viewer). Match start and the results screens are reached from
   inside this loop. (Provenance: `sub_42B9CE` @ 0x42B9CE; `aCreditsBm` use
   @ 30915.)

   **The MENU music id is `0x3F2` = 1010 (CONFIRMED, id corrected).** On entering
   the menu, `sub_42B9CE` plays the menu track **once** via `sub_42741E(0x3F2)`
   guarded by a run-once flag (`v14`: `if (v14) { sub_42741E(0x3F2); v14 = 0; }`).
   `0x3F2` = **1010** = MENU.RSS (SOUNDLST label `menu`) — this *switches* the
   looping music from the boot track (1000) to the menu track and keeps it
   playing while in the menu. **Do not confuse this with `0x3FC` (1020):** that id
   is played by the **Play/round handler `sub_42A3F6`** at its entry
   (`sub_42741E(0x3FC)`, decompile ~29696) — the "win" track for a game round,
   NOT the menu; the same handler plays `0x46A` (1130, `draw`) on the draw branch
   (~29820). The SOUNDLST labels (1000=title, 1010=menu, 1020=win, 1130=draw)
   therefore **agree with the code** — there is no label/code mismatch here; the
   only correction is that the menu-entry track is 1010, played in `sub_42B9CE`,
   while 1020 belongs to `sub_42A3F6`'s round path. (Provenance: `sub_42B9CE`
   `sub_42741E(0x3F2)` call; `sub_42A3F6` `sub_42741E(0x3FC)`/`(0x46A)` calls.)

## The generic Screen primitive — `sub_42A088(name, wait)`

Every full-screen image goes through one routine, `sub_42A088(a1=name,
a2=wait)` @ 0x42A088. It is the exact primitive the spine's `Screen` mirrors:

```
sub_415CE3()                     ; clear the back buffer (memset framebuffer 0)
sub_415C1F()                     ; bump frame + flip bookkeeping
palette = load(sprintf("%s.plt", name))   ; aSPlt_0 = "%s.plt"
apply_palette(palette)           ; sub_41522D: copies + (>>2) to VGA 6-bit
sub_429FF1()                     ; draw pass (sets dword_464994|=0x40; sub_429F1A)
sub_415C1F(); sub_41043C()       ; flip / present
key = getkey()                   ; sub_4102B7
if (wait) {
    t0 = time()
    loop {
        key = getkey()                          ; sub_4102B7
        if (elapsed + getvalue(12) < deadline)  ; ATTRACT TIMEOUT via getvalue(12)
            key = 13                            ; force "advance" (Enter)
        if (key != -1 && key != -2) sound(20)   ; sub_427961(20) nav blip
        if (key >= 0x1B) break                  ; Esc(0x1B) / others exit wait
        if (key == 13) { sound(10); return }    ; Enter selects, plays sound 10
        ; Space (32) also selects (checked after the >=0x1B break)
    }
}
```

Key facts the spine reproduces:

- **Palette is derived from the screen name** (`<name>.plt`), the image itself
  is drawn by the shared draw pass — so a "screen" is fundamentally just a
  name plus a wait flag, exactly the data-driven shape we adopt.
- **A waited screen appears INSTANTLY — sub_42A088 CUTS, it does not wipe or
  fade.** The present is exactly three steps: set the palette (`sub_41522D`,
  instant — its `>>2` is the **8→6-bit VGA palette conversion**, NOT a fade
  loop), blit (`sub_429FF1`; the `sub_429F1A` it calls is a ROT13 debug-watermark
  drawer, NOT a transition), and flip (`sub_41043C`). There is no HEADWIPE / fade
  on a waited screen — logos, title, results, and the `.BM` viewer all just
  replace the previous image. So the port presents these screens with a **cut**;
  the only screen-to-screen wipe in the front-end is the menu→match select wipe
  (`present_menu`), which is a separate, intentional effect. (Provenance:
  `sub_41522D` @ 0x41522D; `sub_429FF1`/`sub_429F1A` @ 0x429FF1/0x429F1A;
  `sub_41043C` @ 0x41043C.)
- **`sub_42A088` does NOT touch music** — it loads a palette, blits, and runs
  the wait loop. The background track is a separate concern (`sub_42741E`), so
  the port's `Screen` carries no music id and never starts a track; the boot
  caller owns the continuous music (see the boot-music note above).
- **The wait is "keypress OR timeout"**: the timeout is **`getvalue(12)` = 7
  SECONDS** — CONFIRMED. VALUELST ships the line **`12,7`**, and `sub_42A088`'s
  wait loop compares C `time_()` (whole seconds) against `start + getvalue(12)`,
  so the auto-advance fires 7 s after the screen appears. When it elapses the
  code synthesizes Enter (13) and advances; on the title that means it proceeds
  to the **menu** (the flow is linear — no re-run). The port sets both the logo
  and title dwell to **7000 ms** (`getvalue(12)·1000`), keypress-skippable.
  (Provenance: VALUELST `12,7`; `sub_42A088` wait loop @ 0x42A088.)
- **The timeout auto-advance is AUDIBLE — it plays SFX 20 + 10, not silence
  (FIXED).** The synthesized Enter (13) is NOT a quiet exit: it re-enters the
  same sound path a real accept takes. The wait loop runs `if (key != -1 && key
  != -2) sub_427961(20)` for the forced `key=13` (the nav blip fires), then the
  `key==13` branch reaches `sub_427961(10)` (the accept sting). So a screen that
  times out plays **BOTH the blip (20) and the accept (10)**, exactly like a
  keypress. The port's `Screen::update` previously just set `done_` on the dwell
  (silent); it now plays `play(20)` then `play(10)` on the timeout (guarded by
  `!done_` so a key that already accepted this frame does not double-fire), so the
  dwell advance is audible. The blip/accept are one-shot SFX, so the looping boot
  music is untouched. (Provenance: `sub_42A088` wait loop: `if (result < v12) v8
  = 13;` then the shared `sub_427961(20)`/`(10)` path.)
- **Skip keys + their SFX (ported exactly).** Reading the wait loop precisely:
  ANY real key (`key != -1 && key != -2`) fires `sub_427961(20)` (the nav blip);
  then the **accept keys Enter (13), Space (32), Escape (0x1B/27)** each reach the
  accept path `sub_427961(10)` and leave the wait, while other codes 1..26 keep
  waiting after their blip. So the confirmed behaviour is: **Enter/Space/Escape
  accept → SFX 10; any other key → SFX 20 blip only**. The port's
  `Screen::on_key(key)` mirrors this — blip 20 on every key, sting 10 + finish on
  Enter/Space/Escape — and crucially **plays these as one-shot SFX (`play`), so
  the looping music is never stopped by a skip; only the screen changes.**
- `sub_427961` and `sub_427BFB` are **identical SOUNDLST group players** (pick
  a random member of the contiguous id run starting at the argument, load the
  named `.RSS`, play it **one-shot** via `sub_427859`/`sub_427B36`, loop count 0)
  — so the numeric arguments (2800, 1700, 10, 20) are **sound ids**, matching
  `AudioEngine::play`'s one-shot id model (distinct from the looping
  `start_music`). (Provenance: `sub_427961` @ 0x427961, `sub_427BFB` @ 0x427BFB —
  same body.)

## The main-menu items — `sub_42B9CE` (CONFIRMED)

The `__noreturn` menu loop draws `MAINMENU.PCX` (palette `mainmenu.plt`) with
an **animated cursor sprite** blitted on top — `sub_41D957(aBombTriggerGre)` /
`sub_41DAA7` resolve `"bomb trigger green"` (the trigger-bomb ANI) as the
highlight, positioned at `getvalue(700)` x / `getvalue(701)+getvalue(702)` y.
The **item labels are baked into the PCX art**; only the cursor moves. The
selection variable (`v10` in the decompile) runs **0..6** — seven rows — and
Enter/keys dispatch it (Provenance: `sub_42B9CE` @ 0x42B9CE):

| `v10` | handler | address | what it is | `.BM`? |
|---|---|---|---|---|
| 0 | `sub_42A3F6` | 0x42A3F6 | **Play/Start** — runs a match then the results flow (owns DRAW/RESULTS/VICTORY, below) | no |
| 1 | `sub_42B0CE` | 0x42B0CE | setup screen A (`sub_42741E(0x410)`) | no |
| 2 | `sub_42B47D` | 0x42B47D | setup screen B (sibling, same `sub_42741E(0x410)`) | no |
| 3 | `sub_4080DC` | 0x4080DC | **CORRECTED: the Options screen** (19-item settings list — team play, random start, key-remap, …), NOT a map editor; see `docs/re/results-and-options.md` §3 | no |
| 4 | `sub_41302D(aCreditsBm)` | — | **Credits** — the `.BM` text viewer on `credits.bm` | **yes** |
| 5 | `sub_41431C` | 0x41431C | **CORRECTED: a generic HELP FILE BROWSER**, not Roulette — lists every `*.BM` in the install (incl. ROULETTE.BM as one topic among ~10) via a real DOS glob; see `docs/re/results-and-options.md` §4 | (BM) |
| 6 | `sub_412987` | 0x412987 | **Quit** — exit confirm + exit sting `sub_427BFB(2600)` | no |

**Row 3 and row 5 corrected (2026-07-08).** A prior pass mislabeled
`sub_4080DC` as the map editor and `sub_41431C` as "roulette" — reading
their bodies shows neither is true. `sub_4080DC` has no editor code and
never references `EDITOR.BM`; it is the interactive Options screen (team
play / random start / conveyor speed / key-remap / …), fully RE'd in
`docs/re/results-and-options.md`. `sub_41431C` has no roulette-wheel
drawing code either; it globs `*.BM` and opens whichever the player picks
in the existing `.BM` viewer — ROULETTE.BM is just one of ~10 listed help
topics. The **real** Goldman Roulette Wheel mini-game is a separate,
menu-unreachable routine (`sub_4034BC`) that runs at the head of the Play
flow (top of `sub_410F81`) when `goldman=1` and a gold player is pending —
fully RE'd in `docs/re/goldman-roulette.md`. **The map editor is now
LOCATED (2026-07-08, second pass):** it hangs off the hidden Ctrl+E ×6
trigger in this very menu loop (raw code 5, six consecutive presses →
`sub_40330E` → `sub_403184` → the full scheme editor `sub_4028D2` with a
real `.SCH` writer `sub_403C16`) — chain and controls in
`docs/re/results-and-options.md` §5. `EDITOR.BM` remains a help text only
(the string appears nowhere in the binary).

**Menu key → sound → action table (raw `sub_4102B7` codes, EXHAUSTIVE, CONFIRMED
`sub_42B9CE`).** The nav blip fires *first* for every real key, then the dispatch:

| key (v8) | sound | effect |
|---|---|---|
| any real key (`≠ -1, ≠ -2`) | `sub_427961(20)` blip | resets idle timer, then falls to the dispatch below |
| `13` Enter | `sub_427961(10)` accept | select current row (v9=1) |
| `32` Space | `sub_427961(10)` accept | select current row (v9=1, via `LABEL_59`) |
| `17` / `27` Escape | `sub_427961(10)` accept | set v10=6 (**Quit**) and select |
| `328` Up | (blip only) | `--v10`, wraps 0→6 |
| `336` Down | (blip only) | `++v10`, wraps 6→0 |
| `280` | `sub_427961(10)` accept | jump v10=3 (**Options**, corrected) and select |
| `315` | `sub_427961(10)` accept | jump v10=5 (**Help browser**, corrected) and select |
| `286` | (blip only) | break the loop → run the current selection |
| `288` | (blip only) | `sub_413D45()` — a toggle, no accept |
| `5` = **Ctrl+E** (×6 in a row) | `sub_427961(10)` accept | `sub_40330E()` — **the MAP EDITOR** (corrected: code 5 is the Ctrl+E ASCII control code, the counter is `++v15 > 5` = six consecutive presses, and the target is the scheme editor, NOT a campaign — full chain in `docs/re/results-and-options.md` §5) |
| idle > `getvalue(92)` s | (none) | **ATTRACT MODE** (corrected — it does NOT simply run the current row): sets the attract flag `dword_464938`, saves the roster/level/team config, forces v10=0 and dispatches Play as an AI-only demo match — see "Attract mode" below |

`getvalue(92)` = **30** (VALUELST `92,30`) — the menu idle timeout, gated by
`getvalue(92) > 5` (the file's own legend documents it as the attract-mode
delay, with < 5 disabling attract entirely), distinct from the waited-screen
`getvalue(12)` = 7. Only Up (328) and Down (336) plus 286/288 stop at the
blip; **every SELECT (Enter/Space/Escape/280/315) plays the accept sting
10**. On the Quit row (6, whether reached
by Escape/17/27 or Enter on the row), the dispatch calls `sub_412987`, which pops
a confirm dialog and — on confirm — plays the exit sting `sub_427BFB(2600)` (the
2600..2699 "go outside and play now!" group) before sleeping 4 s and exiting.

So the confirmed public item set + order is **Play, net-setup A, net-setup B,
Options, Credits, Help browser, Quit** with an animated bomb-trigger cursor
(the map editor and the roulette wheel exist but have no menu row — hidden
trigger / automatic respectively).

**Port fidelity of the menu sounds (FIXED).** `present_menu` now matches the
table: Up/Down play the blip 20 (correct already); **Enter/Space play the accept
10 on EVERY row** including the previously "inert" Editor/Roulette stubs (the
original has no inert-row concept — every row 0..6 is a live dispatch, so the
accept sting always fires; the unbuilt leaves just stay put after the sting
instead of dead-ending); **Escape plays 20 + 10 then the exit sting 2600** (the
"any key" blip + the 17/27 accept + the Quit-handler `sub_427BFB(2600)`, collapsed
since we have no confirm dialog); **Enter on the Quit row also plays 10 + 2600**.
The **campaign easter egg** (key 5 ×5 → `sub_40330E`) and the **280/315 direct
jumps to Editor/Roulette** target features not yet built, so they are documented
gaps — not faked. The **WASD nav aliases** (`SDLK_W`/`SDLK_S` = Up/Down) are a
deliberate modern convenience: in the binary raw 'w'(119)/'s'(115) fall through to
`LABEL_44` (blip only, no move), so binding them to nav is a superset, not a
misrepresentation.

**Cursor anchor — PINNED.** The bomb-trigger cursor's blit is
`x = getvalue(700)`, `y = getvalue(701) + getvalue(702)*row` (decompile: `v11 =
getvalue(700); v1 = getvalue(701); v12 = getvalue(702)*sel + v1; blit("bomb
trigger green", v11, v12)`). VALUELST stores these as the **columns of one
multi-value row** — `700,332,140,38,0` — and the file's own legend labels them
`X, Y - first (top) item / YS - y-spacing / W - width`. The original
`getvalue(id)` (`sub_412135`) reads a **flat array** the loader (`sub_4121FF`)
fills by splitting each `id,a,b,c` line into consecutive slots, so
`getvalue(700/701/702/703)` == columns 0/1/2/3 of row 700 == `{332, 140, 38,
0}`. Confirmed cursor: **x = 332, y = 140 + 38·row**. (The menu's own idle
timeout is `getvalue(92)`, not the waited-screen `getvalue(12)`.)

**Spine mapping.** The polished menu keeps the seven rows in the original v10
order so the cursor anchor lands on the baked labels: Play→`Match`,
setup A→Options `.BM` help, setup B→Network `.BM` help, row 3 (Options,
corrected above)→inert stub, Credits→Credits `.BM`, row 5 (Help browser,
corrected above)→inert stub, Quit→app exit. **Play, Credits, Quit are live**;
rows 3 and 5 are documented inert stubs — row 3's real screen is the RE'd
Options screen (`docs/re/results-and-options.md` §3, a separate
implementation effort) and row 5's real screen is the RE'd `.BM` help
browser (same doc §4), not a map editor / roulette wheel respectively as an
earlier pass assumed. The cursor is now the actual animated `"bomb trigger
green"` sprite (TRIGBOMB.ANI) at the pinned anchor, read live from VALUELST
row 700's columns (`ValueList::column_or`), with `{332,140,38}` as the
fallback; it falls back to a highlight bar only if TRIGBOMB.ANI is absent. The
Controllers/INPUT.BM help has no dedicated main-menu row in the original (it
hangs off the interactive controller-setup screen), so its `OpenControllers`
edge exists in the flow graph but is not bound to a menu row here — a documented
gap belonging to the deferred controller-setup UI.

## Attract mode — the menu idle timeout runs a LIVE AI demo match (CONFIRMED)

The question "does the original run a recorded DEMO match after the title
timeout, like other 1997 games?" is now settled — in two halves:

**1. There is NO recorded-input demo subsystem.** Negative, with evidence:
`sub_42B060` (boot) is a single straight line — music, logos, sting, title,
teardown, return — with no demo branch; the whole-decompile greps for
`demo`, `.dem`, `.rec`, `record`, `playback`, `attract` return zero relevant
hits (the only match is an unrelated ANI-loader diagnostic about a STAT
record); none of the ~93 `fread`/`fwrite` sites has a per-tick input-log
shape (they are stats/config/resource I/O); and the install contains no
`.DEM`/`.REC`/`.INP` or other unexplained files. The port's straight
IPLOGO → HSLOGO → TITLE → menu chain is faithful for the boot portion.

**2. But the MENU idle timeout is a real attract mode — a live, AI-only
match, not a plain "select the current row".** The idle path in
`sub_42B9CE` (pseudo.c 30887-30894) does five things before dispatching:
increments an attract counter (`dword_4642D8`), sets the **attract flag
`dword_464938` = 1**, snapshots the 10 slots' input-type/sub bytes and the
team flag via `sub_4224E2` @ 0x4224E2 (pseudo.c 24605-24620) plus the level
into `dword_4646B8`, forces team play off, and forces **v10 = 0** — so it
ALWAYS dispatches Play (`sub_42A3F6`), regardless of the highlighted row.
The flag then reroutes the whole Play flow:

- **`sub_410F81` (player setup) short-circuits** (pseudo.c 15125-15143):
  goldman wheel skipped (`!dword_464938` gate at 15048), all 10 slots set
  OFF, then `rand()%10 + 1` (clamped to a minimum of 3) slots are set to
  **COMPUTER**, the level is set to `rand() % getvalue(35)` **directly**
  (a specific random stage — bypassing the VALUELST 1150-1160 random-level
  enable flags, so attract can pick hockey rink / coal mine), and the
  function returns immediately — neither the player screen nor the LEVEL &
  ROUNDS screen (`sub_406DDE`, called at `sub_410F81`'s tail, 15516) is
  shown.
- **The round runs live** — the normal sim with AI players; nothing is
  scripted or replayed.
- **Any dispatched-through keypress aborts**: the round loop's key handler
  tail has `if (dword_464938) goto LABEL_34` (pseudo.c 29788 → 29737),
  which clears the pending gold player and sets `dword_464A68 = 2` — back
  to the menu.
- **Round end skips ALL outcome screens**: before the DRAW/RESULTS tiers,
  `if (dword_464938)` tears down and returns (pseudo.c 29812-29819,
  `LABEL_204`) — an attract match never shows DRAW/RESULTS/VICTORY. (The
  related 6 s auto-advance on those screens is gated by `sub_42247A` — an
  **all-AI-roster test**, pseudo.c 24586-24602 — which covers the
  human-configured all-CPU case, not the attract flag.)
- **Menu re-entry restores everything** (pseudo.c 30747-30754): the flag is
  cleared and `sub_422552` (24627-24642) writes the saved roster bytes,
  team flag, and level back.

`getvalue(92)` (VALUELST `92,30`) is the attract delay in seconds; the
file's own legend documents that values < 5 never enter attract mode —
matching the code's `getvalue(92) > 5` gate.

**Port status:** our boot chain is faithful; the **menu-idle attract match
is a documented gap** — reproducing it faithfully means: after 30 s of menu
idle, save the configured roster/level/team, run a live match with
`rand()%10+1` (min 3) CPU players on `rand()%11` (any stage, ignoring the
enable flags), abort to the menu on any keypress, skip all outcome screens,
and restore the configuration. All of it presentation/config-level (the sim
just receives an all-AI `MatchConfig`); the two rand draws are
presentation-side. (Provenance: `sub_42B9CE` idle path pseudo.c
30747-30754/30887-30894; `sub_410F81` attract branch 15125-15143;
`sub_42A3F6` gates 29788/29812; `sub_4224E2`/`sub_422552` 24605-24642;
`sub_42247A` 24586-24602; VALUELST 92.)

## The results / DRAW / VICTORY flow — inside `sub_42A3F6` (CONFIRMED)

The end-of-round path lives at the tail of the **Play** handler (`sub_42A3F6`,
~29820-30130), not a standalone screen. It is a **three-tier** outcome:

1. **DRAW — no survivor.** `if (sub_4219B0(v69) != -1) goto RESULTS;` — the
   survivor query returns the lone survivor's index, or **-1 for none**. With no
   survivor it draws `sub_42A088(aDraw, 0)` (`aDraw = "draw"` → DRAW.PCX) then
   `sub_427BFB(1700)` (the **draw sting, SOUNDLST 1700**), and runs a bespoke
   wait: nav-blip on key, Esc→27, and **`(attract || flag) && time > t0+6000`
   → key=13** — a 6 s auto-advance in attract mode.
2. **RESULTS scoreboard — a survivor exists.** Loads `aResultsPlt`
   (`"results.plt"` → RESULTS.PCX) and prints each player's win tally, computing
   `v73` = the index that reached the match-win threshold (`dword_464A7C` =
   wins-needed), or **-1 if nobody has clinched the match yet**.
3. **VICTORY — the match is won (`v73 != -1`).** Team game → `aTeamU`
   (`"team%u"` → TEAM0/TEAM1.PCX); else → `aVictoryU` (`"victory%u"` →
   **VICTORY0..VICTORY9.PCX**, one per winner index). Draws that named screen
   `sub_42A088(v66, 0)`, dwells `sub_413CB0(3000)` (3 s), and plays the "we have
   a winner" voice group `sub_427BFB(2000)`. (Provenance: `aDraw`/`aResultsPlt`/
   `aTeamU`/`aVictoryU` string table @ 1612-1615; blit sites @ 29825/29888/30130.)

**Note on BONUS.PCX — CONFIRMED DEAD ASSET (2026-07-08).** The install ships
`BONUS.PCX`, but it is **not** referenced by this flow — the per-round outcome
is DRAW (no survivor) or the RESULTS tally (a survivor), and the match winner
is VICTORY%u. The earlier spine guess "round-win = BONUS" was wrong; the
faithful round/match-win screen is `VICTORY<player>`. A binary-wide search now
closes the question: a case-insensitive grep for `bonus` over the whole
decompile returns **zero hits** (no `aBonus*` string constant exists at all),
DATA/RES ships no `bonus.plt` companion palette, VALUELST/MESSAGES contain no
bonus-screen legend or string, and no `.RES` list names it (the only install
file containing the word is ROULETTE.BM's help prose). BONUS.PCX is leftover
art from a cut feature — nothing in the shipped binary can display it, so the
port owes it nothing.

**Spine mapping.** The sim does not yet model teams, so Results shows **DRAW**
(no survivor / time-up, `round_winner()` returns -1) or **`VICTORY<player>`**
(one survivor, naming that player), each as a normal `Screen` with a bounded 6 s
dwell (the `sub_42A3F6` attract auto-advance) then a return to the menu. The
winner voice group (2000) is played by `run_match` on match-over, matching
`sub_427BFB(2000)`. TEAM%u is a documented future hook (needs sim team state).

**Results MUSIC — 1020 (win) / 1130 (draw) (FIXED).** `sub_42A3F6` starts the
looping **"win" track `sub_42741E(0x3FC)` = 1020** at handler entry (played under
the VICTORY screen), and the DRAW branch switches to the **"draw" track
`sub_42741E(0x46A)` = 1130** right before `sub_42A088(aDraw, 0)`. Both are looping
`start_music` tracks that *replace* whatever was playing (stage music). The port's
Results branch previously started no results music — the DRAW/VICTORY screens
played under the leftover stage/menu track. It now calls `start_music(1130)` for
DRAW and `start_music(1020)` for VICTORY (WIN.RSS + DRAW.RSS both ship), closing
that silent-vs-original gap. (0x3FC=1020, 0x46A=1130 — hex confirmed; SOUNDLST
labels `win`/`draw`.)

**SFX 40 (enrt1, "you can't do that here") — NETWORK-ONLY, N/A to our build.**
The results/draw wait loops fire `sub_427961(40)` when a key is pressed but
`sub_40C06A() == 1`. `sub_40C06A` is a bare `return dword_460058;` — the game-mode
global (0 = local, 1/2 = the two network roles; CORRECTED: mode 2 is NOT a
"demo" mode — its only writer is the setter `sub_40C035` @ 0x40C035, whose only
non-zero call is `sub_40C839(2)` at the top of the net-game screen `sub_42B0CE`;
no code path ever sets it for an unattended/demo run — see "Attract mode" below
for what the real demo path uses instead). So enrt1 is the "can't dismiss this
yet" buzz a **networked non-host** hears instead of the accept sting; every other
SFX-40 site in the front end (`sub_42B0CE`/`sub_42B47D` network-setup screens,
lines ~6102/8230/15390) is likewise gated on `sub_40C06A() == 1`. Our front end is
local-only (mode 0), so **SFX 40 can never trigger in the boot/menu/results path**
— it is correctly absent, not a missing sound. (Provenance: `sub_40C06A`
@ 0x40C06A = `return dword_460058`; SOUNDLST `40,enrt1` under the `; some kind of
"you can't do that here" sound` comment.)

**Draw-sting fidelity (fixed).** `sub_427BFB(1700)` plays a random member of the
contiguous SOUNDLST run beginning at 1700 (the file's own `; tie game/draw game`
comment) **once**. The spine previously routed 1700 through `ScreenDef.music_id`,
which *loops* the track — a wrong, repeating sting. It is now a **one-shot**:
`run_app` calls `audio_.play_random_in_range(1700, 1799)` when entering a DRAW
and the DRAW `ScreenDef` has `music_id = -1` (no looping music). The winner path
was already a one-shot (2000-group in `run_match`).

**RESULTS tally tier — RE'd, port still DEFERRED.** The middle tier (a survivor
exists but nobody has clinched the match: `sub_42A3F6` loads RESULTS.PCX and
prints each player's cumulative win tally against `dword_464A7C` = wins-needed)
is now fully RE'd — layout, getvalue ids, message ids, the two independent
packed win/kill counters (`sub_421AC8`/`sub_421B0F`), and the clinch/outcome
logic — in `docs/re/results-and-options.md` §1. It still needs a
**multi-round match structure with a running scoreboard** the spine does
not have yet — `run_app` plays a single round then returns to the menu. Adding it
faithfully means tracking per-player cumulative wins + the match-win threshold
across rounds and rendering the tally on RESULTS.PCX (with the FONT6 glyph draw,
below). Left as the next chunk; the per-round DRAW/VICTORY screens + their stings
are correct today.

## The `.BM` text-screen viewer — `sub_41302D` (CONFIRMED) + the FON font

The credits/help screens (`CREDITS.BM`, `OPTIONS.BM`, `NETWORK.BM`, `INPUT.BM`,
…) all render through **one routine, `sub_41302D`** (@ 0x41302D). Confirmed
behaviour, now implemented in `libs/game` (`bmscreen.{hpp,cpp}` + the `bmtext`
parser + the new `bmfont` parser):

- **Two passes over the file** (pass 1 counts lines, pass 2 lays out), reading
  `fgets`-style in text mode — exactly what the `bmtext` parser already mirrors
  (CRLF/`0x1A`-EOF handling, 4-column tab stops, one `<IMG>` tag form).
- **Layout (confirmed literals):** text starts **34 px** from the top of the
  scroll region, one line per row at the **font cell height**; the left inset is
  **34 px**; the on-screen row count is `v60 = 344 / line_height`; each `<IMG>`
  segment blits its named PCX inline and advances the pen past it.
- **Scroll is keyboard-driven, one line at a time — there is NO auto/timed
  scroll.** Up (`328`) `--v54`, Down (`0x150`) `++v54`, PgUp (`0x149`) `v54 -=
  v60-1`, PgDn (`337`) `v54 += v60-1`, clamped to `[0, count - v60]`. **Enter
  (13) or Escape (27) dismiss** the viewer (`LABEL_100` sets the done flag on
  both). So the earlier task assumption of a "scroll speed" constant does not
  exist — nothing to guess. (Provenance: `sub_41302D` scroll loop @ ~16420-16590.)
- **Font:** `sub_41302D` draws with the **active font**, which graphics-init
  pins to **FONT6** via `sub_431E9C(6)` at the end of `sub_414DF4` (@ 0x417600).
  The text colour is the global draw index `byte_49D38F`. The `FONT<n>.FON`
  files live in the **install root** (not under DATA/). See `docs/formats/fon.md`
  for the decoded format; the port renders glyphs in truecolour (the paletted
  `byte_49D38F` index → a fixed light ink on a dark panel — a cosmetic port
  choice, layout/advance are faithful).

**Interactive settings — Options screen BUILT (clean-room) and now RE'd;
controller-remap RE'd, port still DEFERRED.** These `.BM` files are the **HELP
overlays** listed by the help browser (`sub_41431C`, corrected above). The
**Options screen** (`libs/game/src/options_screen.cpp`) is a fully-interactive
Team Play / Conveyor Speed editor persisting to `options.ini`
(read-modify-write, `bomber::assets::save_options`); its F1 key still reaches
OPTIONS.BM. It was built clean-room against the glue-screen conventions
(random `GLUE<n>` backdrop, FONT6 text, SFX 20 nav / 10 accept) BEFORE the
real screen was RE'd; the original (`sub_4080DC` — a 19-item list including
team play, random start, conveyor speed, "Define keyboard layouts", persisting
to `options.ini` only on app exit via `sub_405DE3`/`sub_410EBF`) is now fully
pinned in `docs/re/results-and-options.md` §3 (+ the complete 22-key
`options.ini` table), so aligning our screen's item list/layout/write-timing
to it is a tracked follow-up. The **key-remap UI** (`sub_407B9D`, reached from
the Options screen's "Define keyboard layouts" row — NOT the
`sub_42B0CE`/`sub_42B47D` net-game screens) is RE'd in the same doc §2;
porting that widget is still a separate implementation effort. The `AppState`
hooks (`Options`, `Network`, `Controllers`) are in place; Network/Controllers
currently show the help text.

## The transition — HEADWIPE.ANI, driven by the standard ANI pacer

There is **no `headwipe` string** anywhere in the decompile, yet
`DATA/ANI/HEADWIPE.ANI` ships. The engine therefore loads it the same generic
way it loads every other ANI — by a runtime-built `DATA/ANI/<name>.ani` path
(`aSSSAni = "%s/%s/%s.ani"`) — and plays it as a screen-to-screen wipe overlay.
Its internal contents (via `strings`) are a **single sequence** built from one
`HEAD`/`HEAD0` STAT with a run of FRAM leaves (`FOA0000.TGA`, …): a growing
mask, exactly a horizontal head-to-head wipe.

Playback obeys the already-confirmed universal ANI rule (`docs/re/facts.md`
"ANI per-step timing — CONFIRMED INERT", `sub_41DAA7`): the shown step is
**`counter % statecnt`**, the owner advancing `counter` once per rendered
frame. There is no per-step duration; the wipe simply steps one frame per tick
until it has shown every step once. The spine's transition primitive advances a
frame counter over the `HEADWIPE` sequence at the front-end frame rate and is
"done" after `statecnt` frames — faithful to that driver. When HEADWIPE.ANI is
absent the spine falls back to a wall-clock alpha fade (cosmetic only).

**Scope: the wipe is NOT played on waited screens.** `sub_42A088` cuts (palette
+ blit + flip, above) — it does not invoke HEADWIPE. So the logos, title,
results, and `.BM` viewer all appear/dismiss by a cut, with no wipe. The port
keeps the HEADWIPE transition only for the **menu→match select** step
(`present_menu`), which is the one place a head-to-head wipe belongs; the earlier
spine also ran the wipe out of every waited screen, which was wrong and has been
removed.

## Audio: looping music vs one-shot SFX — the port model (matches the binary)

The binary keeps **one looping music channel** and a **separate one-shot SFX
pool** (both facts confirmed above from `sub_4273A4` loop `0xFFFF` vs
`sub_427B36` loop `0`). `AudioEngine` mirrors this exactly and needed no new
plumbing for this flow:

- **`AudioEngine::start_music(id)`** owns the single looping music stream
  (`music_stream_`): it loads the id's `.RSS`, clears the stream, and queues the
  samples; `update_music()` re-queues the clip when the buffered audio drops
  below a threshold — a faithful stand-in for the original's `0xFFFF` infinite
  loop. Calling it again with a new id **replaces** the current track (clears +
  re-queues), matching `sub_4273A4`'s "free the old handle, start the new one".
- **`AudioEngine::play(id)`** plays a one-shot clip on an 8-voice SFX pool,
  flushing so the voice frees when it ends — the analogue of the SFX players
  (`sub_427859`/`sub_427B36`, loop 0). Stings (2800/1700/2000/2600) and blips
  (10/20) all go through here, so **they never disturb the looping music**.

The **boot-music model** at the port level (`GameApp::run_boot_attract`,
`present_menu`):

1. `run_boot_attract` calls `start_music(1000)` **once**, before presenting
   IPLOGO — the boot track then loops continuously through the logos and the
   title (no per-screen restart, because `Screen` never touches music).
2. The one-shot title sting `play(2800)` fires right before the title image,
   over the still-playing boot track.
3. `present_menu` calls `start_music(1010)` on menu entry, which replaces the
   boot track with the menu track and keeps it looping while in the menu.
4. Screen skips (`Screen::on_key`) only ever call `play(20)` / `play(10)` — one
   shots — so no skip stops the music; only the presented image changes.

This is presentation-only (no `libs/sim` involvement); the front-end's cosmetic
randomness (SFX group pick) uses `AudioEngine`'s own LCG, never `State::rng`.

## Tunables (all front-end / presentation, zero determinism impact)

| id | meaning | value | status |
|---|---|---|---|
| `getvalue(12)` | attract / auto-advance delay for a waited screen | **7 s** (VALUELST `12,7`) | CONFIRMED value + source: `sub_42A088` waits `time_()` (seconds) to `start + getvalue(12)`; port uses 7000 ms for logos + title. Timeout synthesizes Enter → plays SFX 20 + 10 (audible advance) |
| `getvalue(92)` | main-menu attract-mode delay | **30 s** (VALUELST `92,30`) | CONFIRMED: after `getvalue(92)` s idle (gated `> 5`; legend: < 5 disables attract) `sub_42B9CE` enters ATTRACT MODE — saves config, forces Play, runs a live all-CPU demo match (see "Attract mode"); distinct from `getvalue(12)` |
| `getvalue(700/701/702)` | main-menu cursor x / y-base / y-step | **332 / 140 / 38** (VALUELST `700,332,140,38,0`) | CONFIRMED anchor + values (`sub_42B9CE`): x=getvalue(700), y=getvalue(701)+getvalue(702)·row |
| SOUNDLST 1000 | boot/title music (`title`), looping, started ONCE in `sub_42B060`, continuous across logos+title | TITLE.RSS | CONFIRMED (`sub_42741E(0x3E8)` @ boot, loop 0xFFFF) |
| SOUNDLST 1010 | main-menu music (`menu`), looping, started on menu entry (`sub_42741E(0x3F2)`, v14-gated) — replaces the boot track | MENU.RSS | CONFIRMED (`sub_42B9CE`); NOT 0x3FC/1020 (that is `sub_42A3F6`'s round "win" track) |
| SOUNDLST 1020 | results "win" music (`win`), looping, started at `sub_42A3F6` entry (`sub_42741E(0x3FC)`) — under the VICTORY screen | WIN.RSS | CONFIRMED; port starts it for VICTORY |
| SOUNDLST 1130 | draw-screen music (`draw`), looping, DRAW branch (`sub_42741E(0x46A)`) — under DRAW.PCX | DRAW.RSS | CONFIRMED; port starts it for DRAW |
| SOUNDLST 10 | menu-exit / accept sting (`menuexit`), one-shot | MENUEXIT.RSS | CONFIRMED (`sub_427961(10)` accept path in `sub_42A088` + every menu select in `sub_42B9CE`) |
| SOUNDLST 20 | nav blip (`letter1`), one-shot, on ANY key | LETTER1.RSS | CONFIRMED (`sub_427961(20)` in `sub_42A088`/`sub_42B9CE`/`sub_42A3F6`) |
| SOUNDLST 40 | "you can't do that here" buzz (`enrt1`), one-shot | ENRT1.RSS | CONFIRMED **NETWORK-ONLY** (`sub_427961(40)` gated on `sub_40C06A()==1` in the results/setup wait loops); never fires in local play — correctly absent in the port |
| SOUNDLST 1700 | draw-screen sting group (`draw`/gump1, 1700..1999), one-shot random pick | GUMP1.RSS + group | CONFIRMED (`sub_427BFB(1700)` in `sub_42A3F6`); port uses `play_random_in_range(1700, 1999)` |
| SOUNDLST 2000 | "we have a winner" voice group (2000..2299), one-shot random pick | PROUD.RSS + group | CONFIRMED (`sub_427BFB(2000)` on VICTORY); port `play_random_in_range(2000, 2299)` |
| SOUNDLST 2600 | menu-quit / exit sting group (2600..2699), one-shot random pick | QUITGAME.RSS + group | CONFIRMED (`sub_427BFB(2600)` in quit handler `sub_412987`); port plays it on menu Quit/Escape |
| SOUNDLST 2800 | title intro sting group (2800..2810 "ATOMIC BOMBERMAN!"), one-shot random pick | GEN8A.RSS + group | CONFIRMED (boot `sub_427BFB(2800)`); port `play_random_in_range(2800, 2899)` |

The logo/title dwell is **no longer a tunable — it is CONFIRMED `getvalue(12)` =
7 s** (VALUELST `12,7`), the same waited-screen timeout for all three boot
screens; the port hard-codes 7000 ms from it. The one remaining **our tunable**
in the front-end is the HEADWIPE frame cadence in ms for the surviving
menu→match select wipe (the original steps per rendered frame; the spine uses a
fixed ms-per-frame so it is resolution-independent). Neither touches the sim.
