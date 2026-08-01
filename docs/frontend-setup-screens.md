# The pre-match screens

`SetupScreen` and `MapSelectScreen` (`libs/frontend/src/{setup_screen,
map_select_screen}.cpp`) — `sub_410F81` @0x410F81 and `sub_406DDE` @0x406DDE.
`docs/re/setup-screens.md` is the RE source and `docs/re/network-screens.md` §7
the online half; this page holds the port-side rationale that used to sit inline
(`docs/coding-standards.md` §10).

## PLAYER INPUT TYPE SELECTION (screen 1)

A random `GLUE<n>` backdrop under the 1020 track, header `getstring(50)`, and the
10-slot list: each slot's input type via `getstring(220..224)`, prefixed by
`getstring(51)` "Player %u" and tinted with the slot's intrinsic colour
(VALUELST 200-247 — there is no colour picker; colour is fixed per slot index),
plus a team marker when the slot's team flag is set.

Keys (`docs/re/setup-screens.md`): Up/Down pick a slot, Right cycles its type
(OFF→CPU→KBD0→KBD1→OFF), Left/'0'/'o' set it OFF, 'T' toggles its team, Enter or
Space goes on to the LEVEL screen, Escape cancels to the menu. Presentation only.

**This screen starts no music.** The 1020 track is already running, started by
its caller `sub_42A3F6` at 0x42A436. The screen used to restart it, which
restarted WIN.RSS from the top whenever the goldman wheel ran first — the wheel
had the identical bug (`docs/re/sound-engine.md` §9).

### The TEAM default, corrected twice

**First correction (2026-07-09.)** `sub_410F81` unconditionally calls
`sub_4046CC()` first thing, which (CD present) calls `sub_403EEE()`, which itself
unconditionally calls `sub_4049C0()` before anything else. `sub_4049C0` sets
`dword_46481C[12*j+8] = j & 1` for j in [0,10) — every slot's TEAM byte resets to
an ALTERNATING 0/1 pattern by slot parity every time this screen loads, not to a
flat 0.

Getting that wrong (old behaviour: every slot defaulted to 0) meant Team Play ON
without anyone pressing 'T' put every player on the SAME side: everybody got the
team-1/WHITE 0.RMP override instead of half going red, and `sides_remaining()`
read ≤1 from tick 0, clinching the round instantly.

**Second correction (2026-07-28)** — and the scheme OVERRIDES the parity default,
per slot (`docs/re/facts.md` "The .SCH -S row's 4th field is the per-slot TEAM").
The paragraph that stood in the code claimed `sub_403EEE`'s parse loop "only ever
overwrites a slot's COLOUR from disk, never TEAM, unless a rare 5-field profile
line is present — a hidden colour-profile file this port doesn't implement".

All three parts of that were wrong: the dwords it called colour are the spawn X
and Y, the file being parsed is the scheme itself, and a four-field
`-S slot,x,y,team` row appears in 19 of the 67 shipped schemes (E_VS_W, N_VS_S,
TENNIS, VOLLEY, … — the two-sided maps). The reader stores that field and its
tail loop pushes every slot into the player record's +84 byte via `sub_422437`,
so the map author's layout is already in place before this screen draws a frame.
The 'T' key still overrides it, exactly as in the original, where the key loop
runs after the load.

### Escape and campaign state

Escape blips, plays the accept sting, forfeits any pending gold player
(`dword_46492C = -1`) and clears campaign state.

**Campaign-quit semantics are a CONFIRMED NEGATIVE** (`docs/re/campaign.md`
"Campaign-exit key"): grepping every read/write of `dword_46489C` in the binary
finds it written in exactly TWO places total (`sub_4015C6`'s `=1` and
`sub_42A3F6`'s own entry `=0`, pseudo.c 29692). There is NO key anywhere, Escape
or otherwise, that explicitly clears it. The original's Escape-on-setup just
aborts the current `sub_42A3F6` call to the menu (`dword_464A68=2`);
`dword_46489C` is left stale until the NEXT "Play" click resets it at entry,
which is behaviourally invisible because that stale value is never read before
being overwritten.

The port's explicit clear produces the identical observable outcome (back at the
menu, campaign not running) via an immediate reset instead of an implicit one — a
faithful convenience, not a guess. It only fires if a `*.cam` pick from THIS visit
has not been confirmed into a running match yet; an in-progress campaign is
abandoned via the match loop's own Esc/Ctrl+Q, which likewise does not touch
`campaign_active`.

### The hidden 'C'×5 campaign trigger

`sub_410F81` pseudo.c 15357-15365. Three things about it were wrong in the port:

