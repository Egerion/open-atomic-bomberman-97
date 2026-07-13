# Pre-match SETUP ("glue") screens — RE

Reverse-engineered from `BM95.EXE` (pseudo.c). The original's pre-match
configuration is a small multi-screen flow reached from the two "setup" rows of
the main menu (`sub_42B9CE` v10 == 1 and 2). This doc pins the assets, the
getvalue-driven layout, the string IDs, the player-config model, and the input
flow so the screens can be reproduced 1:1. It records structure only — the
actual `MESSAGES.TXT` text and the `GLUE*.PCX` art load at runtime from the
install and are never committed (clean-room, same as every other asset).

## Follow-up (2026-07-08): the key-remap UI is `sub_407B9D`, off the Options screen

The key-remap UI this doc's CORRECTION left as "elsewhere" is now located:
**`sub_407B9D` @ 0x407B9D**, reached from the Options screen's (`sub_4080DC`)
"Define keyboard layouts" row — a MOUSE-driven 2×6 clickable button grid,
two keyboard sets × six actions, each rebindable via a raw-scancode capture
(`sub_407AD9`/`byte_4A2BA0[256]`). Bindings live in the same
`dword_4645BC[10*set+action]` array `options.ini`'s `keydef=` reads/writes,
and are flushed to disk on normal app exit (an atexit-style hook,
`sub_405DE3`), not on screen close. Full RE: `docs/re/results-and-options.md`
§2 (remap UI, incl. the 2026-07-13 mouse/cursor/defaults corrections) and §3
(the Options screen + the complete 22-key options.ini table). This resolves the open item and confirms `sub_42B0CE`/`sub_42B47D`
genuinely have nothing to do with key rebinding — they stay the START/JOIN
NET GAME screens per the CORRECTION below.

## CORRECTION (2026-07-05): the real player-setup screen is in `sub_410F81`

The VALUELST coordinate legend (below) shows that `sub_42B0CE` / `sub_42B47D`
(main-menu rows 1/2) are the **START / JOIN NET GAME** screens (getvalue 765-778
and 750-763 are labelled "…NET GAME SCREEN"), NOT the player/controller setup.

The **real player-input-type setup screen** lives inside **`sub_410F81`
(@0x410F81)** and is driven by **getvalue(705-723)** = "PLAYER INPUT TYPE
SELECTION". Its confirmed layout (VALUELST, X,Y,YS,W columns → consecutive
getvalue ids):

- Header (msg **50**) at getvalue(705)=**(40,140)**.
- **10 players**, `for i in 0..9`: `sub_421DD2(i, &type, &sub)` returns the
  player's input-type category (0-4) + a sub-index; the line is msg **220**
  (category 0 / off), **221** (1), **222**+sub (2), **223**+sub (3), **224** (4),
  prefixed by the player label msg **51** (formatted with i+1). Drawn at
  getvalue(710)=x **70**, y = getvalue(711)=**170** + getvalue(712)=**24**·i,
  clip width getvalue(713) (see the column-4 correction in the layout section
  below). The cursor for the selected player (`v108`) is drawn via
  `sub_413BD6(x-15, …)`. In team mode (`dword_464964`) a team marker (msg **230**,
  `sub_4141F8`) is appended.
- Joystick pane: heading msg **40** at getvalue(715)=**(300,140)**; per-joystick
  list msg **41**(+i) / msg **42** ("none") at getvalue(720)=x **320**, y =
  **170**+**24**·i, from `sub_429628(i)` (joystick present).

### State model — CONFIRMED (`sub_421DD2` @0x421DD2)

`sub_421DD2(i, &type, &sub)` reads player i from the player array
`dword_461BC4[38*i]` (152 B/player): **type = player byte +16**, **sub = byte
+17**. That +16 is the SAME field the AI reads (docs/re/ai.md — +16==1 →
computer). The input-type categories are:

- **0 = OFF** — slot inactive (msg 220)
- **1 = COMPUTER** — the AISystem drives it (msg 221)
- **2 = keyboard** human (msg 222 + sub = which key-set)
- **3 = joystick** human (msg 223 + sub = which stick)
- **4 = other controller** human (msg 224)

The mover routes `+16 == 1` to the AI and every other active category (2/3/4) to
the human input decoder; `+16 == 0` is absent.

