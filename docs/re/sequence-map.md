# ANI sequence-name map — the complete request table

Audited 2026-07-09 against BM95.EXE's decompile (every `sub_41D957(name)`
sequence lookup and every `sub_4518D0`-composed name feeding one) and the
shipped install's ANI files (`abtool ani` dumps of `DATA/ANI/*`). Rationale:
several visual bugs shared one root cause — our port requesting a sequence
by a guessed name (or from the wrong file) while the original composes a
different one. This table pins the whole class at once.

## The original's loading model (why "which file" matters)

`sub_41D695` (0x41D695, boot) reads **`DATA/ANI/MASTER.ALI`** — a plain-text
manifest — loads every listed `.ANI`, merges ALL their sequences into ONE
global array (`dword_461B5C`), and sorts it by name (`qsort` + `stricmp`).
`sub_41D957` (0x41D957) then binary-searches that single pool by name; a miss
is fatal ("unable to find sequence '%s'"). Consequences:

1. A sequence name resolves to whichever LOADED file owns it — the file name
   itself never appears at a draw site.
2. An `.ANI` file that is not listed in MASTER.ALI (or is commented out with
   `;`) contributes NOTHING: same-named sequences in unlisted files are dead
   art. This is how TRIGBOMB/FLAME/PUNCH/BPICKUP fooled us — each is a
   plausible-looking file whose sequences the original never loads.

The shipped MASTER.ALI loads: kface, powers, **triganim** (`;-trigbomb`),
**mflame** (`;-flame`), walk, bombs (kept "for the jelly bombs"), stand,
tiles0-10, xbrick0-10, xplode1-17, corner0-7, kfont, hurry, kick, duds,
bwalk1-4, **pup1-4**, **punbomb1-4**, extras, conveyor, shadow, misc, edit,
**aliens1** (`;-bomanim2` also commented out).

## Truth table

Direction words come from `sub_413AED` = `off_45BCC4[godir & 3]` =
`{"north","east","south","west"}` (0x45BCC4). "we" = `libs/game`
(sequences.cpp unless noted). Status: MATCH = already correct before this
audit; FIXED = corrected 2026-07-09 in this audit's commit.

| Composed name (draw site) | Owning ANI (loaded set) | Our request | Status |
|---|---|---|---|
| `walk %s` (sub_41F29B 0x41F29B) | WALK.ANI `walk <dir>` | same | MATCH |
| `stand %s` (sub_41F29B) | STAND.ANI `stand <dir>` | same | MATCH |
| `spin` (strcpy, sub_41F29B warp states 6/7) | WALK.ANI `spin` | same | MATCH |
| `kick %s` (sub_41F29B) | KICK.ANI `kick <dir>` | same | MATCH |
| `punch %s` (sub_41F29B) | **PUNBOMB1-4.ANI** `punch <dir>` (one dir/file) | was `punch <dir> green` from PUNCH.ANI (dead art) | **FIXED** |
| `pickup %s` (sub_41F29B action-state 4, ~23397) | **PUP1-4.ANI** `pickup <dir>` (one dir/file) | was never requested (pose missing entirely) | **FIXED** |
| `walkbomb %s` / `standbomb %s` (sub_41F29B) | BWALK1-4.ANI (one dir/file) | same | MATCH |
| `cornerhead %u` (sub_41F29B states 20-39, %u = state−20) | CORNER0-7.ANI `cornerhead 0..12` | same | MATCH |
| `shadow` (sub_41F29B ~23249) | SHADOW.ANI `shadow` | same | MATCH |
| `die green %d` (sub_41F29B tail ~23457) | XPLODE1-17.ANI `die green <n>` | pool of all `die*` sequences (cosmetic pick) | MATCH |
| `kface %s` (sub_41F29B ~23272, gated `dword_45BE3C`) | KFACE.ANI `kface <dir>` | not requested | ORIGINAL-ONLY (see below) |
| `bomb %s green`, kind regular (sub_42331C ~25587) | BOMBS.ANI `bomb regular green` | same | MATCH |
| `bomb regular green` + `" dud"` strcat (dud state 2) | DUDS.ANI `bomb regular green dud` | same | MATCH |
| `bomb %s green`, kind trigger | **TRIGANIM.ANI** `bomb trigger green` (19 steps) | was TRIGBOMB.ANI's 7-step twin (dead art) | **FIXED** |
| `bomb %s green`, kind jelly | BOMBS.ANI `bomb jelly green` | was never requested — jelly bombs drew the regular pulse | **FIXED** |
| `bomb trigger green` (main-menu cursor, sub_42B9CE-area ~30775) | TRIGANIM.ANI | via the same trigbomb store slot | **FIXED** (source file) |
| `flame %s green` (sub_426D06 ~27421, %s from off_45BEA0) | **MFLAME.ANI** (5-step cycles) | was FLAME.ANI (dead art, 7-step cycles) | **FIXED** |
| `flame %s %u` = `flame brick <stage>` (sub_426D06 ~27408, kind 9) | XBRICK<n>.ANI `flame brick <n>` | same (`burn`) | MATCH |
| `tile %u solid` / `tile %u brick` (sub_425D22 ~26760, board) | TILES<n>.ANI | same | MATCH |
| `tile %d blank/solid/brick` (editor, sub_402206 5120-5145, %d = dword_45B7B8 ∈ {0,−1}) | TILES0.ANI (0) / **EDIT.ANI** (−1: `tile -1 blank/brick/solid`) | −1 was believed a dead state; now probes EDIT.ANI | **FIXED** |
| `tile %u %s` (setup sample block ~8027, random fill) | TILES<n>.ANI | stage_preview uses the same names | MATCH |
| `extra arrow %s` (sub_4056CA ~7237) | EXTRAS.ANI | same | MATCH |
| `extra warp %u` (~7273, %u = warp id, shipped = 1) | EXTRAS.ANI `extra warp 1` | same | MATCH |
| `extra conveyor %s` (~7287) | CONVEYOR.ANI | same | MATCH |
| `extra trampoline` (~7299) | EXTRAS.ANI | same | MATCH |
| `ghost %s` / `rover %s` (campaign mover ~4972-4977) | **ALIENS1.ANI** (8 seqs, ghost/rover × 4 dirs, 40x40, hot(20,39)) | was believed cut content (looked for GHOST/ROVER.ANI); drew plain colour markers | **FIXED** |
| `numeric font` / `infinity` (clock, ~14534/14516) | KFONT.ANI | same | MATCH |
| `xxx` (player-row dead marker, sub_420F07 ~23688) | MISC.ANI | same | MATCH |
| `hurry` (~29544) | HURRY.ANI | same | MATCH |
| `goldman` (twinkle, sub_420E39 ~23600) | MISC.ANI | same | MATCH |
| `ring` (goldman wheel ~6032) | MISC.ANI | same | MATCH |
| `teamring%u` (editor starts ~5549) | MISC.ANI `teamring0/1` | same | MATCH |
| `cursor1` (~16699, options cursor) | MISC.ANI | same (options_screen.cpp) | MATCH |
| `power %s` (floor tokens sub_4250DE ~26251, wheel icons sub_425C7F ~26730) | POWERS.ANI | same (audited separately, 2026-07-09 — no mismatch) | MATCH |