- **The blip comes FIRST, always.** `sub_410F81`'s key loop fires
  `sub_427961(20)` @0x411724 for every real key BEFORE its dispatch switch at
  0x411729 — 'c' (0x63) is just another case in that switch (it lands at
  0x41186D). The port's early exit used to jump the queue and swallow the blip.
- **The count is CUMULATIVE, not consecutive** (pseudo.c 840, 1193-1197): ONLY
  the 'C' handler touches the counter — no other key resets it — so 5 total 'C'
  presses across the visit arm the picker. The old any-other-key reset required 5
  CONSECUTIVE presses, which the original never demanded.
- **There is NO accept sting.** This trigger is not the menu's Ctrl+E×6 editor
  trigger it was written to mirror: that one really does play 10 (0x42BD50, right
  before `sub_40330E`), but the campaign arm at 0x411882 calls `sub_4015C6` and
  zeroes its counter with nothing in between. The 20 blip is the only sound five
  C presses make.

**LOCAL ONLY**, and the guard is SILENT: 0x41186D tests `sub_40C06A()` and, when
it is non-zero, jumps straight to the loop tail (0x41188E) with no sound at all.
The SFX-40 buzz that used to be there was invented. A campaign roster/stage pick
is a local-only concept that would never reach the peer.

### Slot rows

One combined `getstring(51)` "Player %u: %s" splice — the original sprintf's the
slot number and the type text in ONE call (pseudo.c 15169-15195); the old
two-piece concat left a literal "%s" on screen with the install's real
MESSAGES.TXT.

Ink is the slot's authentic colour via `sub_41672F(i)` (the .RMP tail quantised
`min(c/3,31)` → RGB555 → LUT). CONFIRMED never the team red/white override:
`sub_410F81` saves and zeroes `dword_464964` around this lookup (pseudo.c
15191-15204), so only the separate TEAM marker is team-inked. And the ink is
NEVER state-dimmed or selection-boosted — no OFF/COM dimming exists, and the old
selected-row +70 nudge was invented. The cursor sprite alone marks the selection.

Outline is black for every slot EXCEPT index 1: the BLACK player's row gets a
WHITE outline (`sub_416867`, pseudo.c 18496-18503) so it stays legible over a dark
glue backdrop.

The team marker (`getstring(230)`) is drawn for EVERY slot whenever Team Play is
on — gated on the GLOBAL `dword_464964` (pseudo.c ~15212), NOT on this slot's own
team byte. CONFIRMED unformatted (no sprintf before the two `sub_4124A4(230)`
reads at ~15221/15223): the COLOUR alone tells the teams apart, via
`sub_4141F8(team)` — team byte ≠ 0 → `byte_49D0DA` red (252,80,80), else
`byte_49D38F` white.

### The joystick pane

`getvalue` 715/720: heading msg 40, then one line per detected stick (msg 41
"Joy %u - %s", the stick's own name in the %s — `sub_429A61(i)`) or, if none, the
single msg-42 line. ALL of it plain white ink over a black outline (pseudo.c
15227-15263) — the old grey (200,200,200)/(150,150,150) tints were invented.

### Start guards

- **Guard 1** (`sub_42223E`, batch_0x410401.cpp 1148-1168): at least two ACTIVE
  slots, or in TEAM mode at least two DISTINCT team values among the active slots.
  Failure pops `getstring(46)` (solo) or `getstring(48)` (team) over
  `getstring(96)`.
- **Guard 2** (`sub_422085`, 1172): two ACTIVE HUMAN slots may not share the same
  input type AND sub-index — the same keyboard set or the same stick. Failure pops
  `getstring(45)`. Checked FIRST.

Both raise a `sub_414340` WINZ-9-patch acknowledge box in `byte_49A390` dark red
(164,0,0) — batch_0x410401.cpp:1161/1175, NOT white — dismissed by
Enter/Space/Escape with a nav blip on any key, drawn over the frozen setup frame.

The accept path also has a 1 s debounce (mirroring the LEVEL screen's, 8100 and
8220-8228) so a held Enter carried from the previous screen cannot blast the
start.

## LEVEL & ROUNDS (screen 2)

A 2-row list on a random `GLUE<n>` backdrop (1020 inherited): row 0 = LEVEL
(-1 RANDOM, else 0..10 of `getvalue(35)`=11 built-ins, named `getstring(150+n)` /
`getstring(149)`); row 1 = NUMBER OF WINS (1..100). Left/Right cycle the
highlighted row's value (level wraps [-1..10]; wins ±1, or ±5 on PgUp/PgDn —
batch_0x405B3A.cpp 1097-1128, codes 372/371, rebound from an invented
Ctrl+Left/Right the original never used). Up/Down switch rows. Enter commits;
Escape backs out.

