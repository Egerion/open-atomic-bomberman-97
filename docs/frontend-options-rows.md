# The Options screen's 19 rows

`sub_4080DC` @0x4080DC — the interactive Options screen
(`libs/frontend/{include/bomber/frontend/options_screen.hpp,src/options_screen.cpp}`,
model in `options_model.hpp`). `docs/re/results-and-options.md` §3 is the RE
source; this page is the per-row disposition the port implements, moved out of
the header under `docs/coding-standards.md` §10 (a rationale longer than ~10
lines is a docs page with a one-line pointer).

## Layout and chrome

The CONFIRMED VALUELST block `745,55,40,22,500` gives x=55, y0=40, ystep=22,
W=500. **W is a text-clip width, not a colour** — `sub_41696C`'s 4th argument is
a width, confirmed by cross-referencing the same "id,X,Y,YS,W" column shape §1's
RESULTS-screen rows use. The backdrop is the random `GLUE<n>` convention
`present_setup` / `present_map_select` share (`docs/re/setup-screens.md`
"Backdrop", `sub_4148E5`).

Three corrections from the 2026-07-09 full re-read of `sub_4080DC` (pseudo.c
8914-9491), all of which reversed something the port had invented:

- **No title/header text draws anywhere in `sub_4080DC`'s body.** There is no
  getstring call before the row loop, and the row-3 dispatch in the caller
  (pseudo.c ~30910) does not wrap the call with a header draw either — unlike
  e.g. the key-remap sub-screen's own confirmed `getstring(1100)` header. The
  port's earlier "OPTIONS" text at a guessed (55,20) had no citation and was
  removed rather than kept. Open question rather than a closed one: the four
  external chrome primitives `sub_41043C` / `sub_415CA4` / `sub_415C1F` /
  `sub_429790` have no visible body in the decompile, so this cannot be proven
  to be the FULL picture — but nothing justifies a specific guessed title.
- **The selection indicator is a real cursor sprite, not a text recolour.**
  `sub_413BD6`'s body (pseudo.c 16691-16721) resolves the ANI sequence name
  `"cursor1"` (`aCursor1`) and blits it at (x-20, row_y) on its own self-paced
  frame timer (getvalue 690/691). `"cursor1"` lives in MISC.ANI, whose sequence
  table is confirmed `cursor1, goldman, ring, safe, scan, teamring0, teamring1`.
  This replaced a yellow-recolour + `"> "` prefix stand-in.
- **`sub_427961(20)` (the nav blip) is the only sound `sub_4080DC` ever plays**,
  unconditionally for any real keypress (pseudo.c 9298-9299 fires it whenever
  the raw key code is neither -1 nor -2). There is no separate "accept" jingle
  (SFX 10) anywhere in the function. The port's earlier `audio.play(10)` calls on
  Enter/Escape/"open key-remap" were invented.

## Every row is drawn, including the ones this port cannot act on