Counts: **24 MATCH, 7 FIXED** (punch source, pickup pose, trigger-bomb
source incl. menu cursor, jelly-bomb sequence, flame source, editor `-1`
tileset, ghost/rover art), **1 ORIGINAL-ONLY** (kface).

## Original-only sequence: `kface %s`

`sub_41F29B` ~23272: after the body blit, if this player's index equals
`dword_45BE3C`, compose `"kface %s"` (facing dir) and blit KFACE.ANI's frame
at `(x−4, y−34)` — a marker face floating above one designated player.
`dword_45BE3C` defaults to −1 (never drawn); it is written in exactly three
places: −1 at match init (~23986), and set/cleared from an input-side special
code in the getinput dispatcher (~22364/22369: code 74 sets it to that
player, 138 clears; also reachable via `sub_4226F6` from a network unpack
~12793). So it is a net/AI "highlight this player" marker, not part of a
normal local match. NOT ported; recorded here for triage.

## Dead art (shipped but never loaded — do not request from these)

Not listed in MASTER.ALI, and/or no composing draw site exists in the exe:

- **TRIGBOMB.ANI** (`;-trigbomb.ani`), **FLAME.ANI** (`;-flame.ani`),
  **BOMANIM2.ANI** (`;-bomanim2.ani`) — explicitly commented out.
- **PUNCH.ANI** (`punch <dir> green`), **BPICKUP.ANI** (`pickup <dir>
  green`), **BOMBWALK.ANI** (`bombwalk <dir> green` — no such format string
  in the exe at all), **BOMBANIM.ANI**, **CLASSICS.ANI** (`classic ...` —
  no literal anywhere), **POWERS1/POWERZ/POWBOTH/POWERSO/POWERSO2/POWERSOK**
  (powerup icon variants; POWERS.ANI is the loaded one), **NUCKBLOW/
  APPLBITE/ZEN.ANI** (single unnamed sequences), **HURRY.ANI is loaded**
  but note **HEADWIPE.ANI is NOT in MASTER.ALI** and has no filename literal
  in the decompile — our Transition's use of it as the screen-wipe overlay
  has no confirmed original draw site (flagged for triage; the fade
  fallback path is unaffected).
- **MISC.ANI `safe` / `scan`** — the file is loaded, but no draw site
  composes either name anywhere in the 1134-function decompile: dead
  sequences inside a live file.

## Port notes (what changed 2026-07-09)

- `AssetStore`: `flame_` now loads MFLAME.ANI; `trigbomb_` now loads
  TRIGANIM.ANI; new `punch_[4]` (PUNBOMB1-4), `pickup_[4]` (PUP1-4),
  `aliens1_` (ALIENS1.ANI), `edit_` (EDIT.ANI) slots, with per-player
  recolours for punch/pickup (their names carry no "green" but the art is
  the green master, recoloured like every player sprite; ALIENS1/EDIT are
  shared/uncoloured).
- `SequenceSet`: `punch` resolved as `punch <dir>` probing PUNBOMB1-4; new
  `pickup`, `bomb_jelly`, `ghost[4]`, `rover[4]` Anims.
- `Renderer`: jelly bombs draw `bomb_jelly` (trigger still wins — the kind
  is exclusive at creation, sub_41EB13); rovers/ghosts draw ALIENS1 art
  (plain-marker fallback kept for partial installs); new pickup-pose
  countdown driven by the `BombGrabbed` event (state 4's anim-length exit,
  sub_41F29B ~23396-23407), overriding the carry pose while it runs.
- `EditorScreen`: the `'0'` tileset toggle's −1 state now resolves EDIT.ANI's
  schematic tiles instead of falling back to flat swatches.

All render/asset-layer only: `libs/sim` untouched, no RNG draws added or
moved, golden hashes unchanged (37/37 suites green).
