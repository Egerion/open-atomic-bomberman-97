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
  29528-29529); that function (pseudo.c 4612-4651) runs a round-timeout
  countdown (`dword_4646C0`, compared against `2 * dword_46494C *
  getvalue(25)`) and, each tick, walks all 10 player slots via
  `sub_421DD2` — for any non-AI slot type it calls `sub_4228C4(i)` (early
  return on a live human check), then decrements `dword_4648B0` and forces
  `dword_464894 = 2` (a state/phase transition) once the round's done. This
  is genuine round-pacing logic specific to campaign play, not a no-op.
- **Gates round-loop exit/continuation.** Two more sites (pseudo.c 21936,
  24317) check `dword_46489C` alongside a player-slot AI-type test and the
  net-mode check `sub_40C06A()==1`, i.e. campaign mode is treated as a
  parallel "controlled" mode alongside netplay for round-continuation
  purposes (both keep looping under external/scripted control rather than
  the normal local win-condition path alone).
- **Roster/level auto-fill.** `sub_4015C6` seeds all 10 slots
  (`sub_42288C`) straight from the picked campaign's rover/ghost/AI counts —
  the player does not manually configure the roster on a campaign run; the
  `.CAM` record's fields 3-8 (rovers, rover speed, ghosts, ghost speed, AI
  count, AI difficulty) do that.

In short: campaign mode is a real, connected sequence of pre-scripted single-
stage matches (name + scheme + monster/AI composition per stage, read from a
`.CAM` file) that auto-advances through `dword_4648B0` stages, replacing the
normal manual level-pick and roster-pick screens for the duration. It reads
as a lightly-used developer/QA or hidden-bonus feature (no menu discovery
path, keyboard-mash trigger, only 3 shipped `.CAM` files with joke names —
"Just One Ghost", "Just One Dude" — rather than a marketed mode) but it is
functionally complete code, not a stub.

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
  hooks.
- `sub_4016DA` (pseudo.c 4612-4651) — per-tick campaign round-pacing,
  called from the round-tick callback `sub_42A191` @ 0x42A191 (pseudo.c
  29528-29529; registered via `sub_43A6FC` at 29701) while `dword_46489C`
  is set.
- `dword_46489C` (campaign-active flag) read sites: pseudo.c 4433, 4458,
  4527, 8087, 15315, 21936, 22877, 24051, 24317, 29528, 29776, 29793.
- String constants: `aTotalOfUCampai` (pseudo.c 1309), `aCouldnTOpenCam`
  (pseudo.c 1310), `aCam` (pseudo.c 1311), `aSSSCam` (pseudo.c 1452).
- Format cross-check: `DATA/RES/SIMPLE.CAM` (install), `docs/RE-NOTES.md`
  asset-format table (`.CAM` row).
- Distinguished from the unrelated "campaign easter egg" mislabel in
  `docs/re/frontend-flow.md` (main menu Ctrl+E×6 → map editor,
  `sub_40330E`), corrected in `docs/re/results-and-options.md` §5.
