# RESULTS tally, key-remap, Options screen, Roulette row, and the map editor — RE

Reverse-engineered from `BM95.EXE` (pseudo.c) and the shipped
`DATA/RES/VALUELST.RES` / `MESSAGES.TXT` (structure and ids only — text and
values stay in the install, never committed; see `docs/re/setup-screens.md`
and `docs/re/frontend-flow.md` for the companion facts this doc extends).

## 1. RESULTS cumulative-tally screen — `sub_42A3F6` middle tier (CONFIRMED)

`docs/re/frontend-flow.md` flagged the RESULTS tier of the Play handler
(`sub_42A3F6`, pseudo.c ~29886-30106) as deferred when this section was
written, because the spine at the time played one round and returned to the
menu. The screen logic itself is fully present in the decompile; the
multi-round match/scoreboard *state* that was then the missing piece has
since shipped (ROADMAP "Multi-round best-of-N loop + RESULTS tally 1:1 —
DONE 2026-07-08"; `docs/re/frontend-flow.md` now reads "RESULTS tally tier
— RE'd, port DONE"). Confirmed layout:

- **Backdrop:** `aResultsPlt` = `"results.plt"` → **RESULTS.PCX** (palette
  loaded via `sub_411D17`/`sub_4151CC` @ 29888-29889), a plain image load —
  no `sub_42A088` screen primitive is used here (this tier draws directly
  into the still-active game surface, `dword_464AE4`).
- **Header line** — only drawn once per round (`if (!dword_464AEC)`): text
  `getstring(30)` (`"Game Winner was %s !"`, formatted with the round's
  winner name), at **x = getvalue(780), y = getvalue(781), colour =
  getvalue(783)** (VALUELST legend `; WINNER SCREEN` / `; "Winner was:"` →
  `780,150,140,0,400`: X=150, Y=140, YS=0, W=400 — width unused for a
  single line). Ink = `byte_49D38F`, background = `byte_495390[0]`.
- **Per-player tally rows** — `for player in 0..9` (or `for team in 0..1` in
  team mode, `dword_464964`): each active slot's line is `getstring(31)`
  (`"Player %u score: %u (kills: %d)"`, non-team) or `getstring(38)`
  (`"Team %u score: %u"`, team mode), drawn at **x = getvalue(785), y =
  getvalue(786) + getvalue(787)·row, colour = getvalue(788)** (VALUELST
  `; each individually-listed player` → `785,150,210,20,400`: X=150,
  Y0=210, YS=20, W=400). Score = `sub_421AC8(i)` (cumulative **match win
  count**, packed in the high word of `dword_461C2C[38*i]`, `>>16`); the
  displayed "kills" figure in the non-team format is `sub_421B0F(i)` (a
  **different** high-word field two ints later in the same 152-byte
  record, `dword_461C2C[38*i+2] >>16`) — i.e. two independently-tracked
  packed counters per player, NOT the same value formatted twice.
  Per-player ink: `sub_41672F(i)` (non-team) / `sub_4141F8(team)` (team
  mode) — the same colour helpers already pinned in
  `docs/re/player-colour.md` and `docs/re/setup-screens.md`.
- **Match-clinch check (its result lands in the winner variable, -1 =
  nobody has clinched yet):** the check splits on **team mode**
  (`dword_464964`, `sub_42A3F6` batch_0x4293E5.cpp:1189-1255).
  - **Team branch (1189-1221):** ALWAYS wins-based — the first team whose
    `sub_421AC8` (win count) reaches `dword_464A7C` (the configured "number of
    wins to clinch the match", `num_to_win_match=`). `win_by_kills` is never
    read here.
  - **Non-team branch (1222-1255):** if `win_by_kills` (`dword_46497C`) is set
    it compares the **highest round-kill total** against `dword_464A7C`,
    breaking ties by requiring the count of players tied at that top total to
    be exactly 1, i.e. a single unique leader; otherwise
    it is the same wins-based check.
  CORRECTED 2026-07-22: `win_by_kills` is a **non-team** feature — team play
  forces it OFF (`batch_0x405B3A.cpp:685-686`, mirrored at
  `options_screen.cpp`'s `activate_row`), so the kill-clinch belongs on the
  NON-team path. The earlier "in team mode with win_by_kills" wording here was
  the root cause of an inverted, dead `is_team_mode() && win_by_kills` gate in
  `game_app.cpp match_clinch()` (fixed the same day).
- **Outcome line** — drawn after the per-player list, at **x =
  getvalue(800), y = getvalue(801), colour = getvalue(803)** (VALUELST
  `; player %u wins the match...` → `800,150,94,0,400`):
  - **No clinch yet (the winner variable still holds -1):** the string id is
    120 plus the `win_by_kills` flag `dword_46497C`, and it is formatted
    with `dword_464A7C` (the **flat target**, NOT the remaining count) — id 120
    `"(Match winner must score %u victories)"` or id 121 `"(... %u kills)"`,
    keyed on `win_by_kills` (a non-team feature), stating the TOTAL goal. Ink =
    `byte_49A624` (a distinct "still playing" colour).
  - **Match clinched (the winner variable holds something other than -1):**
    keyed on `win_by_kills` (`dword_46497C`),
    NOT team mode: set → `getstring(36)` = `"PLAYER %u WINS THE MATCH!"`
    formatted with the winning player number, i.e. the winner index plus 1;
    unset → `getstring(35)`
    = `"%s WINS THE MATCH!"` formatted with the winner name. There is NO
    team-specific "TEAM wins" string on this path. Ink = `byte_497F8F` (a
    distinct "match over" colour, different from the still-playing ink above).
- **Sound:** the winner voice group **`sub_427BFB(2000)`** ("we have a
  winner", already pinned in frontend-flow.md) fires as soon as the clinch
  check yields a winner (the winner variable stops being -1) — i.e. it plays under the RESULTS scoreboard itself, not only
  under the later VICTORY screen. A short **1500 ms** dwell
  (`sub_413CB0(1500)`) follows before input is accepted.
- **Music:** inherited — this tier draws inside the same handler as DRAW/
  VICTORY and does not call `sub_42741E` itself, so it plays under
  whatever the handler already started (1020 "win", per frontend-flow.md).
- **Input / advance:** a `getkey` loop identical in shape to the DRAW wait
  (frontend-flow.md): any real key → SFX 20; **Enter (13) or Space (32)**
  advance (SFX 10); **Esc/anything ≤ 27** advances too
  (`dword_464A68 = 2`, i.e. "abort to menu"); and an **auto-advance** at
  `t0 + 6000 ms` when `sub_42247A()` — CORRECTED: an **all-AI-roster
  test** (@ 0x42247A, pseudo.c 24586-24602: returns 1 iff no slot's
  input-type byte +16 is a human category 2/3/4), NOT an idle-timer
  helper — or the `dword_4646B4` flag is set. I.e. the results screens
  dismiss themselves after 6 s only when nobody human is playing (an
  all-CPU roster; see frontend-flow.md "Attract mode"). Network-only: `sub_40C06A()==1` non-host
  players get the SFX-40 "can't dismiss" buzz instead (see
  frontend-flow.md's SFX-40 note — identical gating, N/A to local play).
- **After RESULTS:** if the match is not yet clinched and fewer than 2
  players remain active (the active-player tally comes out below 2, i.e.
  everyone but one has been
  eliminated from the roster entirely, not just the round), a "not enough
  players" dialog (`getstring(47)`/`getstring(95)`) aborts to the menu;
  otherwise `sub_410B6E()` (match/level re-init) runs another round. If the
  match IS clinched, the next screen is **TEAM%u/VICTORY%u** (frontend-
  flow.md's tier 3), drawn immediately after with a 3 s dwell.

**Reproduction status:** unchanged from frontend-flow.md's assessment — the
per-round DRAW/VICTORY screens are already faithful; RESULTS needs a
persistent per-player win-count (`sub_421AC8`'s field) and round-kill count
(`sub_421B0F`'s field) carried across rounds within one match, plus the
render pass above. No sim/determinism impact (presentation tally only,
`Player` kill/win counters for this purpose are not part of `libs/sim`'s
hashed state and would live in `libs/game`/`libs/match` match-scope data).

### The screen-ink byte globals — PINNED (RGB555 offsets into the shared LUT)

The five palette-index globals this doc's render pass cites
(`byte_49D38F`/`byte_49A624`/`byte_497F8F`/`byte_495390[0]`/`byte_49D0DA`,
plus `sub_4141F8`'s team-ink pair) are **never assigned anywhere in the
decompile** — every other confirmed-runtime-written byte array in the same
region (e.g. `byte_460BD0`/`BDA`/`BE4`, the `.RMP` tail bytes,
`docs/re/player-colour.md`) has visible store sites; these do not. The
resolution: they are not independent bytes at all. IDA's array-bounds
heuristic under-sized `byte_495390[8456]` — the true buffer is a **65536-byte
RGB555 → palette-index LUT** (three sibling buffers at `0x465390`/
`0x475390`/`0x485390`, each exactly `0x10000` apart, confirm the stride; this
is the SAME LUT `sub_41672F` indexes as `byte_495390[b5 | (g5<<5) |
(r5<<10)]`, `docs/re/player-colour.md` §"Setup-screen slot ink"). Every
"byte_49XXXX" ink global is simply a **fixed-offset element of that one LUT**
— IDA only gave it its own name because call sites happen to reference it by
a literal address instead of a computed index. Decoding `addr - 0x495390` as
an `r5|g5<<5|b5` RGB555 packed index (same bit layout `sub_41672F` builds)
yields clean primary/secondary colours:

| global | LUT offset | r5,g5,b5 | RGB555 target | role |
|---|---|---|---|---|
| `byte_49D38F` | 0x7FFF | 31,31,31 | white | general draw ink (header, most UI text) |
| `byte_49D0DA` | 0x7D4A | 31,10,10 | red-ish | `sub_4141F8`'s team-1 ink |
| `byte_49D37A` | 0x7FEA | 31,31,10 | yellow-ish | (seen alongside 49D38F in the editor's powerup sub-screen, §5) |
| `byte_49A624` | 0x5294 | 20,20,20 | mid-grey | "still playing" outcome ink |
| `byte_497F8F` | 0x2BFF | 10,31,31 | cyan-ish | "match over" ink |
| `byte_495390[0]` | 0x0000 | 0,0,0 | black | background ink (trivial: offset 0) |

**Verified against the install's own PCX palettes** (runtime data inspection,
not committed): the LUT is a nearest-colour map from a *target* RGB to
whatever palette is active, so `byte_49D38F` (say) resolves to "the active
palette's nearest entry to pure white". `present_scoreboard`'s RESULTS tier
draws directly into the still-active GAME surface without installing
RESULTS.PCX's own embedded palette (`sub_4151CC` @ 29889 decodes pixels into
`dword_460BC4` only — no `sub_42C534` hardware-palette-install call, unlike
its sibling `sub_41522D` which does call it after `sub_415150`). So the
palette in effect for this tier's ink lookups is whatever the just-finished
ROUND left active — a `FIELD<n>.PCX`-family stage palette. Querying
`DATA/RES/FIELD0.PCX`, `FIELD5.PCX`, `FIELD10.PCX`, and `MAINMENU.PCX`'s
embedded 256-colour trailers (`tail -c 768`, PCX's standard 0x0C-marked
VGA palette block) for the nearest entry to each RGB555 target above gives
**byte-identical results across all four files** (idx 255/254/178/97/182,
confirming a shared "reserved UI colours" region of the master palette, not
per-stage noise):

- **general ink (`byte_49D38F`) = RGB (255, 255, 255)** — exact hit (dist²=0).
- **team-1 ink (`byte_49D0DA`) = RGB (252, 80, 80)** — dist²=17, effectively exact.
- **"still playing" (`byte_49A624`) = RGB (168, 168, 164)** — dist²=32, effectively exact.
- **"match over" (`byte_497F8F`) = RGB (96, 252, 252)** — dist²=214, close.
- **background (`byte_495390[0]`) = RGB (0, 0, 0)** — trivial (offset 0 decodes to black regardless of the active palette; every checked PCX also has a literal (0,0,0) at index 0 as the VGA authoring convention).

`sub_4141F8`'s ELSE branch (non-team-1, i.e. team 0 or non-team mode) is
`byte_49D38F` (general/white ink) — confirmed directly from its body
(pseudo.c 16920-16930: it returns `byte_49D0DA` when its argument is
non-zero and `byte_49D38F` otherwise, and nothing else).

These are FIXED engine-chrome colours (not per-slot/per-player data), so the
port hardcodes the resolved RGB triples as named constants in
`present_scoreboard` rather than routing through `AssetStore` — there is no
"active palette" concept in the truecolour renderer for this LUT to look up
against; the RGB555 → palette-index → RGB round-trip only matters for the
original's paletted framebuffer, and the target RGB triple IS the intended
colour once nearest-match is resolved to an exact/near-exact hit as above.

`byte_49D0DA`'s second use, §4's `HelpBrowser` error dialog ink, is now also
hardcoded from this same table (`kErrorInkR/G/B` = (252, 80, 80),
`libs/game/src/bmscreen.cpp`) rather than a placeholder grey — the two call
sites (team-1 player ink here, and the "manual disabled"/"no help files"
dialogs in §4) are confirmed the SAME LUT element, not independently-tuned
reds that merely resemble each other.

(Provenance: `sub_4141F8` @ 0x4141F8 pseudo.c 16920-16930; `sub_41672F`
@ 0x41672F pseudo.c 18464-18492 [`docs/re/player-colour.md`]; `byte_495390`
LUT sibling stride `0x465390`/`0x475390`/`0x485390`/`0x495390` each
`0x10000` apart, pseudo.c global-declaration block ~3455-3480; `sub_4151CC`
@ 0x4151CC pseudo.c 17691-17703 [no `sub_42C534` call, contrast
`sub_41522D` @ 17713-17736 which does]; `sub_42C534` @ 0x42C534 [hardware
active-palette install]; palette query against the install's
`DATA/RES/FIELD0.PCX`/`FIELD5.PCX`/`FIELD10.PCX`/`MAINMENU.PCX` trailers,
2026-07-08.)

(Provenance: `sub_42A3F6` @ 0x42A3F6 tail, pseudo.c 29886-30106; `sub_421AC8`
@ 0x421AC8, `sub_421B0F` @ 0x421B0F, pseudo.c 24112-24129; VALUELST
`; WINNER SCREEN` block 780/785/790/795/800; MESSAGES.TXT ids 30/31/35/36/
38/47/95/120/121.)

## 2. The real key-remap UI — `sub_407B9D` (CONFIRMED), NOT on the net-game screens

`docs/re/setup-screens.md`'s CORRECTION already established that
`sub_42B0CE`/`sub_42B47D` are the START/JOIN NET GAME screens, not a
controller/key setup UI, and left "the remap UI (if any) is elsewhere" open.
It is not "elsewhere" in the sense of a dedicated main-menu row — **it is
item 15 of the interactive Options screen**, `sub_4080DC` (see §3 below):
selecting **"Define keyboard layouts"** invokes `sub_407B9D` @ 0x407B9D.

Confirmed screen layout and flow (2026-07-13 full re-read — CORRECTS the
default scancodes, the name-table size, and the capture's "Esc cancels"
claim below, and adds the mouse/widget facts the 1:1 port rebuild needed):

- **Backdrop:** no new `sub_4148E5`/`sub_42A088` call — and "draws over the
  Options frame" is now precise: every frame of `sub_407B9D`'s loop starts
  with `sub_415CA4()`, a plain `memcpy` of the SAVED BACKDROP STORE
  (`dword_460BCC`, filled by the image loads) back onto the work surface
  (pseudo.c 18131-18139). That store holds the Options screen's random GLUE
  picture only — the Options ROWS were never saved into it — so this screen
  shows the GLUE backdrop with its own chrome over it, not the option rows.
  Header `getstring(1100)` ("Keyboard definitions") at **(20, 20) clip
  400** via `sub_41696C`, ink `byte_49D37A` yellow (x recovered from the
  raw EXE bytes @ 0x407BD0; an earlier pass misread the clip width as the
  x, "(400, 20)").
- **The screen is MOUSE-driven, with a visible cursor.** `sub_407B9D`
  brackets its whole loop in `sub_431178()` / `sub_431360()` — the widget
  library's mouse-cursor draw/undraw pair (the same pair `sub_41485A`'s
  list dialogs use), so the cursor shows for exactly this screen. The
  cursor is the widget library's default **8×8 bitmap** (`byte_45C310`,
  armed by `sub_430EDC(0, …)` with hotspot (1,1); `sub_430E4C` remaps it in
  place: cell 15 → white `byte_49D38F`, 1 → `byte_497498` (LUT 0x2108 →
  (60,68,56), the bevel-dark element), 0 → the transparent key). The
  64-byte pattern, read from BM95.EXE data @ VA 0x45C310 (W=white, 1=dark,
  .=transparent):

  ```
  1111111.
  1WWWWW1.
  1WWWW11.
  1WWWW11.
  1WWWWW11
  1W11WWW1
  11111WW1
  ....1111
  ```
- **2×6 button grid** — `for keyboard_set in 0..1, for action in 0..5`: a
  REAL bevel-button widget (`sub_432298`, id = `1000*keyboard_set + action
  + 1000`) at **x = 320·keyboard_set + 100, y = 60·action + 60**, labelled
  `getstring(1110)` ("Key %u, %s") formatted with the **0-based** set
  number and the action name `getstring(1120+action)` (action names: **1120
  Move Up, 1121 Move Right, 1122 Move Down, 1123 Move Left, 1124 Action 1,
  1125 Action 2** — six bindable actions per keyboard set, matching
  options.ini's `keydef=<set>,<action>,<scancode>` triples). Buttons are
  created ONCE (first loop pass) and destroyed on exit (`sub_43322C` × 13).
  Below each button, the **currently-bound key's name** is shown via
  `getstring(1140)` ("Key: '%s'") at (x, y+22) clip 200, white ink —
  CORRECTED: the name table `off_45B914` has **0x59 = 89 entries** (not
  256), indexed by `code & 0x7F`, and the line is **not drawn at all** when
  the masked code is ≥ 0x59 (pseudo.c 8743-8744). The masking makes the
  E0-extended arrow codes alias their numpad names — the default set 0
  displays as `(8)Up / (6)Right / (2)Down / (4)Left / Space / Enter`. The
  table content is pinned verbatim in the port
  (`libs/game/include/bomber/game/dos_scancode.hpp`; read from the EXE data
  2026-07-13). An unbound 0 still has a table entry (the empty string), so
  it shows `Key: ''`.
- **Clicks arrive through the key queue.** The widget pump (`sub_432998`)
  shows a button's "down" bitmap while the mouse is held INSIDE it and
  posts its id into the same `sub_4102B7` getkey stream on RELEASE inside;
  `sub_407B9D` just reads ids back out of getkey (1000..2998 grid, 999
  defaults). There is NO keyboard navigation over the grid.
- **Rebind interaction:** a grid click calls **`sub_407AD9(button_label)`**
  @ 0x407AD9 — a modal capture through `sub_412E33`'s completion window
  (the WINZ percent dialog, frontend-flow.md): `sub_412E0C` copies
  "Press key for '%s'" (`getstring(1105)`, formatted with the button's own
  label) into the caption buffer, the window shows it with a `"%d%%"` (=
  "0%") yellow readout and the empty 300-px track, then the routine waits
  500 ms (`sub_413CB0(500)`), flushes the key queue (`sub_41043C`), and
  polls the raw **256-byte keyboard-state array** (`byte_4A2BA0[256]`)
  every frame until a key is down. The 0..255 ascending scan keeps
  OVERWRITING its result, so the highest held index wins. CORRECTED — there
  is **no Esc-cancel**: the caller stores the return value into
  `dword_4645BC[10*set+action]` UNCONDITIONALLY (pseudo.c 8779-8780), and a
  pressed Esc is itself seen by the raw poll (scancode 1), so **Esc binds
  Esc**; the only way the earlier-documented "returns 0" happens is the
  race where the queued Esc arrives after the key is already released — a
  timing artifact that stores 0 (unbound), not a cancel that preserves the
  old binding. No validation, duplicates across actions are allowed (only
  options.ini's *reader* clamps `set∈[0,1]` / `action∈[0,9]` — the array is
  sized for 10 actions per set even though the UI exposes 6; slots 6-9 per
  set are write-only from options.ini and have no in-game rebind UI).
- **Restore defaults button** — widget id 999 labelled `getstring(1130)`
  ("Return to default keys") at (40, 430); it calls `sub_40614A()` and pops
  the `sub_414340` acknowledge modal (`getstring(95)` "NOTE!" over
  `getstring(1131)`, general white ink, its own " Ok " button). CORRECTED
  from the body (pseudo.c 7644-7677) — the hardcoded defaults are: **set 0
  = 200/205/208/203/57/28** (arrows + Space + **Enter** — not 46/'C') and
  **set 1 = 19/34/33/32/31/30** (**R/G/F/D + S + A** — not 17/32/31/30/2/3;
  action 2 becomes 16/'Q' when the BIOS keyboard-nationality global
  `dword_4A2CA4 == 1`, an AZERTY accommodation).
- **Exit:** Enter(13)/Esc(27)/Space(32) leave the screen (pseudo.c
  8783-8792); **F1** (`0x13B`) opens the generic `.BM` help browser
  (`sub_41431C`, §4) without leaving. (An earlier revision garbled this
  bullet by conflating the F1 hook with widget id 999 — 999 is the
  defaults button above.)
- **No sound.** `sub_407B9D` contains no `sub_427961` call — the only
  audible thing on this screen is the nav blip inside `sub_414340`'s own
  key loop (the NOTE modal: blip 20 on any key, closes on Enter/Space/Esc
  or its Ok button, whose widget id IS 27).

**Persistence — CONFIRMED via an exit-time write-back, not per-edit.** The
key bindings live in the SAME `dword_4645BC[10*set+action]` array
`sub_406238` (options.ini reader, `keydef=%u,%u,%u`) populates at startup.
The **writer**, `sub_405DE3` @ 0x405DE3, re-serializes every option
(`aBombermanOptio` header line, then `levelno=` through `keydef=` in the
exact positional order the shipped `options.ini` uses) back to the file —
but `sub_405DE3` is never called directly from the UI. Its only call site is
`sub_406A2A` @ 0x406A2A (`sub_410EBF((int)sub_405DE3)`), and `sub_410EBF` is
the app's generic **exit-hook registrar** (the same routine wraps every
other module's shutdown/cleanup function, e.g. `sub_40104C`, `sub_4017BE`,
`sub_40200C`, …). So: **key rebinds (and every other Options-screen change)
are held in memory and only flushed to `options.ini` when the application
exits normally** — there is no "Save" button and no immediate write on
closing the Options/key-remap screen; a crash or force-quit loses unsaved
changes, exactly like the rest of the settings.

**Port status (2026-07-13): rebuilt 1:1** (`libs/game/keyremap_screen.*`,
`GameApp::present_keyremap_screen`): mouse-driven grid with press-on-hold /
fire-on-release and the widget-lib 8×8 cursor (SDL_HideCursor for the
screen's duration = the 431178/431360 bracket), GLUE-backdrop restore, the
89-entry key-name table + `<0x59` display rule via `dos_scancode.hpp`,
unconditional capture store after the 500 ms arm delay (raw
SDL_GetKeyboardState poll, highest index wins; the queued-Esc race artifact
is NOT reproduced), the real `sub_414340` NOTE modal (sized from its lines,
Ok button, blip-20 key loop), LUT-true inks, Enter/Space/Esc exit, F1 help,
no invented sounds. `keydef=` now round-trips in the ORIGINAL's DOS
scancode space (`dos_scancode.cpp` translates at the ini boundary; before
this pass the port wrote raw SDL_Scancode values, which BM95.EXE would
misread against a shared install), and `default_key_set()` now IS
sub_40614A's set (the port's old arrows+RCtrl / WASD+LCtrl pairing and its
Space-OR-bomb read() shim were invented and are gone).

(Provenance: `sub_407B9D` @ 0x407B9D pseudo.c 8686-8821 (re-read in full
2026-07-13); `sub_407AD9` @ 0x407AD9 pseudo.c 8636-8685; `sub_40614A` @
0x40614A pseudo.c 7644-7677; `sub_415CA4` @ 0x415CA4 pseudo.c 18131-18139;
`sub_431178`/`sub_431360` pseudo.c 34336-34463; `sub_430EDC`/`sub_430E4C` @
0x430EDC/0x430E4C pseudo.c 34192-34310; `sub_432998` @ 0x432998 pseudo.c
35400+; `sub_414340` @ 0x414340 pseudo.c 17003-17107; `off_45B914` +
`byte_45C310` read from BM95.EXE data (PE section map, 2026-07-13);
`sub_406A2A`/`sub_405DE3`/`sub_410EBF` cross-reference pseudo.c 7543-7586,
7914-7929; MESSAGES.TXT ids 27/95/1100/1105/1110/1120-1125/1130/1131/1140.)

## 3. The Options screen — `sub_4080DC` (CORRECTED: this is NOT the map editor)

**Correction to `docs/re/frontend-flow.md`'s main-menu table.** Row 3
(menu selection index 3, address `sub_4080DC` @ 0x4080DC) is labelled "**map editor**
(EDITOR.BM; the 'Ctrl+E ×6' easter egg lands here)" in that doc. Reading the
actual body of `sub_4080DC` (pseudo.c 8914-9491) shows a **19-item
interactive settings/options list** — team play, random start, node name,
conveyor speed, etc. — with no editor canvas, no `aEditorBm`/`EDITOR.BM`
string reference anywhere in the function, and no drawing/tile-placement
code. `EDITOR.BM` (which does ship in the install) is not referenced by
`sub_4080DC` at all — nor anywhere else: the string never appears in the
binary (grep `EDITOR` over the whole decompile: 0 hits); it is reachable
only as a directory-listing entry of the help browser's `*.BM` glob (§4).
The map editor itself HAS now been located (§5 below): a real, reachable
screen behind the main menu's hidden **Ctrl+E ×6** trigger — the
frontend-flow.md claim is now VERIFIED; the editor simply never had a menu
row. `sub_4080DC` **is** the OPTIONS screen; frontend-flow.md's row-3 label
should read "Options" not "map editor".

Confirmed layout — a **19-row toggle/cycle list**, `getvalue(745)=x=55`,
`getvalue(746)=y0=40`, `getvalue(747)=ystep=22`, `getvalue(748)=colour=500`
(VALUELST `; SETTINGS SCREEN: the actual listing of options items` →
`745,55,40,22,500`), cursor via `sub_413BD6(getvalue(745)-20, …)`. Random
glue backdrop (`sub_4148E5`, same helper as the pre-match screens). Every
boolean toggle renders `getstring(<global>+25)` (25=" No ", 26=" Yes ") —
confirming the `+25` idiom already seen elsewhere in the codebase.

| row (cursor index) | msg id | label (paraphrased) | backing global | on-select (Left/Right or Enter) |
|---|---|---|---|---|
| 0 | 250 | Team Play | `dword_464964` (`team_play=`) | toggle; forces `win_by_kills` off; ALSO resets `dword_46492C=-1` on every press (CORRECTED 2026-07-09, `docs/re/goldman-roulette.md` §2.1 — previously only row 6 was documented as a gold-clear trigger) |
| 1 | 251 | Random Start | `dword_464AE8` (`random_start=`) | toggle |
| 2 | 252 | Node Name | (string, `sub_40FE34`) | `sub_4074DC` — text-entry edit (net identity, not persisted to options.ini as a `keydef`-style key; separate from the 22 keys in §3's table) |
| 3 | 253 | Conveyor Speed | `dword_464930` (`conveyor_speed=`) | cycle 0..`getvalue(189)-1` (=0..2: Low/Medium/High, msg 295-297) |
| 4 | 254 | Stomped Bombs Detonate | `dword_464940` (`stomped_bombs_detonate=`) | toggle |
| 5 | 255 | Win Matches By Kill Total | `dword_46497C` (`win_by_kills=`) | toggle; forced off whenever Team Play is on |
| 6 | 256 | Gold Bomberman | `dword_4648BC` (`goldman=`) | toggle; also resets `dword_46492C=-1` (clears the pending roulette winner) — INLINE on every press, not gated on the net before/after value (`docs/re/goldman-roulette.md` §2.1) |
| 7 | 257 | Enclosement Depth | `dword_464974` (`enclosement_depth=`) | cycle 0..`getvalue(28)-1` (=0..3: None/A Little/A Lot/All the way, msg 315-318) |
| 8 | 258 | Scheme File | `byte_4648C4[100]` (`schemefilename=`) | `sub_407582` — the `*.SCH` file-picker LIST DIALOG (CORRECTED 2026-07-13: BOTH dispatch switches route here by jumping to LABEL_46, pseudo.c 9342-9343/9443-9445, so Left/Right/Enter/Space all OPEN THE PICKER; the earlier `sub_4076FE` claim was the Play Time stepper). The picker: `sub_411D17("*.SCH")` path-maps into DATA/SCHEMES, `sub_41404B` findfirst/qsort glob, each row `aSS` = `"%s: %s"` (filename + the file's `-N` name via `sub_404BE9`, default `getstring(727)` "No Scheme Name"), list dialog `sub_41485A` → `sub_42DB80` → `sub_42DBCC` at the literal (100,100), header `getstring(721)`; a selection is cut at its FIRST **':'** (`mov edx, 0x3A` @0x40767A → strchr — CORRECTED 2026-07-26 from the earlier "first '.'" reading, so the stored value KEEPS the extension: "BASIC.SCH"), strcpy'd into `byte_4648C4`, then UPPERCASED (`sub_412A3B` = strupr). Empty glob → `sub_414340` error `getstring(95)`/`getstring(720)` in `byte_49A390` dark red (164,0,0). The stored name is re-parsed into the live scheme at Play-flow entry (`sub_410F81` → `sub_4046CC` → `sub_403EEE`), and it is THAT reader which strips the extension (`strrchr('.')` then `strcat(".sch")` @0x403FE8), which is why both spellings resolve. Full geometry + the ordering/row-text confirmations: §5c. |
| 9 | 259 | Play Time | `dword_464948` (`playtime=`), read via `sub_4078FE()` | `sub_4076FE(±1)` — the CONFIRMED fixed stepper chain 60-90-120-150-180-240-300-600-1001("Infinite", `getstring(280)`), wrapping both ways; an off-list value (hand-edited ini) snaps to `getvalue(100)` (= 150 shipped) instead of stepping (pseudo.c 8489-8559) |
| 10 | 260 | Assign Keyboard Player | `dword_464968` (`assign_keyboards=`) | toggle |
| 11 | 261 | Diseases Can Be Destroyed | `dword_464990` (`diseases_destroyable=`) | toggle |
| 12 | 262 | Lost net players revert to AI | `dword_464928` (`lost_net_revert_ai=`) | toggle |
| 13 | 263 | Disable music during gameplay | `dword_4648C0` (`disable_game_music=`) | toggle |
| 14 | 264 | Modem: P/I/B/# | `dword_464970`/`dword_46482C`/`dword_4648B8`/string (`modemport=`/`modembaud=`/`modemirq=`/`modemdial=`) | `sub_40798B` — a nested modem-config sub-screen |
| 15 | 265 | Define keyboard layouts | `dword_4645BC[]` (`keydef=`) | `sub_407B9D` — the key-remap UI, §2 |
| 16 | 266 | Set Default Network Protocol | `dword_464824`? (`netprotocol=`) | `sub_407F4F` — nested protocol picker |
| 17 | 267 | Use Enhanced Memory Model | `dword_464824` (`smallmemory=`, inverted: label shows `(dword_464824==0)+25`) | `sub_407FEE` |
| 18 | 268 | Adjust Audio | — (no options.ini key) | `sub_407542` — nested volume sub-screen |

**Input model:** identical shape to every other list screen in the front
end — Up/Down (328/336) move the cursor with wrap; Left (0x14B) and Right
(0x14D) both dispatch the SAME per-row handler (toggles ignore direction;
cyclers step forward on Right, backward with wraparound on Left); Enter/
Space activate the highlighted row's handler directly; any real key fires
SFX 20; F1 (`0x13B`) opens the help browser (§4) without leaving the
screen — CONFIRMED directly against `sub_4080DC`'s F1 dispatch (pseudo.c,
where the arm is reached through a "key code <= 0x13B" comparison and calls
`sub_41431C()`), i.e. the generic browser, NOT a fixed
OPTIONS.BM open (`GameApp::present_options_screen` was calling
`present_bm_screen("OPTIONS")` until 2026-07-08 — corrected to
`present_help_browser()`, this screen's F1 site); leaving the screen calls
`sub_410494(dword_464948)` (re-applies the play-time tunable) — no explicit
Esc branch is visible in the excerpted tail, consistent with this screen
being dismissed the same way as its siblings (a `< 0x1B` / `<= 0x1B`
early-exit already covered by the generic list-screen pattern used
throughout `sub_42B9CE`'s children).

### 2026-07-09 full 1:1 audit of the port vs `sub_4080DC`'s actual body

A user report ("the OPTIONS screen likely has gaps vs the original") prompted
a full re-read of `sub_4080DC`'s decompiled body (pseudo.c 8914-9491, not
just the summary table above) against `libs/game/src/options_screen.cpp`.
Five confirmed mismatches, all now fixed in the port:

1. **All rows draw unconditionally, in the SAME ink, always** — the
   render loop (pseudo.c 9097-9290) calls `sub_41696C` once per row with no
   gating, and every single call passes the SAME ink argument,
   `byte_49D38F` (the decompile spends a separate temporary per row — an
   18-strong run of them — but every one is loaded from that identical
   global right before its draw). There is no "hide the
   unsupported rows" branch and no per-row/selected recolour anywhere in
   the function. The port previously hid rows 2/8/10/12/14/16/17
   (net/modem/legacy) entirely — this was the invented deviation, not an
   omission the original also makes. CORRECTED 2026-07-12 (chrome audit):
   the drawn set is **18 rows, getstring(250) @9098 .. getstring(267)
   @9281 — there is NO 19th "Adjust Audio" row**: getstring(268) is never
   fetched anywhere in the binary; only the DEAD dispatch `case 18`
   (sub_407542's "Audio Adjustment screen will be here..." stub) exists,
   unreachable behind the wrap modulus of 18. The same audit replaced the port's
   hardcoded ALL-CAPS row strings with the real MESSAGES.TXT compositions
   (mixed-case labels 250-267; values via getstring 25/26 with their
   padding spaces, 295-297, 315-318, 280/281 M:SS play time, the real
   node-name and four-field modem lines), matching CLAUDE.md's "no
   invented visuals" the other direction — showing what the original
   shows, not fabricating
   an interaction it doesn't have.
2. **No title/header text** — no `getstring`/`sub_41696C` call exists in the
   function before the row loop, and the caller (the row-3 dispatch,
   pseudo.c ~30910-30913) doesn't wrap the call with one either (contrast
   e.g. the key-remap sub-screen's own confirmed `getstring(1100)` header,
   §2). The port's previous "OPTIONS" text at a guessed `(55, 20)` had no
   citation. Removed. Caveat: the four chrome primitives this function calls
   with no visible body in the decompile (`sub_41043C`/`sub_415CA4`/
   `sub_415C1F`/`sub_429790`) can't be proven header-free from pseudo.c
   alone — this is documented as an open question, not a certainty, but
   nothing supports drawing a SPECIFIC guessed string/position either.
3. **The selection indicator is a real sprite, not a text recolour** —
   `sub_413BD6`'s own body (pseudo.c 16691-16721, a separate, fully-
   decompiled routine, not just a citation) resolves the ANI sequence name
   `"cursor1"` (`aCursor1`) and blits it via the standard single-frame blit
   primitive at `(getvalue(745)-20, row_y)`, self-timed by its own
   getvalue(690)/(691)-driven frame counter — completely independent of the
   row text's ink, which (per #1) never changes for the selected row. The
   port's previous yellow-recolour + `"> "` prefix was a stand-in with no
   pixel citation. Fixed: `"cursor1"` is resolved from `AssetStore::misc()`
   (MISC.ANI, already loaded for the editor's teamring markers; its
   sequence table is confirmed `cursor1, goldman, ring, safe, scan,
   teamring0, teamring1`) and drawn as a real sprite at `(x-20, row_y)`.
   Pacing PORTED faithfully 2026-07-12 (the earlier per-drawn-frame spin was
   an accepted stand-in): sub_413BD6 IDLES on step 0 and blinks — one step
   per rendered frame through the sequence — every getvalue(690) +
   rand()%getvalue(691) SECONDS (VALUELST 690 = {2,2}, the file's own "blink
   rate of the little bomber-dude cursor" legend); the shared
   `CursorIndicator` (cursor_indicator.hpp) now drives both this screen and
   the player-setup screen's instance (the setup screen anchors at x-15,
   uniquely — every other caller uses x-20).
4. **Row-navigation wraps over 18, not 19 — row 18 is permanently
   unreachable** — pseudo.c 9086 assigns the row-count literal 18 to the
   wrap-modulus variable, and both wrap paths use that same value verbatim:
   the Up-key underflow wraps the cursor row to modulus - 1 (i.e. 17), and
   the Down key increments the cursor row and resets it to 0 as soon as it
   reaches the modulus. With 19 rows (msg
   ids 250-268 inclusive) but a wrap modulus of 18, the cursor variable can
   only ever hold 0-17; the switch statements' `case 18` (Adjust Audio) is
   therefore genuinely dead code in the shipped binary — not an RE
   ambiguity, an actual off-by-one in the original. Faithfully reproduced
   (not "fixed") as `kCursorRowCount = 18` in `options_screen.hpp`: row 18
   draws every frame but never receives the cursor and never dispatches.
5. **No distinct "accept" sound** — `sub_427961(20)` is the ONLY sound this
   function ever plays, unconditionally for any real keypress (pseudo.c
   9298-9299 fires `sub_427961(20)` whenever the key code is neither -1 nor
   -2 — the two no-key sentinels — and this test is evaluated
   BEFORE the Enter/Esc/arrow dispatch). There is no `sub_427961(10)` call
   anywhere in `sub_4080DC`. The port's previous `audio.play(10)` on Enter/
   Escape/opening the key-remap screen was invented; replaced with the same
   uniform SFX 20 every other key already gets.

Two more corrections that follow directly from re-reading rows 0 and 6's
handler bodies (pseudo.c 9309-9314, 9333-9336, and the mirrored Left/Right
switch at 9410-9438): **toggling Team Play (row 0) ALSO clears the pending
Goldman winner** (`dword_46492C = -1`), the exact same side effect row 6
(Gold Bomberman) has — the port's caller (`GameApp::present_options_screen`)
previously only compared the `goldman` field before/after to decide whether
to reset `gold_player_`; it now also compares `team_play`.

Three rows (10 Assign Keyboard Player, 12 Lost net players revert to AI, 17
Use Enhanced Memory Model) were previously "shown as no-op" candidates but
turned out to be trivial to wire for real: each is a plain boolean with an
existing, already-round-tripped `assets::Options` field
(`assign_keyboards`/`lost_net_revert_ai`/`smallmemory`) and no gameplay
consumer either way, so they are now LIVE toggles like every other boolean
row rather than static placeholders — closer to the original (which also has
no consumer for these beyond the options.ini round-trip) than a hardcoded
`(N/A)` would have been. Row 12 has since GAINED a consumer: it selects the
peer-drop policy in `net::DropPolicy::revert_to_ai` (ADR-0011 Risks,
"Dropped/late peers") — on, a lost peer's seat goes to the AI; off, the drop
ends the match. Row 17's displayed label is INVERTED versus its
backing value (`getstring((dword_464824==0)+25)`, pseudo.c 9280:
`smallmemory==0` shows "YES", `==1` shows "NO") — ported as-is, not
normalized, since the original genuinely displays it this way.

(Provenance: full re-read of `sub_4080DC` pseudo.c 8914-9491; `sub_413BD6`
pseudo.c 16691-16721; row-0/row-6 handler bodies pseudo.c 9309-9314/9333-
9336/9410-9438; MISC.ANI sequence table cross-check against the install,
2026-07-08 per §5d. Port changes: `libs/game/{include/bomber/game,src}/
options_screen.{hpp,cpp}`, `libs/game/src/game_app.cpp`
(`present_options_screen`, `init`, `flush_options`).)

### 2026-07-09 (2nd pass) — Enter/Space regression fix, and a layout re-verification

A playtest of the above audit (merge `9ee04ce`) reported two problems: the
row list looked shifted over the backdrop, and pressing a row (not just
navigating) dropped straight back to the main menu. Investigation:

**The exit bug — CONFIRMED and fixed.** Re-reading `sub_4080DC`'s key tail
in full (pseudo.c 9297-9406, not just the "Input model" paragraph's summary
above, which was already correct but never actually implemented) pins the
EXACT dispatch, all of it switching on the raw key code the loop reads: the
ONLY branch that raises the loop's own exit flag is the one for key 0x1B
(Escape, pseudo.c 9374-9378). Enter (13) and Space (32) both jump to
LABEL_29 (pseudo.c 9306) — the very label the per-row switch for Right
(key 0x14D, pseudo.c 9406) dispatches to; Left (key 0x14B) runs a second,
textually-separate switch with the SAME 19 cases (pseudo.c 9408-9485) —
toggles do the same toggle regardless of direction, cyclers step backward
instead of forward, and the "opens a sub-screen" rows (2/8/14/15/16/17/18)
dispatch the SAME open action Right/Enter/Space use (e.g. row 15's
`sub_407B9D`, pseudo.c 9362/9469, is reachable via ALL FOUR keys, not
Enter-only). The port's `OptionsScreen::on_key` had `done_ = true` on
Enter/Space for every row except KeyRemap — that branch does not exist
anywhere in `sub_4080DC`; it was invented (predates this session — traceable
back to the original interactive-Options commit `9a237b4`, not something the
2026-07-09 1:1 audit introduced) and is the reported "rows kick you to the
main menu" bug. Fixed: `on_key`'s Enter/Space/Right cases now all call a
shared `activate_row(dir)` (dir=+1) — the exact same per-row switch Left
(dir=-1) already ran — and Escape is the only key that sets `done_`. The
KeyRemap row (15) now also opens via Left/Right, not Enter/Space only,
matching the original's jump to LABEL_53 from both switches.

**The layout complaint — re-verified, no numeric error found.** Re-checked
every input to the `55/40/22/500` constants against primary sources rather
than re-deriving from the decompile's noisy register tracking:
- `DATA/RES/VALUELST.RES` (a plain-text resource, not opaque binary) line
  454-456 literally reads `; SETTINGS SCREEN:` / `; the actual listing of
  options items` / `745, 55, 40, 22,500` — one entry, four columns,
  confirming `getvalue(745/746/747/748)` = `55/40/22/500` exactly as coded
  (`ValueList::columns[745] = [55,40,22,500]`, the same flattening
  `reslist.hpp`'s `column_or` documents for `getvalue(745+N) ==
  columns[745][N]`).
- The SAME "id,X,Y0,YSTEP,W" 4-column shape is already load-bearing,
  RE-confirmed, working code elsewhere in this port: `GameApp::
  present_scoreboard` reads the §1 RESULTS-screen row list via
  `values_.column_or(785, 0/1/2, ...)` = X/Y0/YSTEP, and the main menu
  cursor block (`700,332,140,38,0`, `reslist.hpp`'s own doc comment) uses
  the identical layout. Column order (X, Y0, YSTEP, W) is therefore
  cross-validated against a second, independently-working consumer, not
  just this screen's own reading of the decompile.
- `MISC.ANI`'s `cursor1` sequence (dumped via `abtool ani`) is frames
  11-14 (`POINTER1..4.TGA`), each 32x32 with hotspot `(16,31)` — i.e. the
  original's row-selection indicator is a fairly large animated pointer
  graphic anchored near its own bottom edge, not a slim `>` marker; at
  `(x-20, row_y)` with a 22px row pitch it visibly extends into the row
  above by design (confirmed from the asset itself, not a port bug).
- `GLUE1.PCX`'s header confirms 640x480 — the same as
  `SDL_SetRenderLogicalPresentation`'s logical size — so there is no
  backdrop/logical-resolution scale mismatch either.
No wrong constant, sign, or off-by-one was found in `kListX`/`kListY0`/
`kListYStep`/`kCursorX` against any of the above. The likeliest explanation
for the reported "kayık" impression is the exit bug above: a screen where
most rows kick you back to the menu on the natural "select" key reads as
broken/disoriented as a whole, independent of the actual per-row pixel
coordinates. If a specific row/coordinate is still visibly wrong after this
fix, it needs a fresh screenshot to pin — nothing in the primary sources
above supports changing the numbers further.

(Provenance: pseudo.c 9297-9406 re-read in full, not summarized; `DATA/RES/
VALUELST.RES` lines 452-456 read directly as text; `abtool ani DATA/ANI/
MISC.ANI` frame/sequence dump; `GLUE1.PCX` header bytes 4-11. Port changes:
`libs/game/{include/bomber/game,src}/options_screen.{hpp,cpp}` only — no
layout constant changed.)

### The full options.ini key list — CONFIRMED via the writer/reader positional match

`docs/re/setup-screens.md`'s note that only `conveyor_speed=` was mapped is
now fully resolved. The **reader** (`sub_406238` @ 0x406238) is a chain of
`stricmp` string-key branches whose literal comparison strings Hex-Rays
elided (calling-convention loss on the `weak`-prototyped `stricmp_`/
`strcpy_` stubs), but the **writer** (`sub_405DE3` @ 0x405DE3) emits the
identical key set via `fprintf(f, "<key>=%u\n", value)` in a fixed order
that matches the shipped `options.ini`'s key order exactly (cross-checked:
`aLevelnoD` → `aNumToWinMatchU` → `aEnclosementDep` → `aConveyorSpeedU` →
`aTeamPlayU` → `aRandomStartU` → `aStompedBombsDe` → `aWinByKillsU` →
`aGoldmanU` → `aSchemefilename` → `aPlaytimeU` → `aAssignKeyboard` →
`aDiseasesDestro` → `aLostNetRevertA` → `aDisableGameMus` → `aModemportU` →
`aModembaudU` → `aModemirqU` → `aModemdialS` → `aNetprotocolU` →
`aSmallmemoryU` → `aKeydefUUU`×20), which is positionally identical to the
reader's `stricmp` chain (same count, same order, same globals assigned).
**All 22 keys, resolved:**

| options.ini key | global | clamp (post-read, `sub_406238` tail) |
|---|---|---|
| `levelno` | `dword_464998` | `< -1 → -1`; `>= getvalue(35) → getvalue(35)-1` |
| `num_to_win_match` | `dword_464A7C` | `< 1 → 1` |
| `enclosement_depth` | `dword_464974` | `< 0 → 0`; `>= getvalue(28) → getvalue(28)-1` |
| `conveyor_speed` | `dword_464930` | `< 0 → 0`; `>= getvalue(189) → getvalue(189)-1` |
| `team_play` | `dword_464964` | normalized to 0/1; forces `win_by_kills=0` when set |
| `random_start` | `dword_464AE8` | normalized to 0/1 — CORRECTED 2026-07-08: the reader's `stricmp` chain does NOT bind these two keys in the writer's fprintf order; the Options screen's own draw/switch (msg 251 ↔ `dword_464AE8`, msg 254 ↔ `dword_464940`) and the gameplay reads settle it (`docs/re/facts.md` "Options toggles") |
| `stomped_bombs_detonate` | `dword_464940` | normalized to 0/1 — CORRECTED 2026-07-08, see the `random_start` row |
| `win_by_kills` | `dword_46497C` | normalized to 0/1 |
| `goldman` | `dword_4648BC` | normalized to 0/1 |
| `schemefilename` | (string buffer) | `strcpy_`, no numeric clamp |
| `playtime` | `dword_464948` | `< 60 → 60`; `!= 1001 && > 600 → 600` (1001 = a special "unlimited" sentinel) |
| `assign_keyboards` | `dword_464968` | normalized to 0/1 |
| `diseases_destroyable` | `dword_464990` | none observed |
| `lost_net_revert_ai` | `dword_464928` | none observed |
| `disable_game_music` | `dword_4648C0` | normalized to 0/1 |
| `modemport` | `dword_464970` | none observed |
| `modembaud` | `dword_46482C` | none observed |
| `modemirq` | `dword_4648B8` | none observed |
| `modemdial` | (string buffer) | `strcpy_`, no numeric clamp |
| `netprotocol` | `dword_464828` | `< 0 → 0`; `> 3 → 3` |
| `smallmemory` | `dword_464824` | normalized to 0/1 |
| `keydef=<set>,<action>,<scancode>` | `dword_4645BC[10*set+action]` | `set∈[0,1]`, `action∈[0,9]` else the whole line is dropped |

Every key not in this list (an unrecognized `key=value` line) hits the final
`else` branch and logs a debug warning (`aWarningUndefin` = "WARNING:
undefined equality in file '%s'", `aTextSS` = "Text: '%s' == '%s'") via
`sub_42C0C8` — a console/log print, not a user-facing dialog, confirming
unknown keys are silently ignored at the UI level.

**Team play / random start are exactly `team_play`/`random_start` above —
there is no separate "game-type" screen.** The earlier open question ("The
OPTIONS / game-type screen that toggles team play") is answered: it is
this same 19-item Options screen (`sub_4080DC`), not a distinct screen —
Team Play and Random Start are simply rows 0 and 1 of the list above.

(Provenance: `sub_4080DC` @ 0x4080DC pseudo.c 8914-9491; `sub_406238`
@ 0x406238 pseudo.c 7693-7913; `sub_405DE3` @ 0x405DE3 pseudo.c 7543-7586;
VALUELST `; SETTINGS SCREEN` block `745,55,40,22,500`, `27`/`28`/`189`;
MESSAGES.TXT ids 250-268, 295-297, 315-318, 25/26.)

## 4. The main menu's row 5 — CORRECTED: a generic HELP FILE BROWSER, not Roulette

`docs/re/frontend-flow.md`'s main-menu table lists row 5 (`sub_41431C`
@ 0x41431C) as "roulette/help (`sub_4124A4(610)`; ROULETTE.BM ships)" and
elsewhere calls the Roulette row "an inert stub in our port". Reading
`sub_41431C` and what it calls resolves this precisely:

- **`sub_41431C` itself** is two lines: `sub_4124A4(610)` (fetches the
  string **`"*.BM"`** — a filename wildcard, NOT display text; message 610
  is a glob pattern, confirmed by content) then `sub_414235()`.
- **`sub_414235`** @ 0x414235 is a **generic help-file browser**: gated on
  `getvalue(15)` ("is the online manual enabled?", default 1). If enabled,
  it calls `sub_41404B("*.BM", &count)` — a real DOS `findfirst`/
  `findnext` directory glob (confirmed: `dos_findfirst_`/`dos_findnext_`/
  `dos_findclose_` calls, `qsort_`-sorted results) over the install root,
  collecting **every** `*.BM` file present (`CREDITS.BM`, `EDITOR.BM`,
  `HIGHMEM.BM`, `INPUT.BM`, `LOWMEM.BM`, `MANUAL.BM`, `NETWORK.BM`,
  `OPTIONS.BM`, `README.BM`, `ROULETTE.BM` — all ship in the install root).
  It then shows a **selectable list dialog** (`sub_41485A`, header
  `getstring(600)` = "Available help files:") of the matched filenames; on
  selection, it opens that file through the same `.BM` text viewer
  (`sub_41302D`) documented in frontend-flow.md, and loops back to the list
  after the viewer closes (`Enter`/`Esc` dismiss, per that doc's existing
  scroll/dismiss facts) until the player cancels the list itself. If no
  `.BM` files are found or the manual is disabled, an error dialog fires
  (`getstring(4)`/`getstring(5)`, `sub_414340`).
- **`ROULETTE.BM`'s own content confirms the topic split**: it is titled
  "Gold Bomberman Roulette Wheel Screen Help" — i.e. it is *documentation
  about* the Goldman roulette feature, one help topic among ~10, not the
  roulette screen itself.

**The actual Goldman Roulette Wheel mini-game is a SEPARATE, unrelated
routine — `sub_4034BC` @ 0x4034BC — and it is NOT reachable from the main
menu at all.** It is invoked automatically at the head of the Play flow —
CORRECTED (2026-07-08): the call at pseudo.c 15043-15057 sits at the top of
**`sub_410F81`** (the player-setup screen, def 14924), not in `sub_410B6E`
as first attributed — whenever **`goldman=1`** (`dword_4648BC`, the
Options-screen "Gold Bomberman" toggle, §3), the game is local
(`!sub_40C06A()`), it is not an attract/demo run (`!dword_464938`), and a
pending gold player exists (`dword_46492C != -1`, checked inside
`sub_4034BC`). It draws a genuine animated Lissajous-curve spinning wheel
and awards the previous winner one bonus powerup — the full mechanics
(5-draw rand sequence, deceleration model, the 6-slot prize table
`{0,1,3,8,4,13}`, the +86-inventory award at round init, the gold twinkle,
assets/sounds/message ids) are now pinned in **`docs/re/goldman-roulette.md`**,
which supersedes this paragraph's summary.

**Corrected mapping:**
- Main menu row 5 = the **Help/Manual browser** (`sub_41431C`→
  `sub_414235`), listing all `.BM` files — best reproduced as a simple
  file-picker over the same `.BM` viewer the port already has, not an
  inert stub.
- **There is no main-menu row for the Goldman Roulette Wheel** — it is
  conditional, automatic presentation at Play-flow entry, gated purely by
  the `goldman=` option and a pending gold player. It belongs wired in
  front of the player-setup screen (the top of our `present_setup`
  equivalent), not the menu — see `docs/re/goldman-roulette.md`.

**Loop-back detail, from `sub_414235`'s body (pseudo.c 16933-16983,
transcribed in full):** the `sub_41404B("*.BM", &count)` glob runs exactly
ONCE per browser open — the browser body is a do-while loop that calls
`sub_41485A(...)` (the `.BM` viewer) each pass and keeps looping for as long
as the list dialog's chosen-index return is not -1, re-showing the SAME list
dialog (header `getstring(600)`, ink `byte_49D38F`, at `(100, 100)`) on every
return from the `.BM` viewer and indexing the SAME filename array from that
one glob at the dialog's chosen index, rather than re-globbing;
the directory is only re-read on the browser's NEXT top-level open. The list
finally frees via `sub_414173` when the dialog itself returns -1 (its own
Esc/cancel). The two gated error paths (`getvalue(15)==0` "manual disabled"
and an empty glob) both draw through `sub_414340` in ink `byte_49D0DA`
(a distinct global from the list's own white `byte_49D38F` — DECODED in §1's
LUT table: RGB (252, 80, 80), the SAME LUT element as `sub_4141F8`'s team-1
ink, confirmed identical, not merely similar) with `getstring(5)`/`getstring(4)`
(disabled) or `getstring(4)`/`getstring(95)` (empty) — CORRECTION: reading
the exact call order, the "disabled" branch is `getstring(5)` then
`getstring(95)`, and the "empty glob" branch (the arm taken when the glob
returned a null/empty filename array) is
`getstring(4)` then `getstring(95)` — i.e. only the FIRST string differs
between the two error cases (5 vs 4), both share the `95` second line and
the `414340` two-line dialog shape.

**Port status (2026-07-08):** `HelpBrowser` (`libs/game/src/bmscreen.cpp`)
now wires both facts: `enter(bool manual_enabled)` gates on the caller's
`getvalue(15)` reading BEFORE the glob (both `GameApp::present_help_browser`
and `present_help_browser_modal` pass `values_.at_or(15, 1) != 0`), and its
`draw()` renders the disabled/empty error pair in the decoded `byte_49D0DA`
RGB (252, 80, 80) rather than a placeholder grey. The Options screen's F1
(`sub_4080DC`) and the editor chooser's F1 (`sub_403184`) were also corrected
to open this SAME generic browser instead of a fixed OPTIONS.BM/EDITOR.BM
cut — see §3's and §5's own F1 rows below, both of which already documented
`sub_41431C` as the target; the port had drifted from that fact until now.

(Provenance: `sub_41431C` @ 0x41431C pseudo.c 16996-17001; `sub_414235`
@ 0x414235 pseudo.c 16933-16995; `sub_41404B` @ 0x41404B pseudo.c
16867-16894; `sub_4034BC` @ 0x4034BC pseudo.c 5921-6132 (roulette-wheel
math helpers `sub_403382`/others at 5876-5919); its caller pseudo.c
15043-15057; VALUELST `15,1`, `; Goldman Roulette Wheel` block 805/
1000-1010; MESSAGES.TXT ids 4/5/95/600/610; `ROULETTE.BM` header line
confirms the help-topic framing.)

## 5. The map/scheme editor — FOUND: `sub_4028D2`, behind Ctrl+E ×6 (CONFIRMED)

The editor §3 left "not-yet-located" is real, complete, and reachable. It
has **no menu row**; its sole entry point is a hidden trigger inside
`sub_42B9CE`'s input loop (pseudo.c 30876-30883): raw key code **5** — the
ASCII control code for **Ctrl+E** (the same stream encodes Ctrl+Q as 17,
used by the menu's Escape/quit alias) — tracked by a same-key repeat
counter that any other key resets; `++counter > 5` fires on the **6th
consecutive press**, plays accept SFX 10 and calls `sub_40330E` (its only
call site in the binary). So the old "Ctrl+E ×6" folklore is exactly right,
and frontend-flow.md's "key 5 ×5 campaign easter egg" label for this
trigger was wrong on both counts (it is 6 presses, and it opens the editor,
not a campaign).

Call chain, all confirmed by body reads:

- **`sub_40330E` @ 0x40330E** (pseudo.c 5847-5861) — wrapper: saves and
  zeroes the team-play flag `dword_464964` around the editor (bracketed by
  the no-op stubs `sub_4021DC`/`sub_4021F1`), calls `sub_403184`.
- **`sub_403184` @ 0x403184** (pseudo.c 5745-5844) — the editor's own
  3-item menu on a random `GLUE<n>` backdrop (`sub_4148E5`, the same helper
  as the pre-match screens): title `getstring(730)` at getvalue(810/811/813)
  (VALUELST `; Editor - mainmenu header` → `810,50,100,0,400`), rows
  `getstring(731..733)` at getvalue(815-818) (`; Editor - mainmenu items` →
  `815,80,140,20,400`). Keys: **'1' (49)** → `sub_4028D2(0)` (edit an
  existing scheme — first runs the `*.SCH` file picker `sub_407582`
  @ 0x407582, a `findfirst` glob like the help browser's, then the `.SCH`
  parser `sub_403EEE` @ 0x403EEE); **'2' (50)** → `sub_4028D2(1)` (new
  scheme); **Esc/'Q'(81)/'q'(113)** exit; **315 (F1)** → help browser
  `sub_41431C` — i.e. the SAME generic browser row 5/Options-F1 open, NOT a
  fixed EDITOR.BM cut (`GameApp::present_editor`'s chooser-level F1 was
  calling `present_bm_screen("EDITOR")` until 2026-07-08, contradicting this
  very fact; corrected to `present_help_browser()`). SFX 20 blip on any key.
- **`sub_4028D2` @ 0x4028D2** (pseudo.c 5429-5716) — **the editor
  screen**. Draws the tile grid with the `tile %d blank` / `tile %d solid`
  / `tile %d brick` ANI sequences (§5d) and the 10 player-start markers
  (slot number in the slot's colour `sub_41672F`/`sub_416867`, plus a
  `teamring%u` ANI sprite showing each start's team flag — `aTeamringU`,
  pseudo.c 1325). Interactions (verified against the body, incl. the key
  `switch`):
  - **left mouse** — paint the hovered cell with the current brush
    (`sub_4048EB(gx, gy, brush)` via the pixel→cell mappers
    `sub_42665C`/`sub_4266A3`). PINNED: exactly ONE cell per click — the
    brush has only a TYPE, a single variable holding 0, 1 or 2; **no
    multi-cell brush exists**,
    closing the earlier "brush sizes 1/2/3, even-size anchor?" question by
    removal. `sub_4048EB` writes the cell chars directly: `'#'` (35)
    solid, `':'` (58) brick, `'.'` (46) blank — the `-R` row alphabet;
  - **right mouse** — MOVE the currently-selected player-start marker to
    the hovered cell (writes `dword_46481C[12*slot]`/`+4`, clamped);
  - **'1'/'2'/'3'** select the brush (blank/solid/brick); **Tab/Enter/
    Space** cycle it (the brush type is incremented and wraps back to 0 once
    it passes 2);
  - **Ctrl+F (6)** — flood-fill the whole grid with the brush, gated by a
    **`getstring(760)`/`getstring(97)` yes/no confirm** first (then a
    plain j/k double loop over `sub_4048EB` — a full-board fill);
  - **Ctrl+B (2)** — reset the board to the new-scheme defaults
    (`sub_4049C0`, §5a), with a `getstring(740)`/97 confirm when dirty;
  - **'0' (48)** — toggles the tile-art set number `dword_45B7B8` between
    0 and -1 (the global is incremented, and forced back to -1 whenever that
    increment leaves it greater than 0). A dead-end feature: no TILES ANI
    ships a `tile -1 *` sequence, so the editor's art is effectively
    always tileset 0;
  - **'+'/'='/'-'/'_'** cycle the selected start slot; **'T'/'t'** toggle
    that start's team flag;
  - **'D'/'d'** — brick-density prompt (text entry `sub_42E938`, atoi,
    clamped 0-100 into `dword_4647A0` — the scheme `-B` field; the ONLY
    clamped numeric prompt in the editor);
  - **'N'/'n'** — scheme-name prompt (the `-N` field, `getstring(728)`);
  - **'P'/'p'** — `sub_402595` @ 0x402595 (pseudo.c 5275-5413), the
    13-row powerup-rules sub-editor (§5b);
  - **Esc/'Q'/'q'** — exit with a save-changes confirm (`getstring(735)`),
    then a filename prompt (`getstring(736)`), writing through
    **`sub_403C16` @ 0x403C16** (pseudo.c 6182-6250) — a real `.SCH`
    serializer emitting the exact shipped format: header comment lines,
    `-V,%u` version (every shipped scheme is **`-V,2`**), `-N,%s` name,
    `-B,%u` density, `-R,%2u,%s` rows, 10 `-S,%u,%d,%d,%d` starts, and 13
    `-P,%2u,%2d,%d,%2d,%2d,%s` powerup rows whose trailing comment text is
    `getstring(800+i)` — the same 800-block strings the roulette result
    screen uses (`docs/re/goldman-roulette.md` §7). (Writer format-string
    literals pseudo.c 1335-1349.)
  - **315 (F1)** — help browser.

### §5a. New-scheme defaults — `sub_4049C0` @ 0x4049C0 (PINNED)

`sub_4028D2(1)` ("new scheme") and the Ctrl+B reset both run `sub_4049C0`
(pseudo.c 6681-6737), then the caller sets the default name from
**`getstring(729)`**. The "blank" board is NOT blank:

- density `dword_4647A0` = **90**;
- rows: even rows are memcpy'd from **`":::::::::::::::"`** (all brick),
  odd rows from **`":#:#:#:#:#:#:#:"`** (brick/solid alternating) — the
  classic pillar field, fully bricked (both 16-byte templates are static
  data, pseudo.c 1350-1351);
- start `j` (0..9): x = **`getvalue(600 + 2j)`**, y = **`getvalue(601 +
  2j)`** (VALUELST ids 600..619), wrapped into the board with repeated
  `+= / -= width/height` loops; team flag = **`j & 1`** (alternating);
- all 13 powerup-rule fields zeroed.

### §5b. Powerup sub-editor — `sub_402595` + `sub_4023A2` (PINNED)

Layout: header `getstring(754)` at (300, 30) in the `byte_497F8F` cyan ink
(§1's ink pin); 13 rows at y = 24·i + 60, each a mouse button (id 5000+i,
`sub_432298`) labelled `getstring(755)`; the powerup NAME column
(`getstring(756)` with `getstring(850+i)`) at x≈90 in the `byte_49D37A`
yellow ink; the born-with column (`getstring(757)` with `dword_4647A4[i]`)
at x≈210 and the override column (`getstring(759)` with `dword_4646C4[i]`
when `dword_464764[i]` is set, else `getstring(758)`) at x≈450, both in
the general white ink; exit hint `getstring(737)` at the bottom. Exit
keys: Enter(13)/Esc(27)/Space(32)/'Q'/'q'; F1 (315) = help browser. Rows
are activated by MOUSE ONLY (hit ids 5000..5012 → `sub_4023A2(row)`).

**`sub_4023A2` @ 0x4023A2** is the rules/override entry widget — a CHAIN
of four modal prompts in fixed order, each independently cancellable (a
cancel keeps that ONE field and the chain still continues):

1. **born-with count** — `getstring(762)`, generic text-entry dialog
   `sub_42E938` (maxlen 20, seeded `"%u"` with the current value, Done/
   Cancel buttons, Enter commits / Esc returns -1), committed via atoi
   (`sub_4516C1`). **NO clamp** (only the `.SCH` reader clamps `< 0 → 0`
   at load);
2. **forbidden** — `getstring(764)`, yes/no dialog `sub_42EDE0` →
   `dword_4647E0[i]`;
3. **has-override** — `getstring(766)`, yes/no dialog → `dword_464764[i]`;
4. **override value** — `getstring(768)`, text entry seeded `"%d"`, atoi,
   **NO clamp** — asked only when has-override is set; when it is NOT set
   the value is unconditionally forced to **0** (the else branch), even if
   prompt 3 was cancelled with it already clear.

Field→`-P` mapping (cross-confirmed reader `sub_403EEE` ↔ writer format):
`-P,<id>,<bornwith = dword_4647A4>,<hasoverride = dword_464764>,
<overridevalue = dword_4646C4>,<forbidden = dword_4647E0>,<name comment>`.
The reader clamps bornwith `< 0 → 0` and normalises both booleans `!= 0`.

### §5c. The *.SCH file picker — `sub_407582` @ 0x407582 (PINNED)

Globs `"*.SCH"` via `sub_41404B` (the help browser's findfirst/qsort
helper, §4), pre-reads each file's embedded `-N` name (`sub_404BE9`), and
lists the results through the generic list dialog at **(100, 100)** with
header **`getstring(721)`** ("Available Scheme Files:") and the general
white ink (`byte_49D38F | 0x10000`). An empty glob shows the
`getstring(720)`/`getstring(95)` error dialog instead.

#### 2026-07-26 re-read — three corrections and the full dialog geometry

This section previously said the rows were `"%s %s"`, that selection cut at
the first space, and that the dialog showed 13 rows; §3's row-8 entry said
the cut was at the first `'.'`. All four claims were wrong. A byte-level
re-read of `sub_407582` and the whole dialog chain fixes them and supplies
the geometry the port had been drawing without.

**The call chain (this resolves the `sub_41485A`-vs-`sub_42DBCC` clash).**
Neither prior citation was complete — there are FOUR routines, not two:

| addr | routine | what it actually is |
|---|---|---|
| `0x407582` | `sub_407582` | the picker: glob, reformat, call, write back |
| `0x407657` | `sub_41485A` | a mouse show/hide bracket (`sub_431178`/`sub_431360`) around ONE call, and nothing else. `ret 0xc` |
| `0x41488E` | `sub_42DB80` | an argument trampoline: re-pushes its 5th through 7th arguments and appends an 8th argument, the INITIAL SELECTION INDEX, hardcoded `0`. `ret 0xc` |
| `0x42DB94` | `sub_42DBCC` | the widget that measures, builds and draws. `ret 0x10` |

So §5c's "`sub_41485A` → `sub_42DBCC`" skipped `sub_42DB80`, and
`editor_screen.cpp`'s "the generic list dialog (`sub_42DBCC`) is invoked at
(100, 100)" named the right widget but not the routine `sub_407582` calls.

**The window is NOT centred.** `sub_42DBCC` opens it with
`sub_43C734(x, y, w, h, colormode=256, flags=0x14)` @ `0x42DC81`, passing
its own 5th and 6th arguments straight through — and `sub_407582` @
`0x407641` pushes the
literal pair `(100, 100)`. `sub_43C734` really is **6-arg** (`ret 8` plus
the four Watcom register args), and its 1st and 2nd arguments reach
`sub_43D398` @
`0x43C8C2`, which bounds-checks `[win+0x18]` (the width it just stored)
`+ edx` against the right clip edge and `[win+0x1c]` (height) `+ ebx`
against the bottom — so **argument 1 = x, argument 2 = y**, decisively. This independently
CONFIRMS the rescued `worktree-dialog-chrome-todo-re` branch (commit
`0c0b00d`) and retires the old "X is never an explicit parameter in this
family" note. Cross-check: the boot LOADING dialog @ `0x412E7D` passes
`eax = 0x96` (150) with `ebx = 0x168` (360) width — centring would be 140,
so 150 is a hardcoded literal, exactly as that branch reported.

**TEN visible rows, not thirteen.** @ `0x42DC44` the widget seeds two
SEPARATE counters: `[esp+0xA8] = 10` (the rows it draws — the value used at
every later site) and `ebp = 13` (the font-height multiplier for the window
it requests). Both decrement together when the allocation fails
(`cmp ebp, 8; jg` @ `0x42DCA2` → heights 13..9, rows 10..6), which the port
never hits. The invariant is `multiplier == rows + 3`. Thirteen rows is not
merely wrong but impossible: the item area alone needs `13*fh` starting at
`y = fh+16`, i.e. `14*fh + 16 > 13*fh + 22` for any `fh > 6`.

**Geometry** (window-relative; the widget draws into the window's own
buffer at `bitmap + y*pitch + x`). Full offset citations live in
`libs/game/include/bomber/game/list_dialog_geometry.hpp`:

- `item_w` = widest ITEM (`sub_42FEF0` @ `0x42DC16`, a plain max over
  `textwidth`); `win_w = max(item_w + 16, textwidth(title)) + 20`, and the
  widget bumps `item_w` so `item_w == win_w - 36` always.
- `win_h = (rows + 3) * fontheight + 22`.
- 1-px BLACK outer rect `(0,0)..(w-1,h-1)` (`sub_442384` @ `0x42DCF9`),
  then a RAISED bevel at inset 1 (`sub_44240C` @ `0x42DD39`).
- Title strip: base-coat fill at `(5,5)` sized `(w-11) x (fh+3)`
  (`sub_442A5C` @ `0x42DD9E`), a SUNKEN bevel `(5,5)..(w-6, fh+8)`
  @ `0x42DE0E`, and the title centred at `(w/2 - tw/2, 8)` @ `0x42DDCB` in
  `dword_45C478` grey (168,168,164).
- Items at `x = 8`, `y0 = fh + 16`, pitch `fh` (@ `0x42DE4A`); base-coat
  fill at `(5, fh+14)` sized `(item_w+6) x (rows*fh+2)` @ `0x42DEA2`, inside
  a SUNKEN frame `(5, fh+13)..(item_w+10, item_bottom)` @ `0x42DFD2`.
- **Scrollbar, drawn UNCONDITIONALLY** — there is no branch around it, so a
  list that fits still shows a full-height bar. Arrow buttons at
  `x = w-25`, `y = fh+13` and `y = item_bottom - fh - 5`, hotkeys `0x148`/
  `0x150` (the DOS extended up/down codes); labels are the HARDCODED
  literals `"\x18"`/`"\x19"` at `0x45AAAC`/`0x45AAB0`. Track: base-coat fill
  15 px wide at `(w-21, 2*fh+23)` @ `0x42E0F8` in a SUNKEN frame
  `(w-22, 2*fh+22)..(w-6, item_bottom-fh-9)` @ `0x42E189`. The thumb is a
  **FIXED 15x15** raised bevel @ `0x42E1D4`, not proportional.
- `"Done"` button at `(w/2 - 32, win_h - fh - 14)` @ `0x42E072` — the
  hardcoded literal at `0x45AAB4` (NOT a `getstring`), widget id 27, the
  same x rule and id as `sub_414340`'s `" Ok "`.

**No WINZ 9-patch.** `sub_42DBCC`'s body contains no `sub_41726B`/
`sub_416B43` call anywhere, so this dialog keeps `sub_43C734`'s flat
colormode-256 base coat, `dword_45C46C` → **(88, 84, 80) grey**. The "grey
popup" recollection that prompted this pass is correct; the blue WINZ skin
belongs to the boot/confirm family, not here.

**Selection is a LIGHTEN, not an invert.** `sub_442C28` @ `0x42DF80` runs
over exactly `(8, fh+16 + sel*fh)` sized `item_w x fh`, remapping each pixel
through `byte_495390[p*256 + 0x93]` — a runtime-built 256x256 blend LUT
(built @ `0x42C726`-`0x42C777` as `c' = c + (31-c)*k/128` per 5-bit
channel, i.e. a lerp toward white). Crucially `sub_432298` calls the SAME
`sub_442C28` at `0x4323DB` on the same base coat to make a button face, so
**the selected row's background is the button-face colour by construction**
— no new constant needs deriving. The row's already-drawn text is washed
too, but the general white ink (240,248,252) = 5-bit (30,31,31) is at the
ramp ceiling and does not move, so white text stays white over the band.

**Ordering — CONFIRMED, and it is not what the row text suggests.**
`sub_41404B` uppercases EVERY globbed name first (`sub_412A3B`/strupr over
the whole array @ `0x414146`) and only THEN qsorts @ `0x41415D` with the
comparator at `0x41400F`, which is a plain `sub_451F10`/**strcmp** on the
two `char*` (Watcom's dword-at-a-time strcmp — the `0xFEFEFEFF`/`0x80808080`
zero-byte trick). Because the `": <name>"` suffix is appended AFTERWARDS by
`sub_407582`'s own reformat loop, **the sort key is the uppercased bare
filename and the scheme name never participates.**

**Row text — CONFIRMED `"%s: %s"`.** The format string at `0x458B11` is
literally `"%s: %s"`, sprintf'd @ `0x4075EE` from the glob filename (WITH
its extension, and already uppercased by `sub_41404B`) and
`sub_404BE9(filename)`. `sub_404BE9` strcpy's `getstring(727)`
("No Scheme Name") into its static return buffer BEFORE it opens the file,
so that default also covers an unreadable or `-N`-less file; on success it
takes everything after the first `,` on the first `-N` line, strips the
trailing newline, and caps the line at 99 chars. So rows really do read
`"BASIC.SCH: Basic Bomberman"`.

**Selection write-back cuts at `':'`, NOT at `'.'`.** @ `0x40767A` the
instruction is `mov edx, 0x3A` (raw bytes `ba 3a 00 00 00`) feeding
`sub_45167A`/strchr — a COLON. Cutting `"BASIC.SCH: Basic Bomberman"` at its
first `':'` therefore recovers the filename **WITH its extension**, and that
is what is strcpy'd into `byte_4648C4` @ `0x4076AE` and uppercased
(`sub_412A3B`) @ `0x4076B8`. It still loads because the extension strip
lives in the READER: `sub_403EEE` @ `0x403FE8` does `strrchr(name, '.')`,
truncates there, then `strcat`s `".sch"` — so `"BASIC.SCH"` and `"BASIC"`
resolve to the same file. (The shipped `options.ini` reads
`schemefilename=BASIC` because that default was never round-tripped through
the picker.) The visible consequence is Options row 8, which prints the
buffer verbatim through `getstring(258)` `"Scheme File: %s"` — after a pick
the original shows **"Scheme File: BASIC.SCH"**, not "...: BASIC".

**Nav keys** (jump table at `0x42DBA0`, dispatched @ `0x42E46E` for codes
`0x147..0x151`): Home `0x147`, Up `0x148`, PgUp `0x149`, End `0x14F`,
Down `0x150`, PgDn `0x151`; Left/Right and the gaps fall through to the
default arm. Enter (13) or a row widget (`0x400+i`) accepts; `0x1B`/the
"Done" button cancels. The dialog returns the absolute index, or -1. NOTE
the 4th argument (in `ecx`, which `sub_407582` passes as 0) is a
**callback**: when non-null, Enter invokes it with the item list and the
selected index, and the list stays open instead of returning — an unused capability at this call site.

**Port status (2026-07-26):** `list_dialog_geometry.hpp` holds the geometry
above as SDL-free integer math, doctest-pinned in
`tests/game/test_list_dialog.cpp` (ctest `list_dialog`);
`dialog_chrome.cpp`'s `draw_list_dialog` draws through it; and
`SchemeFilePicker::draw` now renders the picker THROUGH that chrome instead
of the bare `font_->draw(header, 100, 100)` + plain rows it had before. The
`':'` cut, the strupr-then-strcmp ordering and the 10-row window are ported.
NOT established from the binary, and therefore not reproduced: the exact
truecolour value of the `sub_442C28` wash is taken as "whatever the button
face already is" rather than re-derived (the blend LUT lives in `.bss` and
is built at runtime, so it cannot be read statically), and the mouse-driven
parts of the widget — dragging the fixed thumb, the `0x200+i` per-row click
targets, the `sub_4321F0` slider — are still keyboard-only in the port.

### §5d. Editor canvas art (PINNED) — and the port's wiring

- **Tiles:** `sub_4022A1` (grid draw, pseudo.c 5148-5180) draws EVERY cell
  with the `"tile %d blank"` frame first, then overdraws a non-blank
  cell's `"tile %d solid"`/`"tile %d brick"` frame — sequence names
  formatted with `dword_45B7B8` (`sub_402206`, pseudo.c 5120-5145), which
  is **always 0** in practice (the '0' key's -1 state is dead). I.e. the
  editor draws **TILES0.ANI**'s own `tile 0 blank/solid/brick` sequences —
  the same art the match field uses. The brush preview draws the brush's
  tile frame AT the mouse cursor each frame: `sub_402206` is called with the
  current brush type and the resulting frame is blitted through `sub_415920`
  at the mouse x/y.
- **Cell geometry:** the cell→pixel mappers are the match field's own
  `sub_426524`/`sub_42655F` (`cellW·x + cellW/2 + originX`, `cellH·y +
  cellH − 1 + originY` — bottom-centre anchors over the standard field
  origin/cell tunables), so the canvas IS the in-game field layout (our
  port: origin (20, 68), cell 40×36, renderer.hpp's constants).
- **Start markers:** slot number `"%u"` (i+1) at (cell_centre_x − 20,
  cell_bottom − 36) in `sub_41672F(i)`'s ink — the editor wrapper
  `sub_40330E` zeroes team play around the whole editor, so this is
  always the slot-colour branch (the `.RMP` tail through the RGB555 LUT,
  `docs/re/player-colour.md`) — then the **`teamring%u`** sequence (%u =
  the start's team flag 0/1) at (cell_centre_x − 20, cell_bottom) via the
  named-sequence draw `sub_41735A`.
- **`teamring%u`'s ANI home: `DATA/ANI/MISC.ANI`** — its sequence table is
  `cursor1`, `goldman`, `ring`, `safe`, `scan`, `teamring0`, `teamring1`
  (checked against the install's file, 2026-07-08). This also resolves the
  Goldman wheel's previously-unattributed `ring` pointer sequence
  (`docs/re/goldman-roulette.md` §3/§7) to MISC.ANI.

**Port status:** the above is wired in `libs/game/{editor_grid,
editor_screen}.{hpp,cpp}` + `AssetStore::misc()`: single-cell painting,
§5a's new-scheme board (VALUELST 600..619 starts passed through
`GameApp`), the -V,2 writer version, §5b's 4-prompt chain (keyboard
'E'/Right substitutes for the original's mouse-only row buttons — a
documented deviation), §5c's 10-row picker at (100,100) with `-N` name
suffixes, the Ctrl+F fill confirm, and the real TILES0/MISC.ANI canvas
art (stage 0 loaded at editor entry; flat swatches remain only as the
missing-asset fallback).

2026-07-09: the four remaining items closed out —

- **Ctrl+B board reset** (case 2, pseudo.c 5584-5599): PINNED and ported.
  While the board is untouched it resets immediately with NO confirm;
  once touched, the getstring(740)/97 confirm gates it — `EditorScreen`'s
  own `dirty_` flag mirrors the original's own modified counter exactly,
  including the
  easy-to-miss detail that Ctrl+B marks the board dirty EVEN WHEN THE
  CONFIRM IS CANCELLED (the counter is incremented unconditionally after the
  if/else, pseudo.c 5598). The same modified-flag gate also closes a second,
  previously-unported fact: the Esc/'Q' exit case runs its WHOLE
  save-confirm+write body only when that flag is non-zero (pseudo.c
  5621-5643) — an
  untouched board now exits silently with no prompt and no write, which
  the port's Esc/Q handler did not do before this pass (it always asked).
- **'0' dead tileset toggle** (case 48, pseudo.c 5654-5657): PINNED and
  ported as `editor_grid.hpp`'s free function `toggle_editor_tileset`
  (doctest-covered, SDL-free) plus `EditorScreen::tileset_` +
  `refresh_tile_sequences()`, which re-resolves the "tile %d
  blank/solid/brick" Anims from the live tileset id instead of the
  previously-hardcoded "0".
- **Brush-preview-at-cursor** (pseudo.c 5518-5524): PINNED — every frame,
  AFTER the grid draw and BEFORE the start-marker/status-text draws, the
  original re-draws the CURRENT brush's own tile frame at the live mouse
  position via the SAME `sub_415920` primitive the grid cells use. Ported
  via `EditorScreen::on_mouse_move` (fed from `game_app.cpp`'s
  `SDL_EVENT_MOUSE_MOTION`, logical-coordinate-mapped like the existing
  mouse-down handler) and a `draw_step` call in `draw()` at the raw
  cursor pixel instead of a cell centre.
- **`sub_41456C`/`sub_42E938` exact dialog chrome**: PINNED (already
  documented above, `docs/re/frontend-flow.md` "The sub_43C734
  dialog-chrome primitive" / "sub_432298 — the button widget", pinned
  2026-07-09 for the boot LOADING dialog + main-menu quit confirm).
  `libs/game/{include/bomber/game,src}/dialog_chrome.{hpp,cpp}` extracts
  those primitives out of `game_app.cpp` into a shared, reusable form —
  `draw_confirm_dialog` (the two-line `sub_41456C` family: the editor's
  Ctrl+B/Ctrl+F/Esc-save confirms) and `draw_text_entry_dialog` (the
  `sub_42E938` family: the density/name prompts, y=180 CONFIRMED literal)
  — so every editor dialog now draws through the SAME pinned grey-fill/
  bevel primitive as the rest of the front-end, replacing the earlier
  ad hoc solid-black boxes. NOT pinned (documented, not guessed): which of
  `sub_41456C`'s two packed prompt lines draws on top at the pixel level
  (`dialog_chrome.hpp`'s own TODO(RE) — the same class of register-spill
  ambiguity as `sub_43C734`'s X-placement) and `sub_42EDE0`'s/
  `sub_42E938`'s exact width baseline (both reuse the confirm family's
  content-driven-width-with-minimum-clamp shape rather than the precise,
  decompiler-ambiguous register-spilled term — `dialog_chrome.hpp`'s own
  comments on `draw_compact_confirm_dialog`/`draw_text_entry_dialog`); the
  powerup sub-editor's `sub_42EDE0` Forbidden/HasOverride prompts still
  draw their own minimal reproduction (`PowerupRulesScreen::draw`) rather
  than routing through `draw_compact_confirm_dialog` — a follow-up, not a
  blocker for this item.

MESSAGES ids: 720-721 (picker error/header), 728-729 (name prompt /
default new-scheme name), 730-733 (editor menu), 735-740 (save-confirm,
filename prompt, exit hint, "start %u" status, density prompt, reset
confirm), 742-743 (scheme-file/density status labels), 754-759 + 762-768
(powerup sub-editor), 97/95 (yes/no dialog chrome), 800-812 (powerup
comments) + 850-862 (powerup names). VALUELST: 600-619 (default starts),
810/815 (editor menu layout).

(Provenance: trigger pseudo.c 30876-30883; `sub_40330E` 5847-5861;
`sub_403184` 5745-5844; `sub_4028D2` 5429-5716; `sub_402206` 5120-5145;
`sub_4022A1` 5148-5180; `sub_402595` 5275-5413; `sub_4023A2` in the same
range; `sub_4048EB` 6629-6660; `sub_4049C0` 6681-6737; `sub_403C16`
6182-6250; `sub_403EEE` 6252+; `sub_407582` 8422-8480; `sub_42E938`
32751-32840; `sub_42DBCC` 32204-32340; row templates + writer format
strings pseudo.c 1335-1351; `sub_426524`/`sub_42655F`/`sub_42665C`/
`sub_4266A3` 27031-27095; MISC.ANI/TILES*.ANI sequence tables + shipped
`-V,2` versions from the install, 2026-07-08.)

## Determinism / golden — no impact

Everything in this doc is presentation (RESULTS render pass, Options menu,
key-remap UI, help browser, roulette-wheel animation) or **process-lifetime
configuration state** (`options.ini`, in-memory only until exit). None of it
touches `libs/sim`'s hashed `State`. The one gameplay-relevant fact is that
`team_play`/`random_start`/`enclosement_depth`/`conveyor_speed`/
`stomped_bombs_detonate`/`diseases_destroyable`/`num_to_win_match`/
`win_by_kills`/`goldman` are all **match-setup inputs** that already flow
into `MatchConfig`/`Tuning` at match-start (not per-tick), so wiring the
Options screen to write them is config plumbing, not a sim change — no
golden-hash recapture required unless a specific tunable's *default value*
changes (in which case cite this doc + the VALUELST/options.ini evidence in
the same commit, per `CLAUDE.md`'s determinism contract).