**WORKING COPIES.** `sub_406DDE` 8092-8093 seeds `dword_45E0B8`/`dword_45E0B4`
from the committed globals on entry; edits touch only those, Enter/Space commits
them (LABEL_101, 8261-8271) and Escape DISCARDS them. The port's old in-place
member edits leaked cancelled changes into the next visit.

**Escape aborts the WHOLE Play flow**, not one screen. `sub_406DDE`'s Esc handler
(pseudo.c 8186-8191) is called straight from `sub_410F81`'s TAIL (pseudo.c 15516,
gated `if (!dword_464A68)`) with NO loop back to the player screen afterwards —
exactly like the Goldman wheel's own Esc. It also forfeits any pending gold
player.

### The guest is read-only, with no carve-out

CORRECTED 2026-07-28. The old comment claimed "Up/Down (pure navigation) and F1
stay live on a guest; every value-changing key buzzes". `sub_406DDE` has no such
carve-out: EVERY arm of its dispatch opens with the same `sub_40C06A() == 1` test
and the same SFX-40 buzz — Up (0x4071A5), Down (0x4071D6), the 0x174 stepper
(0x407209), Right (0x4072B3), Left (0x407355), Enter/Space (0x4073F4), F1
(0x407490) and Alt+D (0x4074AD). The ONLY ungated key is Escape (0x407464), which
sets its flags and leaves silently. The host owns this screen completely; a guest
can look and leave.

A guest also shows the level LABEL the host has on screen, taken verbatim off the
wire (`SetupPreviewFrame::level_name`) rather than re-resolved from this install's
`getstring(150+n)` — so a custom map the guest does not have still reads correctly
instead of showing the wrong name or a blank.

The guest commits the WIN TARGET as well as the level when the host confirms. The
win target is not part of the confirmed `MatchConfig` (it is a front-end
match-scope value, `dword_464A7C`, broadcast as its own kind-44 message), so
without that commit the guest ran the host's board with ITS OWN stale target and
the two peers disagreed about when the match was over — one starting round N+1
while the other showed VICTORY.

### The sample-block preview

`sub_406AA3` (level & rounds audit 2026-07-12). Border fill = the general WHITE
`byte_49D38F` (240,248,252) — NOT the old invented (40,40,60) — at (378,80,
224x202). The field swatch is a 1:1 CROP of FIELDn.PCX starting 48 rows down: the
source is taken 12·640 int-sized steps into the 640-byte-wide bitmap, 4 bytes a
step, so 48 scanlines. It is NOT a stretch — `sub_4428B4` is a plain rect copy —
220x198 at (380,82), which lines the backdrop's own board grid up under the drawn
tiles. Then the 5x5 solid/brick grid at native 40x36 cells.

The pattern is re-rolled only on screen entry and on a LEVEL row change
(`sub_406AA3` re-arms its own roll flag), NEVER every frame. Solid/brick-ness is
re-derived at draw time from the (col&1, row&1) parity rule — pure geometry that
does not need rolling. What is rolled is WHICH cells carry art and which level's
tileset each drawn cell uses, and for a RANDOM level that pick is re-made PER
CELL (a pinned quirk).

### Both screens' shared chrome

- Row text goes through the 4-pass-outline primitive (`sub_41696C`) in the general
  white ink `byte_49D38F` (240,248,252 — LUT 0x7FFF → idx 72), clipped to the
  VALUELST row's column-3 width. **Column 3 is a CLIP WIDTH, not a colour**;
  setup-screens.md's earlier "colour" label for it was wrong, and colour never
  comes from VALUELST here. The original marks the active row with the cursor
  sprite ALONE — the old per-row colour highlight and grey key legend were
  invented.
- The footer is `sub_413FB9` → `getstring(330)`, centred via `sub_4172BA`'s
  `x = cx - (w+2)/2`, in cyan `byte_497F8F` (96,252,252).
- The row cursor is `sub_413BD6`'s MISC.ANI "cursor1", hotspot-anchored at
  `(getvalue(row list) - 15, row_y + 16)` on the setup screen and `-20` on the
  options/level screens. **The +16 is pinned EMPIRICALLY** from a 1:1 native
  capture of the level screen (2026-07-12; VALUELST 736 = 170, measured sprite
  rows 155..186 → anchor = row_y + 16): the dude's feet stand just under the row
  text's baseline. The decompile loses the +16 to register mangling at every call
  site, so the capture is the authority.
- F1 dispatches the SAME generic `*.BM` help browser as menu row 5, the options
  screen and the in-round key — one routine, `sub_41431C`, composited over
  whichever screen is current (`sub_410F81` 15432-15436).
- The F2 chat overlay is PORT-ONLY, not RE'd. Open, it consumes every key so
  typing never also cycles a slot or a level behind it; closed, it takes only F2
  and the screen behaves exactly as it did before chat existed.