**Roster → `MatchConfig`:** `player_count` = count of slots with `+16 != 0`;
`ai[i] = (+16 == 1)`; a human slot (`+16 ∈ {2,3,4}`) binds a keyboard/controller.
We support OFF / COMPUTER / KEYBOARD / JOYSTICK (type-3 slots) — SDL3 gamepad
support shipped 2026-07-08 (`GamepadMapper`: enumeration, hotplug, d-pad/
left-stick + south/east buttons, `sub_421E80`'s confirmed slot-type wrap
order; ROADMAP "SDL3 gamepad support — DONE 2026-07-08"; `docs/re/
coverage-audit.md` table row #39). Type-4 "other controller" has no SDL3
analogue and is not ported. The two net-game screens below are kept for
reference but are NOT the screen we reproduce.

## Backdrop — a RANDOM glue picture (CONFIRMED: `sub_4148E5` @0x4148E5)

Both pre-match screens draw a **random** decorative backdrop via the shared
`sub_4148E5()` helper (called by `sub_410F81` @ pseudo.c 15121 and `sub_406DDE`
@ 8089). Its body (CONFIRMED):

```
v3 = getvalue(16) <= 1 ? 1 : getvalue(16);   // getvalue(16) = how many glue backdrops
idx = rand() % v3;                            // uniform random
name = sprintf("glue%u.plt", idx);            // aGlueUPlt
apply_palette(name);  load(name);             // sub_4151AD/sub_411D17/sub_4151CC
```

`getvalue(16) == 7` (VALUELST `16,7`), install ships `GLUE0.PCX`..`GLUE6.PCX`
(7) + `glue0.plt`..`glue6.plt`. So each pre-match screen = a random `GLUE<n>`
full-screen backdrop with the UI text drawn over it. (Presentation-side random
pick — a presentation LCG, never `State::rng`.) The `GLUE<n>.PCX` image is
blitted by the shared draw pass (`sub_429790`); the palette is the `.plt`.

## Music — 1020 (WIN.RSS), NOT 1040 — CORRECTED (`sub_42A3F6` @0x42A3F6)

The prior pass's `sub_42741E(0x410)` = 1040 guess was **WRONG**. `sub_410F81`
itself contains **no** `sub_42741E` call — it inherits whatever track the caller
started. The caller is the **Play handler `sub_42A3F6`** (menu row 0), which at
its entry runs `sub_42741E((_DWORD*)0x3FC)` = **music 1020** (WIN.RSS, SOUNDLST
label `win`) and then calls `sub_410F81()` (decompile ~29697-29698). The
following level/rounds screen (`sub_406DDE`) also plays nothing new, so **both
pre-match screens run under the 1020 track**. (0x3FC == 1020 hex-confirmed; the
same id `sub_42A3F6` keeps under the VICTORY results screen — see
frontend-flow.md.) It is *not* the menu track 1010 and *not* 1040.

## String source — MESSAGES.TXT via getstring `sub_4124A4(id)`

`sub_4124A4(id)` returns a format string by numeric id from a loaded string
table; the table is the install-root text file **`MESSAGES.TXT`** (id → text,
C `printf`-style `%` specifiers; the file's own header warns not to touch the
`%`). It is the user's own game file — parse it at runtime, DO NOT commit its
text. Needs a small `libs/assets` parser: id-keyed entries, `;`-comment lines
skipped, `%u/%d/%s` format specifiers preserved for `sub_4518D0` (= `sprintf`).

Message IDs the setup screens use (purpose only; text stays in the file):

| id  | used by | purpose |
|-----|---------|---------|
| 60  | `sub_42B47D` | controller-screen column/header label |
| 62  | `sub_42B47D` | an ACTIVE slot's line (formatted with the controller index) |
| 63  | `sub_42B47D` | an OFF/inactive slot's line |
| 66  | `sub_42B47D` | controller-screen title |
| 70–73 | `sub_42B0CE` | screen-A labels (active/inactive item, headers) |
| 80  | `sub_42B47D` | the "press a control now" bind prompt (animated, cycles `dword_45BFB4[c&3]`) |
| 95, 100, 110 | both | bind-confirm / bind-cancel overlays (`sub_414340`) |

## Screen B — controller assignment (`sub_42B47D` @ 0x42B47D)

The main setup screen: assign each of the 10 player slots to a controller (=
human) or leave it OFF. Structure per frame:

1. Enter mode 1 (`sub_40C839(1)`), music 1040, random glue backdrop.
2. Title = `getstring(66)` blitted at **getvalue(750)=y? , 751, 753** (the
   original's `sub_41696C(surface, text, x, getvalue(753), getvalue(751),
   fg=byte_497F8F, bg=byte_495390[0])` — 751/753 are the y/colour pair, plus
   750). Header = `getstring(60)` at **755 / 756 / 758**.
3. **10-slot list** — `for (i=0;i<10;++i)`:
   - `sub_40F163(i)` → slot active? If active: type/index via `sub_40F191(i)` /
     `sub_40F1BD(i)`, line = `getstring(62)` formatted with the controller index;
     else line = `getstring(63)` (off).
   - Blitted at **getvalue(760) (x), getvalue(761)+getvalue(762)*i (y),
     getvalue(763) (colour)** — 761 = list origin y, 762 = per-row y step, 760 =
     x, 763 = colour. (Row y = `761 + 762*i` from the `v41 = v11 + v10` with
     `v10=getvalue(761)`, step `getvalue(762)`.)
   - The selection cursor is drawn for row `v59` via `sub_413BD6(getvalue(760)-20, …)`.
4. Input (`sub_4102B7` getkey; any real key → SFX **20**):
   - **Up = 328 (0x148)** → `--sel` (wrap 9); **Down = 336 (0x150)** → `++sel` (wrap 0).
   - **Space = 0x20** → SFX **10**, then BIND: for the selected slot, run a
     ~3000 ms detect loop (`sub_40EC6F(sel)` polls controllers; `sub_40F386()` =
     a control was pressed) showing the animated `getstring(80)` prompt at
     (150,400); on detect, bind that controller to the slot; on timeout, cancel
     (overlay `getstring(100)`/`getstring(110)` + `getstring(95)`). Restarts the
     screen (`LABEL_3`).
   - **Any key < 0x20** (Enter 13 / Esc 27) → break the loop → leave the screen
     (proceed to the match / back to the menu).

Player-config store: **`unk_4632CC`, 404 bytes per player × 10** (accessor
`&unk_4632CC + 404*i + 4`, pseudo.c ~29160). Holds per-slot: active flag, the
assigned controller/type (`sub_40F1BD` returns <4 = a keyboard/joystick index),
and (for team play) the team. Human = has a controller; the remaining active
slots the match fills as COMPUTER (the AI's `+16==1` type). OFF = inactive.

## Screen A — game options (`sub_42B0CE` @ 0x42B0CE)

Mode 2 (`sub_40C839(2)`), music 1040, random glue backdrop. A **4-item** list
(`for i in 0..3`, `sub_40F1E9(i)` active? `sub_40F217(i)` value; strings 71
active / 72 off) at getvalue **775 (x) / 776+777*i (y) / 778 (colour)**, plus
two headers at **765/766/768** and **770/771/773** (strings 73 and 70). Same
getkey model (20 on any key, Enter proceeds). This is the game-type / options
pane (team play, etc.) — lower priority than screen B for a playable roster.

## COMPLETE RE (2026-07-05): the two pre-match screens end-to-end

The Play path is **`sub_42A3F6` → `sub_410F81` (player-input screen, which
then runs `sub_406DDE`, the level/rounds screen, from its own tail) →
`sub_410B6E` (match init) → the round loop**. CORRECTED (2026-07-08): the
level screen call is at `sub_410F81`'s tail (pseudo.c 15516, on the
non-aborted exit path), NOT inside `sub_410B6E` as first written;
`sub_410B6E` (called at 29700, after `sub_410F81` returns) is pure match
init. The two net-game screens `sub_42B0CE`/`sub_42B47D` above are a
SEPARATE flow (JOIN/START NET GAME, VALUELST legend 750-778) — NOT
reproduced here.

**Follow-up (2026-07-08): `sub_410F81` hosts two more things at its head.**
(a) The **Goldman Roulette Wheel** pre-roll: before drawing anything, the
function calls `sub_4034BC` when goldman is on, the game is local, attract
is off, and a gold player is pending — full RE in
`docs/re/goldman-roulette.md`. (b) The **attract-mode short-circuit**: when
the menu idle timeout fired (`dword_464938`), the function skips both setup
screens entirely and auto-fills a random all-CPU roster + random level —
see frontend-flow.md "Attract mode".

### Screen 1 — PLAYER INPUT TYPE SELECTION (`sub_410F81` @0x410F81)

VALUELST legend (confirmed by the file's own comments before the 700 block).
**Column-4 CORRECTION (2026-07-12 pixel-RE pass):** the 4th column is the CLIP
WIDTH handed to the text primitive `sub_41696C` as its max-width argument
(header 200, slot rows 150, joystick heading 170, joystick rows 320) — NOT a
colour. Colour never comes from VALUELST on this screen: every string is
drawn by `sub_41696C`'s 4-outline-passes + ink scheme (pseudo.c 18516-18572;
the (w+2) scratch buffer pins the ±1 px outline), header/joystick pane in
white `byte_49D38F` over black, slot rows in `sub_41672F(i)` (the slot's own
.RMP-tail colour) over `sub_416867(i)` (black; WHITE for slot 1, the black
player — docs/re/player-colour.md CORRECTION), and the centred footer
`getstring(330)` "Press F1 for help" at VALUELST 790 = (320,440) in cyan
`byte_497F8F` (96,252,252). There is NO state dimming (OFF/COM rows keep full
ink) and NO selected-row recolour — the selection is marked solely by the
bomber-dude cursor `sub_413BD6` (MISC.ANI "cursor1", hotspot-anchored at
`getvalue(710)-15, row_y` — this screen alone uses -15; options/level use
-20; idle step 0, timed blink per VALUELST 690 = {base 2, spread 2} seconds,
one step per rendered frame). F1 (key 0x13B, 15432-15436) opens the generic
sub_41431C *.BM browser. All ported 2026-07-12 (present_setup rewrite:
outlined text, real colours, cursor1 indicator, footer, F1; the invented
grey joystick pane, +70 selected-row boost and key-legend line removed).
```
; PLAYER INPUT TYPE SELECTION:
705, 40,140,  0,200   ; text heading of player input type listings
710, 70,170, 24,150   ; actual listing of player input types  (x, y0, ystep, W)
715,300,140,  0,170   ; text heading of available joysticks
720,320,170, 24,320   ; actual listing of available joysticks
```

- **Backdrop:** random `GLUE<n>` via `sub_4148E5()` (getvalue 16 = 7). **Music:**
  1020 (inherited from `sub_42A3F6`; the screen starts no track). Header
  `getstring(50)` at getvalue(705) = **(40,140)**, clip width getvalue(708)=200
  (white/black-outline ink — see the column-4 correction above).
- **10-slot list** (`for i in 0..9`): `sub_421DD2(i, &type, &sub)` reads
  **type = player byte +16**, **sub = byte +17** from `dword_461BC4[38*i]`
  (152 B/player). Line by category:
  - `type==1` → `getstring(221)` (**COMPUTER**)
  - `type==2` → `getstring(222)` + sub (**KEYBOARD %u**, sub = which key-set)
  - `type==3` → `getstring(223)` + sub (**JOYSTICK %u**)
  - `type==4` → `getstring(224)` (**OTHER**)
  - else (0) → `getstring(220)` (**OFF**)
  The row is ONE combined sprintf of `getstring(51)` = "Player %u: %s" with
  args (i+1, typetext) — pseudo.c 15169-15195 (a two-piece concat leaves a
  literal "%s" on screen with the real MESSAGES.TXT). Drawn at x =
  getvalue(710) = **70**, y = getvalue(711)=**170** + getvalue(712)=**24**·i,
  clip width getvalue(713)=**150**. Cursor for the selected slot (`v108`) via
  `sub_413BD6(getvalue(710)-15, …)`.
- **COLOUR is per-slot and IMPLICIT — there is NO colour picker on this screen.**
  Each of the 10 slots has a **fixed colour keyed by its index**: VALUELST
  **200-247** are RGB triplets (stride 5) with the file's own comments
  `0.RMP: white, 1.RMP: black, 2.RMP: red, 3.RMP: blue, 4.RMP: green,
  5.RMP: yellow, 6.RMP: cyan, 7.RMP: magenta, 8.RMP: orange, 9.RMP: purple`.
  The engine recolours player i's sprites through `i.rmp` (`aURmp = "%u.rmp"`,
  index = player number) — an index-remap TABLE, not a truecolour tint (the full
  format + blit is docs/re/player-colour.md). So "player colour" = the slot's
  intrinsic colour; the faithful UI just DISPLAYS it (tint each slot's label with
  its colour). The label ink `sub_41672F(i)` derives from the slot's stored RGB
  `byte_460BD0/BDA/BE4[i]`, which is the **`.RMP` tail** (the engine overwrites
  the VALUELST-derived percents with the file's tail on load). `Tuning::
  color_rgb[10][3]` mirrors VALUELST 200-247 and is the fallback when a `.RMP`
  is absent.
- **TEAM** is the player byte **+84** (`dword_461BC4[38*i + 21]`, `LOBYTE`),
  read by `sub_4223E7(i)` and written by `sub_422437(i, v)`. Rendered ONLY in
  team mode (`dword_464964 != 0`), for EVERY slot (not gated on that slot's own
  team value): a trailing marker glyph, `getstring(230)`, appended after the
  name+type line (pseudo.c ~15212-15224) — CONFIRMED **unformatted** (no `%u`:
  the original never sprintf's it, just two back-to-back `sub_4124A4(230)`
  reads with no intervening `sub_4518D0`), inked via `sub_4141F8(team)`
  (returns `byte_49D0DA` red (252,80,80) for team 1 else `byte_49D38F` white
  (255,255,255), `docs/re/player-colour.md` "Team Play colour override"). So
  the COLOUR of the marker glyph — not its text — is what tells the two teams
  apart. The name+type line itself is NOT recoloured: `sub_410F81` deliberately
  saves+zeroes `dword_464964` around its own `sub_41672F(i)`/`sub_416867(i)`
  ink lookup (pseudo.c ~15191-15204), forcing that text to keep the slot's own
  individual `.RMP` colour even under Team Play (`sub_41672F` itself DOES
  branch on team mode when called normally — see player-colour.md's "Setup-
  screen slot ink" — this caller just neutralises that branch for this one
  call). Team mode is toggled on the OPTIONS game-type screen, OFF by default.
  The 'T'/'t' key on THIS screen toggles the +84 byte between 0 and 1.
- **TEAM default — CORRECTED 2026-07-09** (reported symptom: Team Play forms
  only a white team and the match "ends instantly"): `sub_410F81` calls
  `sub_4046CC()` unconditionally as its second statement (pseudo.c line
  15046). `sub_4046CC`, when a CD is present, calls `sub_403EEE(byte_4648C4)`
  (pseudo.c line 6573), which ITSELF unconditionally calls `sub_4049C0()`
  first (pseudo.c line 6321) before attempting to parse any saved profile.
  `sub_4049C0` (pseudo.c lines 6702-6718) sets, for every slot `j` in
  `[0,10)`: `dword_46481C[12*j] = <default colour x>`, `[12*j+4] = <default
  colour y>`, and **`[12*j+8] = j & 1`** — the TEAM field, alternating
  0,1,0,1,... by slot parity. `sub_403EEE`'s own save-file parse loop only
  ever overwrites a slot's colour fields (`+0`/`+4`) from disk; TEAM (`+8`)
  is left at the `j & 1` default UNLESS a "-S slot,x,y,team" 5-field profile
  line is present (pseudo.c line 6427, `dword_46481C[12*v31+8] =
  sub_4516C1(v39)!=0`) — a rare, hidden colour-profile file format this port
  does not implement. `sub_403EEE` finishes by pushing all 10
  `dword_46481C[...+8]` values into `dword_461BC4[38*i+21]` via
  `sub_422437(k, ...)` (pseudo.c line 6491) — the exact array `sub_4223E7`/
  the setup screen's own TEAM column reads. Net effect: **every time the
  setup screen loads, TEAM resets to an alternating 0/1/0/1 pattern by slot
  index**, not to a flat 0 and not persisted from a prior visit. The port's
  `GameApp::present_setup()` (`libs/game/src/game_app.cpp`) now mirrors this
  with `for (slot) setup_team_[slot] = slot & 1;` at entry. Before this fix,
  `setup_team_` defaulted (and stayed) all-0, so Team Play ON without anyone
  pressing 'T' put every player on the SAME sim side: `MatchConfig::team[]`
  was uniformly 1, giving everyone the team-1/WHITE `0.RMP` colour override
  (nobody red) and making `sim::sides_remaining()` read `<=1` from tick 0
  (round-end "one side left" firing before any player could act). The
  round-end rule itself was not the bug — `sides_remaining() <= 1` is the
  correct, deliberate generalisation of the pre-existing `alive_count() <=
  1` solo rule (docs/re/ai.md "our semantics"); it only misfired because the
  roster it was fed was degenerate.
- **Joystick pane:** heading `getstring(40)` at getvalue(715)=(300,140); per-stick
  list `getstring(41)`(+i) if `sub_429628(i)` (present) else `getstring(42)`
  ("none") at getvalue(720)=x **320**, y **170**+**24**·i, colour getvalue(723).

**Input-type cycle helpers (CONFIRMED):**
- `sub_421E33(i, type, sub)` — set slot i's +16=type, +17=sub outright.
- `sub_421E80(i)` — **cycle FORWARD** one step: 0(off)→1(computer); 1→2 kbd
  sub 0; 2 sub 0→2 sub 1; 2 sub 1→3 joy 0; joystick advances through present
  sticks (`sub_429628`) then wraps back to 0(off). This is the main cycle.

**Key table (raw `sub_4102B7` codes, EXHAUSTIVE, `sub_410F81`):** any real key
(`≠ -1,-2`) first fires SFX **20** (`sub_427961(20)`).

| key | effect |
|---|---|
| `328` Up (0x148) | `--v108` cursor, wraps 0→9 |
| `336` Down (0x150) | `++v108` cursor, wraps 9→0 |
| `333` Right (0x14D) | `sub_421E80(v108)` — **cycle input type forward** (unless type==4) |
| `331` Left (0x14B) | reset the slot: `sub_421E33(v108,0,0)` (type/sub→0 = OFF), unless type==4 |
| `48` '0' / `111` 'o' | `sub_421E33(v108,0,0)` — set slot OFF |
| `84` 'T' / `116` 't' | toggle TEAM +84: `sub_422437(v108, sub_4223E7(v108)==0)` |
| `13` Enter | (< 0x20 branch) leave the screen → proceed to match init |
| `27` Esc | back out: `dword_46492C=-1`, `dword_464A68=2`, SFX **10** |
| `32` Space | in NET only (`sub_40C06A()==1`) — bind detect; local = inert |
| `1` | dev: set all 10 slots to COMPUTER (local only) |
| `288`/`315` | menu toggles / roulette (`sub_413D45`/`sub_41431C`) |
| `67` 'C' ×5 (local only) | **CAMPAIGN picker** — 5 consecutive presses (same-key counter `v115`, any other key resets it; `!sub_40C06A()` guard) open the `*.cam` file picker `sub_4015C6` (pseudo.c 15357-15365). Missed by the original "EXHAUSTIVE" pass — full chain in `docs/re/campaign.md` |

So the local-play controls are: **Up/Down pick a slot; Right cycles its type
(OFF→CPU→KBD0→KBD1→JOY…→OFF); Left/'0' set it OFF; 'T' toggles its team; Enter
starts; Esc cancels.** No colour key. Plus the hidden 'C'×5 campaign easter
egg above.

### Screen 2 — LEVEL & ROUNDS (`sub_406DDE` @0x406DDE) = the "map select"

VALUELST legend (`; OPTIONS SCREEN:`):
```
730,400,100,5,5     ; where the "sample" blocks are displayed (X,Y,XSize,YSize=5x5)
735, 55,170, 24,300 ; the actual listing of options items  (x, y0, ystep, colour)
```

- **Backdrop:** random `GLUE<n>` (`sub_4148E5`), **music 1020** (inherited).
- A **2-item list** (`v34 = 2`): row 0 = **LEVEL**, row 1 = **NUMBER OF WINS**.
  Drawn at x = getvalue(735) = **55**, y = getvalue(736)=**170** +
  getvalue(737)=**24**·row, colour getvalue(738)=**300**. Cursor via
  `sub_413BD6(getvalue(735)-20, …)`.
- **LEVEL** = `dword_45E0B8`: **−1 = RANDOM**, else **0..getvalue(35)-1**.
  `getvalue(35) == 11` = number of built-in levels. Level name =
  `getstring(dword_45E0B8 + 150)` (string 149 for RANDOM), formatted into the
  level-line `getstring(210)`. The 11 level names are also `getvalue(450..460)`
  (`new traditionalist green acres, classic green acres, hockey rink, ancient
  egypt, coal mine, beach, aliens, haunted house, under the ocean, deep forest
  green, inner city trash`). On the LEVEL row: **Left(331)** `--level` (wraps
  below −1 to `getvalue(35)-1`); **Right(333)** `++level` (wraps above
  `getvalue(35)-1` to −1). Each change calls `sub_40FA66(level)` (stores it;
  net-syncs in mode 2).
- **NUMBER OF WINS** = `dword_45E0B4` (1..100): rounds line `getstring(211)`
  formatted with it, header `getstring(dword_46497C + 208)`. On the WINS row:
  Left/Right = ±1, Up(0x148 while row 1)/Down(0x150) = the `sub_413CB0` deltas,
  and PgUp(0x174)/PgDn(371) = ±5 (`dword_45E0B4 += 5` clamped 1..100),
  `sub_40FAD5` applies it.
- **Confirm/cancel:** any real key → SFX 20. **Enter (13)** →
  `sub_427961(10)`; **commit `dword_464998 = dword_45E0B8` (level) and
  `dword_464A7C = dword_45E0B4` (wins)**; done. **Esc (27)** → `dword_46492C
  = -1` (forfeits any pending Goldman gold player — CORRECTED 2026-07-09,
  `docs/re/goldman-roulette.md` §2.1: previously undocumented here) THEN
  `dword_464A68 = 2` (pseudo.c 8186-8191). This does **NOT** step back to
  the player-setup screen — `sub_406DDE` is called from `sub_410F81`'s own
  TAIL (§ below, "How the level flows into the match") with nothing after
  it but a return, so Esc here aborts the WHOLE Play flow straight to the
  menu, the same shape as the Goldman wheel's own Esc
  (`goldman-roulette.md` §5). Up(328)/Down(336) move between the 2 rows
  (wrap).

**How the level flows into the match (`sub_410B6E` @0x410B6E):** at match init
it resolves the committed level:
```
if (dword_464998 < 0)                 // RANDOM
    for up to 200 tries:
        dword_46499C = rand() % getvalue(35)
        if getvalue(dword_46499C + 1150) break   // RANDOM-LEVEL enable flags
else
    dword_46499C = dword_464998        // the chosen specific level
```
`dword_46499C` (0..10) then names every per-level asset: `FIELD<n>.PLT`/
`FIELD<n>.PCX` (`aFieldUPlt_0`), `EXTRA<n>.RES` (`aExtraURes`), `TILE<n>*`
(`aTileUSolid`/`aTileUBrick`), and per-level flag rows `getvalue(<n>+340)` /
`getvalue(<n>+450)`. The **RANDOM LEVEL enable array is VALUELST 1150-1160** (11
flags; the file disables `1152` hockey rink and `1156` coal mine by default) —
already mirrored by `Tuning::level_enabled[11]` and honoured by
`match::pick_stage` (its seed-based pick from the enabled set == the original's
200-try loop). So a specific level → override the stage index directly; RANDOM →
keep `pick_stage`.

### The "sample" block preview — CONFIRMED (`sub_406AA3` @0x406AA3)

`sub_406DDE`'s frame loop calls a separate helper, `sub_406AA3()`, exactly
once per **screen entry** and once again per **LEVEL row change** (the outer
loop's `v35` flag: set to `1` before the loop's first pass, and
`++v35`'d — i.e. re-armed — whenever `sub_40FAB3() != dword_45E0B8` fires,
the net-sync path for a level edit; local play's Left/Right level cycle sets
`dword_45E0B8` directly and takes the SAME `v35` re-arm on the next frame
through the `sub_40C06A()==1` branch above it). WINS-row edits do **not**
re-run it. Full body (pseudo.c 7938-8041):

```c
v18 = getvalue(730);   // X = 400
v19 = getvalue(731);   // Y = 100
v24 = getvalue(732);   // XSize = 5  (columns)
v25 = getvalue(733);   // YSize = 5  (rows)
sub_4151AD();          // apply the CURRENT (glue) palette — no new backdrop
```

1. **Field swatch panel** — `FIELD<n>.PLT` for the SELECTED level (`n =
   dword_45E0B8`) or, for RANDOM, `n = rand() % max(getvalue(35),1)` — picked
   ONCE per `sub_406AA3()` call, i.e. re-rolled only on entry/level-change,
   not every frame. `sub_4150F0` decodes it into a raw framebuffer; if that
   succeeds:
   - `sub_4168B5(v18-22, v19-20, YSize*36+22, XSize*40+24, byte_49D38F)` — a
     filled border/background rect, inset 22px left/20px up from the grid
     origin, sized to the grid box + a ~20-24px margin, colour
     `byte_49D38F` (a fixed UI ink, not level-dependent — the same byte
     `sub_4141F8` uses for the "team 2" tint elsewhere in this doc).
   - `sub_4152D7(&field[12*640], v18-20, XSize*40+20, v19-18, YSize*36+18,
     …)` — CORRECTED 2026-07-12 (level&rounds audit): the source expression
     indexes an `int*`, so `12*640` int elements = **48 scanlines**, not 12;
     and `sub_4152D7` -> `sub_4428B4` (pseudo.c 47637-47641) is a **plain
     1:1 rect copy, NOT a stretch**. So: a 220×198 crop of FIELDn.PCX
     starting at (0,48), copied to (v18-20, v19-18) = (380,82) — which puts
     the backdrop's own board grid (origin (20,68)) flush under the drawn
     tile cells (dest inset +20,+18). The earlier "12 scanlines +
     stretch-blit" reading is superseded. Border fill = the general white
     `byte_49D38F` (240,248,252).
2. **The 5×5 block grid** — `for (i in 0..YSize) for (j in 0..XSize)` at
   cell `(v18 + 40*j, v19 + 36*i)` (40×36 = the same `TILEn.ANI` cell pitch
   the in-match renderer uses, `sim::kTileW/kTileH` — **no stretching**, 1:1
   native tile size):
   - `(j&1) && (i&1)` → **always SOLID** (the checkerboard-parity cells, the
     same parity the in-match board's outer solid lattice uses).
   - else if `(j>1 || i>1)` → **BRICK with probability 4/5** (`rand()%5 !=
     0`); the top-left 2×2 corner (`j<=1 && i<=1`) is reserved as guaranteed
     clear (a "spawn corner"), matching every built-in level's own top-left
     start-safety carve-out.
   - else → **blank** (no tile drawn).
   - For a drawn (solid/brick) cell, the **tileset index is re-picked per
     cell**: a specific level draws every cell from that level's own
     `TILE<n> solid`/`TILE<n> brick` (`n = dword_45E0B8`, CONFIRMED — pinned
     art, no randomness beyond the brick/blank coin flip); RANDOM (-1)
     re-rolls `n = rand() % max(getvalue(35),1)` **independently for every
     cell**, so a RANDOM-level preview shows a patchwork of different
     levels' tile art in the same grid — a deliberate "random" novelty
     effect, not a bug. Blit is `sub_41532B`, a stretch-blit into the exact
     40×36 cell rect (a no-op stretch since the source ANI frames are
     already 40×36).
   - The cursor row highlight (`sub_413BD6`) and everything else in the
     outer loop draw over/around this panel every frame; the panel itself
     is a snapshot from the last `sub_406AA3()` call, not redrawn per frame.
3. `sub_415189()` restores the caller's palette on return (paired with the
   `sub_4151AD()` at entry — the same apply/restore pair `sub_4148E5`'s glue
   backdrop uses).

**Reproduction note:** this is cosmetic set-dressing (a random illustrative
maze pattern + the level's own tile art), not the level's actual layout —
faithfully reproducing the checkerboard/4-in-5-brick/2×2-clear-corner rule
and the per-cell RANDOM tileset re-roll is what "1:1" means here. PORTED
2026-07-12: the port now also does the exact 48-row 1:1 crop (the old
whole-texture stretch is gone), the white border fill, the outlined white
row text with the cursor1 sprite as the ONLY selection marker, the
getstring(211) "%u %s to win match" wins line (208/209 Wins/Kills), the
centred cyan F1 footer, Space-accept + the 1 s accept debounce, Ctrl+arrow
±5 wins, F1 -> help browser, and Escape discarding the working level/wins
copies (present_map_select, game_app.cpp).

### Message IDs the two screens use (purpose only; text stays in the file)

| id | screen | purpose |
|---|---|---|
| 50 | player | header ("PLAYER SETUP") |
| 51 | player | per-slot label, `%u` = i+1 |
| 220 | player | slot line: OFF |
| 221 | player | slot line: COMPUTER |
| 222 | player | slot line: KEYBOARD `%u` (sub) |
| 223 | player | slot line: JOYSTICK `%u` (sub) |
| 224 | player | slot line: OTHER |
| 230 | player | team marker (team mode only) |
| 40/41/42 | player | joystick heading / present / none |
| 45/46/48/96 | player | bind-abort overlays (net only) |
| 149 | level | "RANDOM" level name |
| 150+n | level | built-in level n's name |
| 208+ | level | rounds header |
| 210 | level | level line format (`%s` = level name) |
| 211 | level | rounds line format (`%u` = wins) |

## Reproduction (implemented in `libs/game`)

1. `present_setup` (rewritten) — the PLAYER INPUT TYPE screen: random `GLUE<n>`
   backdrop, **music 1020**, header `getstring(50)`, the 10-slot list at
   getvalue(705/710-713), **each slot label tinted with its authentic on-screen
   colour** via `AssetStore::slot_color(i)` — the truecolour equivalent of the
   original's per-slot ink `sub_41672F(i)` (quantise the slot's `.RMP` tail RGB
   to 5 bits/channel, `min(v/3,31)`, then expand5 in place of the palette LUT).
   The `.RMP` tail is the authoritative per-colour value (docs/re/player-colour.md);
   `Tuning::color_rgb` is only the fallback when a `.RMP` is absent. Input-type per
   slot (OFF/COMPUTER/KEYBOARD0/KEYBOARD1/JOYSTICK — SDL3 `GamepadMapper`,
   ROADMAP "SDL3 gamepad support — DONE 2026-07-08"), a per-slot
   TEAM flag toggled by 'T'. Keys: Up/Down slot, Right cycle type, Left/'0' off,
   'T' team, Enter start, Esc cancel — mirroring the table above. **Team Play
   marker**: whenever `team_play_` is on, every slot line gets a trailing
   "TEAM"-style marker glyph inked red `(252,80,80)` (team byte 1) or white
   `(255,255,255)` (team byte 0) — the port's equivalent of `sub_4141F8`
   (docs/re/player-colour.md "Team Play colour override"); the slot's own
   name+type text stays in its individual `.RMP` colour regardless of Team
   Play, matching the original's `dword_464964`-neutralising call site. The
   SAME red/white split also now recolours every in-match sprite that inherits
   a player's colour (`Renderer::render_colour`, same doc section) — the fix
   for the reported "Team Play doesn't visibly split the roster" gap.
2. `present_map_select` (new) — the LEVEL & ROUNDS screen: random `GLUE<n>`
   backdrop, **music 1020**, the LEVEL row (RANDOM + the 11 named levels,
   `getstring(150+n)` / `getstring(149)`) and the WINS row (1..100), at
   getvalue(735-738). Left/Right cycle the highlighted row's value, Up/Down move
   between rows, Enter commits, Esc backs to the player screen. Also draws the
   **sample-block preview panel** at getvalue(730-733) (X=400,Y=100,5x5 cells,
   pinned above): a bordered box behind a 5x5 grid of solid/brick tiles in the
   selected level's own `TILE<n>` art (RANDOM re-picks a level per cell, per the
   pin), refreshed whenever the LEVEL row changes. `AssetStore::stage_preview(n)`
   loads/caches each level's preview tiles + field swatch independently of the
   live match's `load_stage` slot (missing/broken art draws nothing, never
   crashes) so cycling levels never disturbs an in-progress match's own art.
3. Flow: Menu Play → `present_setup` → `present_map_select` → match. Esc backs
   up one step at each screen.

**Roster/level → match:** `setup_type_[i]` → `active[]`/`ai[]` (as before);
`setup_team_[i]` → `MatchConfig::team[]` → the hashed `Player::team` (see the
"Determinism / golden" update right below — this was config-only/non-hashed
when first written, no longer true); the committed level index → `start_match`
overrides the stage (specific level) or keeps `pick_stage` (RANDOM). The
committed win target → `win_target_` (best-of).

## Determinism / golden — UPDATE (2026-07-08): team mode landed, one-time recapture

At the time this doc was first written, `MatchConfig::team[]` was config-only
and not copied into any hashed `Player` field. That follow-up (tracked in
`docs/re/ai.md`) has since landed: `Player::team` is now a hashed field, copied
verbatim from `MatchConfig::team[]` at setup (`setup.cpp`), and gates AI
targeting (`docs/re/ai.md` §3.4/§5.3) and round-end ("one team left"). This grew
the hash layout by one word per player (`hash.cpp`), so `tests/test_golden.cpp`
needed a one-time constant recapture — see that file's own note and the commit
that landed `Player::team`. Every golden scenario leaves every slot's team at 0
(the default), so gameplay is byte-identical; only the digest layout shifted.
Roster/colour/level are still pure `MatchConfig`/presentation as described
above. The random glue pick uses a presentation LCG, never `State::rng`.

Sources: `sub_42B47D` (0x42B47D), `sub_42B0CE` (0x42B0CE), `sub_4124A4`
(getstring), `sub_42741E(0x410)` (music 1040), the glue-pick at ~17335,
`unk_4632CC` (404 B/player), VALUELST 16/750–778, MESSAGES.TXT ids 60/62/63/66/
70–73/80/95/100/110. Sample-block preview: `sub_406AA3` (0x406AA3, pseudo.c
7938-8041), called from `sub_406DDE` (pseudo.c 8145), VALUELST 730-733,
`dword_4648A0`/`dword_4648A4` (36/40 = tile cell pitch, pseudo.c 27013-27014,
== `sim::kTileH`/`kTileW`), `aFieldUPlt`/`aTileUS` string templates
(pseudo.c 1399-1402).