The 2026-07-09 audit also found the port's earlier "hide the 8 unwired rows"
choice was itself the bug: the original draws every row unconditionally, in the
SAME general ink (`byte_49D38F`, RGB(255,255,255) — §1's LUT decode) for EVERY
row including the selected one. There is no per-row colour distinction for
"wired" vs "not wired" or "selected" vs not. So every row is shown here too,
even the net/modem/legacy ones — hiding them was the invented deviation, not
showing them (CLAUDE.md's "no invented visuals").

## The row count is 18, and it is a literal

`kCursorRowCount = 18` is the literal at pseudo.c 9086, used verbatim by both
the Up-key underflow wrap and the Down-key overflow wrap.

**Retraction (2026-07-12, the chrome audit).** An earlier reading had this as
"19 rows are drawn but row 18 (Adjust Audio) is unreachable via Up/Down". That
was wrong. The draw loop contains exactly 18 `sub_41696C` row calls
(getstring 250 @9098 .. 267 @9281), and `getstring(268)` "Adjust Audio" is never
fetched anywhere in the binary. Only the DEAD dispatch `case 18` exists
(`sub_407542`'s "Audio Adjustment screen will be here..." stub). The 19th drawn
row was the port's invention and has been removed; `OptionRow::kCount` is 18.

## Per-row disposition

| row | label | disposition |
|---|---|---|
| 0 | Team Play | LIVE toggle, persisted `team_play=`. ALSO clears the pending Goldman winner (`dword_46492C=-1`, same as row 6) |
| 1 | Random Start | LIVE toggle, persisted `random_start=` |
| 2 | Node Name | LIVE (2026-07-25). Opens `sub_4074DC`'s text-entry prompt (`getstring(290)` "Enter new node name:", 30-char field) on Left/Right/Enter/Space, the same modal-push contract as rows 8/15. Persisted to install-root NODENAME.INI (`sub_40C08C` / `sub_40C140`), **not** options.ini — §3 is explicit that this is not one of the 22 keys |
| 3 | Conveyor Speed | LIVE cycle, persisted `conveyor_speed=` |
| 4 | Stomped Bombs Detonate | LIVE toggle, persisted `stomped_bombs_detonate=` |
| 5 | Win Matches By Kill Total | LIVE toggle, persisted `win_by_kills=`; forced off with Team Play (§3) |
| 6 | Gold Bomberman | LIVE toggle, persisted `goldman=`; also clears the pending Goldman winner |
| 7 | Enclosement Depth | LIVE cycle, persisted `enclosement_depth=` |
| 8 | Scheme File | LIVE. Opens the `*.SCH` picker (`sub_407582`) on all four of Left/Right/Enter/Space |
| 9 | Play Time | LIVE cycle over the confirmed `sub_4076FE` chain, persisted `playtime=` |
| 10 | Assign Keyboard Player | LIVE toggle, persisted `assign_keyboards=`. No distinct sim consumer (the setup screen's own KEYBOARD 0/1 slot picker covers it), but a real boolean with a real key, so it round-trips |
| 11 | Diseases Can Be Destroyed | LIVE toggle, persisted `diseases_destroyable=` |
| 12 | Lost net players revert to AI | LIVE toggle, persisted `lost_net_revert_ai=`; no consumer, round-trips like row 10 |
| 13 | Disable music during gameplay | LIVE toggle, persisted `disable_game_music=` |
| 14 | Modem: P/I/B/# | SHOWN, non-interactive. The original's own value formatting here is a single `%d` of modemport (pseudo.c 9249-9251), not the 4-field breakdown the label implies |
| 15 | Define keyboard layouts | LIVE: opens `KeyRemapScreen` (§2) |
| 16 | Set Default Network Protocol | SHOWN, non-interactive |
| 17 | Use Enhanced Memory Model | LIVE toggle, persisted `smallmemory=`. The label is INVERTED versus the backing value (the row fetches string id 25 plus one more when `dword_464824` is zero, pseudo.c 9280): `smallmemory==0` shows "YES", `==1` shows "NO" |

Row 17's value is not consumed by this port. The ORIGINAL does consume it —
`sub_42814B` forces the sound cull's `keep=1` for every range when it is set
(`docs/re/sound-engine.md` §3) — an arm this port deliberately does not mirror.

## Row 8's filename is shown verbatim

The row draw formats `byte_4648C4` as-is. The picker (`sub_407582`) stores the
name cut at the first **colon** (0x3A, CORRECTED 2026-07-26 — the earlier
reading said `'.'`) and uppercased (`sub_412A3B` strupr), so a picked value reads
"BASIC.SCH" while a hand-edited options.ini value shows however the file spells
it. The extension strip lives in the READER (`sub_403EEE` @0x403FE8), not here.

## Key dispatch

CONFIRMED 2026-07-09 (pseudo.c 9297-9406): the tail switches on the raw key
code, and the ONLY branch that raises the exit flag is 0x1B (Escape). Enter (13)
and Space (32) both jump to LABEL_29 — the EXACT SAME per-row switch Right
(0x14D) dispatches to. Left (0x14B) runs a second, separately-listed switch with
the same cases: toggles do the same toggle (direction ignored), cyclers
decrement, and every "opens a sub-screen" row dispatches the same open action.
So row 15 opens via all four of Left/Right/Enter/Space, not Enter-only.

The port's previous `done_ = true` default on Enter/Space had no such branch in
`sub_4080DC` at all — it was invented, and it was the reported "rows drop you
back to the main menu" bug.

## The key-remap sub-screen

Row 15 opens `sub_407B9D` (§2), rebuilt 1:1 on 2026-07-13 from a full body
re-read. `libs/frontend/{include/bomber/frontend/keyremap_screen.hpp,
src/keyremap_screen.cpp}`.

### Widgets

It brackets itself in the widget library's mouse-cursor show/hide pair
(`sub_431178`/`sub_431360` — the same pair the `sub_41485A` list dialogs use) and
creates 13 REAL bevel-button widgets via `sub_432298`:

- a 2×6 grid, id `1000*set + action + 1000`, at x = 320·set + 100, y = 60·action + 60
- id **999**, "Return to default keys", at (40, 430) (pseudo.c 8762-8763)

Clicks come back as widget ids from the same getkey queue (`sub_4102B7` returns
1000..2998 / 999). `sub_432998`, the widget-lib pump, shows a button's "down"
bitmap while the mouse is held INSIDE it (bevel pair swapped, unwashed face) and
fires its id on RELEASE inside.

The default mouse cursor is the widget library's own 8×8 bitmap `byte_45C310`,
read from BM95.EXE's data section through `sub_430E4C`'s colour remap: value 15 →
white `byte_49D38F`, 1 → `byte_497498` (LUT 0x2108 → (60,68,56)), 0 →
transparent. Hotspot (1,1), from `sub_430EDC`'s default-arm a5/a6.

### Chrome

All inks LUT-true (`docs/re/frontend-flow.md` "COLOR.PAL"):

- `getstring(1100)` header at (20, 20), clip 400, in `byte_49D37A` (252,248,88).
  **Retraction**: the old (400,20) header read the CLIP WIDTH as the x. The real
  x was recovered from the raw EXE bytes at 0x407BD0 after Hex-Rays lost it.
- under each grid button, `getstring(1140)` "Key: '%s'" at (x, y+22), clip 200,
  in `byte_49D38F` (240,248,252) — drawn ONLY while `(code & 0x7F) < 0x59`
  (pseudo.c 8743-8744), with the name from the original's own 89-entry
  scancode-name table (`dos_scancode.hpp`; the E0-extended arrows alias their
  numpad names).
- the capture modal (`sub_412E33`): a 360 × 8·fontheight window at y=200, WINZ
  9-patch, the `getstring(1105)` message centred at 1.5 lines in white, a `"%d%%"`
  readout at 3.5 lines in yellow (pinned at 0 for the whole wait), and the
  white-framed 300-px track at 5.5 lines.

The screen paints NO backdrop of its own — each frame restores the SAVED backdrop
store (`sub_415CA4` = memcpy from `dword_460BCC`), which holds the Options
screen's random GLUE picture (not its rows).

### Rebind capture

`sub_407AD9` waits 500 ms with the key queue flushed (`sub_413CB0(500)` +
`sub_41043C`), then polls the RAW 256-byte scancode state (`byte_4A2BA0`) until
any key is down. The 0..255 ascending scan keeps overwriting its result, so the
HIGHEST held index wins. The value is stored UNCONDITIONALLY into the binding
table `dword_4645BC` at index 10·set + action, with **no cancel branch**:
pressing Esc binds Esc. Only the tap-released-between-polls race can store 0 =
unbound, a timing artifact this port does not reproduce.
