# RESULTS tally, key-remap, Options screen, and the Roulette row — RE

Reverse-engineered from `BM95.EXE` (pseudo.c) and the shipped
`DATA/RES/VALUELST.RES` / `MESSAGES.TXT` (structure and ids only — text and
values stay in the install, never committed; see `docs/re/setup-screens.md`
and `docs/re/frontend-flow.md` for the companion facts this doc extends).

## 1. RESULTS cumulative-tally screen — `sub_42A3F6` middle tier (CONFIRMED)

`docs/re/frontend-flow.md` flags the RESULTS tier of the Play handler
(`sub_42A3F6`, pseudo.c ~29886-30106) as deferred because the spine plays one
round and returns to the menu. The screen logic itself is fully present in the
decompile — only the multi-round match/scoreboard *state* is the missing
piece on our side. Confirmed layout:

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
- **Match-clinch check (v73):** the loop tracks the first player/team whose
  `sub_421AC8` (win count) reaches `dword_464A7C` (the configured "number of
  wins to clinch the match", `num_to_win_match=` in options.ini). In team
  mode with `win_by_kills` (`dword_46497C`) set, the clinch instead compares
  the **highest round-kill total** (`sub_421B0F`) against `dword_464A7C`,
  breaking ties by requiring a single unique leader (`v78 == 1`).
- **Outcome line** — drawn after the per-player list, at **x =
  getvalue(800), y = getvalue(801), colour = getvalue(803)** (VALUELST
  `; player %u wins the match...` → `800,150,94,0,400`):
  - **No clinch yet (v73 == -1):** `getstring(dword_46497C + 120)` formatted
    with `dword_464A7C` (the wins-needed number) — i.e. two different
    strings (id 120 or 121) depending on the `win_by_kills` mode, both
    telling the player how many wins/kills are still needed. Ink =
    `byte_49A624` (a distinct "still playing" colour).
  - **Match clinched (v73 != -1):** `getstring(36)` (team: `"PLAYER %u WINS
    THE MATCH!"`-style, formatted `v73+1`) or `getstring(35)` (non-team,
    formatted with the winner name string) depending on `dword_46497C`.
    Ink = `byte_497F8F` (a distinct "match over" colour, different from the
    still-playing ink above).
- **Sound:** the winner voice group **`sub_427BFB(2000)`** ("we have a
  winner", already pinned in frontend-flow.md) fires as soon as `v73 != -1`
  is computed — i.e. it plays under the RESULTS scoreboard itself, not only
  under the later VICTORY screen. A short **1500 ms** dwell
  (`sub_413CB0(1500)`) follows before input is accepted.
- **Music:** inherited — this tier draws inside the same handler as DRAW/
  VICTORY and does not call `sub_42741E` itself, so it plays under
  whatever the handler already started (1020 "win", per frontend-flow.md).
- **Input / advance:** a `getkey` loop identical in shape to the DRAW wait
  (frontend-flow.md): any real key → SFX 20; **Enter (13) or Space (32)**
  advance (SFX 10); **Esc/anything ≤ 27** advances too
  (`dword_464A68 = 2`, i.e. "abort to menu"); and an **attract/idle
  auto-advance** at `t0 + 6000 ms` when `sub_42247A()` (idle-timeout gate,
  same helper as the menu's `getvalue(92)` idle check) or the
  `dword_4646B4` flag is set. Network-only: `sub_40C06A()==1` non-host
  players get the SFX-40 "can't dismiss" buzz instead (see
  frontend-flow.md's SFX-40 note — identical gating, N/A to local play).
- **After RESULTS:** if the match is not yet clinched and fewer than 2
  players remain active (`v76 < 2`, i.e. everyone but one has been
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

Confirmed screen layout and flow:

- **Backdrop:** whatever the Options screen already has on-screen (no new
  `sub_4148E5`/`sub_42A088` call — `sub_407B9D` draws directly over the
  Options screen's own frame). Header `getstring(1100)` ("Keyboard
  definitions") at fixed (400, 20) via `sub_41696C`.
- **2×6 button grid** — `for keyboard_set in 0..1, for action in 0..5`: a
  clickable button (`sub_432298`, a mouse-hit-testable label widget, id =
  `1000*keyboard_set + action`) at **x = 320·keyboard_set + 100, y =
  60·action + 60**, labelled `getstring(1110)` ("Key %u, %s") formatted
  with the keyboard-set number and the action name `getstring(1120+action)`
  (action names: **1120 Move Up, 1121 Move Right, 1122 Move Down, 1123 Move
  Left, 1124 Action 1, 1125 Action 2** — six bindable actions per keyboard
  set, matching options.ini's `keydef=<set>,<action>,<scancode>` triples).
  Below each button, the **currently-bound key's name** is shown via
  `getstring(1140)` ("Key: '%s'") using a **256-entry scancode→name string
  table** (`off_45B914[scancode]`) indexed by the low 7 bits of
  `dword_4645BC[10*set + action] & 0x7F` — the SAME array `options.ini`'s
  `keydef=` reader/writer targets (§3's key mapping table).
- **Rebind interaction:** clicking a button (hit id `1000*set+action`)
  calls **`sub_407AD9(action_label)`** @ 0x407AD9 — a modal capture: shows
  "Press key for '%s'" (`getstring(1105)`), waits 500 ms, then polls a raw
  **256-byte keyboard-state array** (`byte_4A2BA0[256]`, the low-level
  scancode state table, distinct from the queued `sub_4102B7` getkey used
  everywhere else in the UI) every frame until a key is down (returns its
  scancode) or Esc cancels (returns 0/no-op). The returned scancode is
  stored straight into `dword_4645BC[10*set+action]` — no validation,
  duplicates across actions are allowed (only options.ini's *reader* clamps
  `set∈[0,1]` / `action∈[0,9]` — note the reader's array is sized for 10
  actions per set even though the UI only exposes 6; slots 6-9 per set are
  therefore write-only from options.ini and have no in-game rebind UI).
- **Restore defaults button** — a separate widget (id 999) labelled
  `getstring(1130)` ("Return to default keys") at (40, 430); selecting it
  calls `sub_40614A()` (hardcodes the 12 default scancodes — the same
  defaults options.ini ships pre-filled, `200/205/208/203/57/46` for set 0
  and `17/32/31/30/2/3` for set 1) and pops a confirm dialog
  (`getstring(1131)`/`getstring(95)`, `sub_414340`).
- **Exit:** Enter/Esc/Space (`< 0x20`, `== 0x20`) leave the screen; row 999
  (F1-style help hook, `sub_41431C` — the generic `.BM` browser, §4) is
  also reachable via key `0x13B`.

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

(Provenance: `sub_407B9D` @ 0x407B9D pseudo.c 8686-8821; `sub_407AD9`
@ 0x407AD9 pseudo.c 8636-8685; `sub_406A2A`/`sub_405DE3`/`sub_410EBF` cross-
reference pseudo.c 7543-7586, 7914-7929; MESSAGES.TXT ids 1100/1105/1110/
1120-1125/1130/1131/1140.)

## 3. The Options screen — `sub_4080DC` (CORRECTED: this is NOT the map editor)

**Correction to `docs/re/frontend-flow.md`'s main-menu table.** Row 3
(`v10==3`, address `sub_4080DC` @ 0x4080DC) is labelled "**map editor**
(EDITOR.BM; the 'Ctrl+E ×6' easter egg lands here)" in that doc. Reading the
actual body of `sub_4080DC` (pseudo.c 8914-9491) shows a **19-item
interactive settings/options list** — team play, random start, node name,
conveyor speed, etc. — with no editor canvas, no `aEditorBm`/`EDITOR.BM`
string reference anywhere in the function, and no drawing/tile-placement
code. `EDITOR.BM` (which does ship in the install) is not referenced by
`sub_4080DC` at all; it is presumably one of the files the help browser (§4)
lists, and the map editor itself (if implemented) lives at a different,
not-yet-located address — **the "Ctrl+E ×6" easter-egg-to-editor claim in
frontend-flow.md is unverified and should be treated as a documented gap,
not a confirmed fact**, pending a separate search for the real editor entry
point. `sub_4080DC` **is** the OPTIONS screen; frontend-flow.md's row-3
label should read "Options" not "map editor".

Confirmed layout — a **19-row toggle/cycle list**, `getvalue(745)=x=55`,
`getvalue(746)=y0=40`, `getvalue(747)=ystep=22`, `getvalue(748)=colour=500`
(VALUELST `; SETTINGS SCREEN: the actual listing of options items` →
`745,55,40,22,500`), cursor via `sub_413BD6(getvalue(745)-20, …)`. Random
glue backdrop (`sub_4148E5`, same helper as the pre-match screens). Every
boolean toggle renders `getstring(<global>+25)` (25=" No ", 26=" Yes ") —
confirming the `+25` idiom already seen elsewhere in the codebase.

| row (v166) | msg id | label (paraphrased) | backing global | on-select (Left/Right or Enter) |
|---|---|---|---|---|
| 0 | 250 | Team Play | `dword_464964` (`team_play=`) | toggle; forces `win_by_kills` off |
| 1 | 251 | Random Start | `dword_464AE8` (`random_start=`) | toggle |
| 2 | 252 | Node Name | (string, `sub_40FE34`) | `sub_4074DC` — text-entry edit (net identity, not persisted to options.ini as a `keydef`-style key; separate from the 22 keys in §3's table) |
| 3 | 253 | Conveyor Speed | `dword_464930` (`conveyor_speed=`) | cycle 0..`getvalue(189)-1` (=0..2: Low/Medium/High, msg 295-297) |
| 4 | 254 | Stomped Bombs Detonate | `dword_464940` (`stomped_bombs_detonate=`) | toggle |
| 5 | 255 | Win Matches By Kill Total | `dword_46497C` (`win_by_kills=`) | toggle; forced off whenever Team Play is on |
| 6 | 256 | Gold Bomberman | `dword_4648BC` (`goldman=`) | toggle; also resets `dword_46492C=-1` (clears the pending roulette winner) |
| 7 | 257 | Enclosement Depth | `dword_464974` (`enclosement_depth=`) | cycle 0..`getvalue(28)-1` (=0..3: None/A Little/A Lot/All the way, msg 315-318) |
| 8 | 258 | Scheme File | `byte_4648C4[100]` (`schemefilename=`) | `sub_4076FE(±1)` — step through the on-disk `.SCH` list |
| 9 | 259 | Play Time | `dword_464948` (`playtime=`), read via `sub_4078FE()` | `sub_4076FE(±1)` — same stepper helper as Scheme File (shared cursor state) |
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
screen; leaving the screen calls `sub_410494(dword_464948)` (re-applies the
play-time tunable) — no explicit Esc branch is visible in the excerpted
tail, consistent with this screen being dismissed the same way as its
siblings (a `< 0x1B` / `<= 0x1B` early-exit already covered by the generic
list-screen pattern used throughout `sub_42B9CE`'s children).

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
| `random_start` | `dword_464940` | normalized to 0/1 |
| `stomped_bombs_detonate` | `dword_464AE8` | normalized to 0/1 |
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
menu at all.** It is invoked automatically during match/round setup
(`sub_410B6E`'s init path, pseudo.c ~15050-15057) whenever **`goldman=1`**
(`dword_4648BC`, the Options-screen "Gold Bomberman" toggle, §3) and a
pending round winner exists (`dword_46492C != -1`) and the game is local
(`!sub_40C06A()`); it draws a genuine animated Lissajous-curve spinning
wheel (VALUELST `; Goldman Roulette Wheel` block: `1000,320,240` = wheel
centre, `1002,200,150` = perimeter radii, `1004,70` = circle resolution,
`1006,1,1` = Lissajous X/Y params, `1010,5` = how many seconds the
"twinkling" finish lasts) that lands on and highlights the round's winner
before the RESULTS tier (§1) is shown.

**Corrected mapping:**
- Main menu row 5 = the **Help/Manual browser** (`sub_41431C`→
  `sub_414235`), listing all `.BM` files — best reproduced as a simple
  file-picker over the same `.BM` viewer the port already has, not an
  inert stub.
- **There is no main-menu row for the Goldman Roulette Wheel** — it is
  conditional, automatic, mid-round-transition presentation, gated purely
  by the `goldman=` option. It is a genuinely separate future feature
  (an animated wheel widget) from the menu row, and belongs wired into the
  round-transition flow (between round-teardown and RESULTS), not the menu.

(Provenance: `sub_41431C` @ 0x41431C pseudo.c 16996-17001; `sub_414235`
@ 0x414235 pseudo.c 16933-16995; `sub_41404B` @ 0x41404B pseudo.c
16867-16894; `sub_4034BC` @ 0x4034BC pseudo.c 5921-6132 (roulette-wheel
math helpers `sub_403382`/others at 5876-5919); its caller pseudo.c
15043-15057; VALUELST `15,1`, `; Goldman Roulette Wheel` block 805/
1000-1010; MESSAGES.TXT ids 4/5/95/600/610; `ROULETTE.BM` header line
confirms the help-topic framing.)

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
