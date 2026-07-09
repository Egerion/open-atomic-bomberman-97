# Coverage audit — RE + port completeness vs BM95.EXE

Snapshot at 2026-07-08 (worktree `worktree-agent-ae75baa4f26b0b60b`, after
merge with `main` @ `e265541`). Netplay is out of scope per ADR-0003 — listed
here only to mark the boundary, not as work to schedule.

Method: cross-referenced every `sub_XXXXXX` cited in `docs/re/*.md` (~230
unique addresses, `facts.md`/`ai.md`/`frontend-flow.md`/`in-match-shell.md`/
`results-and-options.md`/`setup-screens.md`/`stage-actors.md`/
`goldman-roulette.md`/`enclosure.md`/`player-colour.md`) against the 330
top-level function definitions in `pseudo.c`, the explicit gap/TODO/DEFERRED
statements already recorded in those docs, `docs/ROADMAP.md`'s own open
checkboxes, `grep -rn TODO libs/ apps/ tests/`, and the install's file-format
inventory vs `libs/assets` parsers. This file is a work-queue source, not
prose — keep entries terse and re-sort by priority as items close.

## Legend

RE status: **pinned** (facts.md entry + address + evidence) · **partial**
(address identified, some behaviour undocumented) · **unRE'd** (address
known to exist, no facts.md entry) · **N/A** (netplay/dead code, out of
scope).

Port status: **ported** (code + test) · **partial** · **absent** · **N/A**.

---

## 1. Gameplay-mechanic subsystems (libs/sim)

| # | Subsystem | RE status | Port status | Evidence | Next task | Priority |
|---|---|---|---|---|---|---|
| 1 | Movement/collision (`sub_41F29B`, `sub_41EC84`) | pinned | ported | facts.md "Player movement", `movement.cpp`, `test_move.cpp` | — | done |
| 2 | Bomb machine: kind/fuse/kick/flight/jelly/dud/trigger/goldflame (`sub_42331C`, `sub_41EB13`) | pinned | ported | facts.md, `bombs.cpp`, `test_jelly/dud/trigger_allowance/goldflame/kick_nuances.cpp` | — | done |
| 3 | Flame-arm stops at first target hit | **partial** | **absent** | ROADMAP "Known parked fidelity gaps ... flame-arm stops"; no facts.md entry beyond flame lifetime (10 frames, `sub_426d06`) | RE the per-tile flame propagation stop condition in `sub_42708D`/flame spread loop, then port + golden recapture | **high** — correctness gap in core flame mechanic |
| 4 | Flying-bomb (thrown/punched) landing on a powerup tile | **partial** | **absent** | ROADMAP "flying-bomb landing on powerups"; facts.md only covers flying-bomb spawn (`sub_424987`→`sub_41013F`) and the enclosure exemption | RE landing-tile occupancy test for flying bombs vs powerup tiles, port into `bombs.cpp` | **high** — visible bug class (bombs vanishing/stacking on powerups) |
| 5 | Scatter occupancy test (head-hit powerup scatter) | **partial** | **partial** | ROADMAP "scatter occupancy test"; facts.md "Head hit" covers the scatter RNG (`sub_4255B2`) but not a full occupancy/collision check against other scattered items | RE whether `sub_4255B2` checks prior scatter targets in the same call for collisions | medium |
| 6 | Powerup pickup dispatcher, mutual exclusions, random reroll | pinned | ported | facts.md, `powerups.cpp`, `test_random.cpp` | — | done |
| 7 | Disease system (9 diseases, contagion/cure/visual) | pinned | ported | facts.md "Disease system", `diseases.cpp`, `test_disease.cpp`, `test_stomped_diseases.cpp` | — | done |
| 8 | HURRY enclosement wall | pinned | ported | `docs/re/enclosure.md` (no open gaps stated), `enclosure.cpp` | — | done |
| 9 | Stage actors: conveyor/trampoline/dirarrow/warphole | pinned | ported | `docs/re/stage-actors.md` ("all four actor types IMPLEMENTED/VERIFIED"), `test_stage_actors/conveyor/trampoline.cpp` | — | done |
| 10 | Options toggles → sim (stomped-bombs-detonate, diseases-destroyable, random-start) | pinned | ported | facts.md "Options toggles", `test_options.cpp` | — | done |
| 11 | Team mode (sim side: `Player::team`, round-end side logic) | pinned | ported | ROADMAP "TEAM MODE, sim side — DONE 2026-07-08", `test_team.cpp` | — | done |
| 12 | AI — all 8 behaviours (`ai.c`/VALUELST 900-series) | pinned | ported | `docs/re/ai.md`, ADR-0005, `ai.cpp`, `test_ai.cpp` | — | done (Phase 2 complete) |
| 13 | AI: campaign "rover" mover (`sub_401B5C`) | pinned (address+role identified) | **N/A (out of scope until campaign)** | ai.md: "matters only if/when we port campaign monsters" | leave parked; revisit under Campaign item (§5) | low |
| 14 | AI: `sub_4245DA` column-guard comparand | **partial** | ported (best-effort) | ai.md: "not fully pinnable from pseudocode... safest interpretation pending a disassembler re-pin" | re-derive with IDA/disassembler pass instead of pseudocode-only reading, confirm or correct current port choice | medium — silent behavioural risk in a shipped AI path |
| 15 | Wall-slam SFX ids 140–146 | **unconfirmed call site** | ported (best guess) | facts.md: "UNCONFIRMABLE (no call site found)... port keeps existing mapping unconfirmed" | targeted disasm search for the call site, or accept as permanently unconfirmed and document as such explicitly (currently only in facts.md prose, not this table) | low |

## 2. Front-end / presentation subsystems (libs/game)

| # | Subsystem | RE status | Port status | Evidence | Next task | Priority |
|---|---|---|---|---|---|---|
| 16 | Boot flow (IPLOGO→HSLOGO→TITLE→menu), `sub_42B060`/`sub_42B9CE` | pinned | ported | frontend-flow.md, ADR-0004, `app_flow.hpp`, `test_frontend.cpp` | — | done |
| 17 | Attract mode boot-loop restart (no attract on boot) | pinned | ported | frontend-flow.md "boot flow is STRAIGHT-LINE" | — | done |
| 18 | Attract mode: menu-idle LIVE AI-only demo match | pinned | **absent** (parked) | frontend-flow.md "menu-idle attract match... documented gap"; ROADMAP "attract-mode demo match (partial work parked in a worktree; task was user-stopped)" | resume the parked worktree or restart: wire `dword_464938` attract flag equivalent, roster/level save-restore, AI-only dispatch, suppress DRAW/RESULTS/VICTORY screens | **high** — largest known fully-RE'd-but-unported feature |
| 19 | Main menu (7 rows, cursor, navigation) | pinned | ported | frontend-flow.md "main-menu items", `present_menu` | — | done |
| 20 | Results tail: DRAW/RESULTS/VICTORY tiers | pinned | ported | frontend-flow.md, ROADMAP "Multi-round best-of-N loop + RESULTS tally 1:1 — DONE 2026-07-08" | — | done — frontend-flow.md's older "port still DEFERRED" note fixed to match (§RESULTS tally tier) | done |
| 21 | VICTORY music using track 1020 instead of 1130 | pinned | **RESOLVED** | `game_app.cpp`: `kDrawMusicId` (1130) is started for DRAW, RESULTS, **and** VICTORY/TEAM (`audio_.start_music(kDrawMusicId)` in every outcome branch); `kWinMusicId` (1020) is scoped to the Play/setup path only. frontend-flow.md's "Results MUSIC" section and the 1020/1130 tunables rows updated to match — this audit's snapshot was stale on this row | — | done |
| 22 | `.BM` generic help browser (menu row 5 + in-round F1) | pinned | ported | ROADMAP "The generic .BM help BROWSER — DONE 2026-07-08" | — | done |
| 23 | Controllers/INPUT.BM menu-row binding | pinned | **absent** | frontend-flow.md: "no menu-row binding, documented gap" | wire INPUT.BM leaf to its real menu entry point (currently reachable only as a stub leaf?) — verify against `app_flow.hpp`'s `Controllers` state | low |
| 24 | Pre-match SETUP screens (player input type, level/rounds) | pinned | ported | setup-screens.md, ROADMAP "Pre-match SETUP screens... DONE 2026-07-05" | — | done |
| 25 | Net-game setup screens (`sub_42B0CE`, `sub_42B47D`) | pinned (addresses identified) | **N/A — netplay, excluded (ADR-0003)** | setup-screens.md: "explicitly NOT reproduced (kept for reference)" | none — out of scope | N/A |
| 26 | Player colour `.RMP` remap pipeline | pinned | ported | player-colour.md (no open gaps), `rmp.cpp`, `test_rmp.cpp` | — | done |
| 27 | Options screen (19-row `sub_4080DC`) | pinned | ported (most rows) | results-and-options.md, ROADMAP "Options screen + key-remap aligned to the RE — DONE 2026-07-08"; net/modem/memory rows explicitly omitted (netplay/dead HW, N/A) | — | done for in-scope rows |
| 28 | Key-remap UI (`sub_407B9D`) | pinned | ported | results-and-options.md, `KeyRemapScreen` | — | done |
| 29 | Goldman Roulette wheel | pinned | ported | goldman-roulette.md, ROADMAP "Goldman Roulette wheel — DONE 2026-07-08" | — | done, except prize id 13 (clogs) — see #30 | done |
| 30 | Goldman wheel prize id 13 (clogs, speed-penalty booby prize) | pinned | **explicit no-op / deferred** | goldman-roulette.md §3: "outside our 13-kind scheme space... decision deferred until the wheel screen itself is built" (wheel screen now IS built, per #29 — **this decision point is now actionable and stale**) | decide and implement (or explicitly reject) the clogs inventory-kind representation now that the wheel is live; `goldman_wheel.hpp` §8 still says "decision deferred" | medium — small scope, decision was blocked on a prerequisite that has since shipped |
| 31 | Hidden scheme/map editor (`sub_4028D2`) | pinned | ported | results-and-options.md, ROADMAP "Hidden scheme editor — DONE 2026-07-08" | — | done |
| 32 | Editor: Ctrl+B reset, '0' dead-tileset toggle, brush-preview-at-cursor, exact dialog chrome | pinned | **absent** | results-and-options.md: "Still NOT reproduced" | low-value polish pass on editor_screen.cpp/editor_grid.cpp | low |
| 33 | Editor: powerup sub-editor mouse-only interaction | pinned | ported (keyboard substitute) | results-and-options.md: "documented deviation" | none required — accepted deviation, note if mouse support is later added | low |
| 34 | In-round HUD (clock/warning ink/hurry flash/SFX 2700) | pinned | ported | in-match-shell.md ("a confirmed gap" note is now stale — HUD shipped per later ROADMAP "MM:SS clock HUD... reconciled") | **fix stale "confirmed gap" note in in-match-shell.md** | low (doc hygiene) |
| 35 | In-round debug/cheat keys (1/4/18/274-305/288) | pinned | **absent** (F1 only) | in-match-shell.md: "remain unwired except F1" | low priority — these are developer/QA keys, not player-facing; port only if debug tooling is wanted | low |
| 36 | Pause (Ctrl+Q forfeit, no real pause) | pinned | ported | in-match-shell.md negative finding ("NO pause exists") | — | done |
| 37 | Multi-round best-of-N + win_by_kills clinch | pinned | ported | ROADMAP "Multi-round best-of-N loop", "Kill attribution + win_by_kills — DONE 2026-07-08" | — | done |
| 38 | Faithful screen inks (RGB555 LUT) | pinned | ported | ROADMAP "Faithful screen inks — DONE 2026-07-08" | — | done |
| 39 | SDL3 gamepad support | pinned (`sub_421E80` cycle order) | ported | ROADMAP "SDL3 gamepad support — DONE 2026-07-08" | — | done |

## 3. Asset-format coverage (libs/assets vs install tree)

| Format | Extension | Count in install | RE status | Port status | Evidence | Next task | Priority |
|---|---|---|---|---|---|---|---|---|
| Sound list | `.RSS` | 2027 | pinned | ported | RE-NOTES.md, `rss.hpp` | — | done |
| Animation | `.ANI` | 95 | pinned | ported | RE-NOTES.md, `docs/formats/ani.md`, `ani.cpp` | — | done |
| Scheme (map) | `.SCH` | 67 | pinned | ported | RE-NOTES.md, `sch.hpp`, `test_sch_write.cpp` | — | done |
| Image | `.PCX` | 62 | pinned | ported | RE-NOTES.md, `pcx.hpp` | — | done |
| Text data (misc) | `.TXT` | 18 | pinned | ported | MESSAGES.TXT via `messages.cpp`; VALUELST.RES/SOUNDLST.RES are `.RES` (see below), not `.TXT` — confirm the 18 `.TXT` files are all covered (MESSAGES.TXT + READMEs, some non-gameplay) | spot-check remaining `.TXT` files aren't silently unparsed data | low |
| Palette remap | `.RMP` | 10 | pinned | ported | `docs/re/player-colour.md`, `rmp.hpp/.cpp`, `test_rmp.cpp` | — | done |
| BM help-screen | `.BM` | 10 | pinned | ported | `docs/formats/bm.md`, `bmscreen`/HelpBrowser | — | done |
| Resource list | `.RES` | 8 (incl. VALUELST.RES, SOUNDLST.RES, EXTRA*.RES) | pinned | ported | `reslist.hpp`, `extra.hpp`, facts.md, stage-actors.md (EXTRA*.RES actor registry) | — | done |
| Font | `.FON` | 3 | pinned | ported | `docs/formats/fon.md`, `bmfont.hpp`, `test_bmfont.cpp` | — | done |
| Misc data | `.DAT` | 3 (LEVELS.DAT, bmstats.dat, + 1 more) | **unRE'd** | **absent** | RE-NOTES.md: "🟡 Minor/low priority (fonts, key remaps)" — stale since FON/RMP are now done, but `LEVELS.DAT` itself was never RE'd | grep pseudo.c for `LEVELS.DAT` read site, determine purpose (level unlock state? campaign progress?), RE + parse only if it gates any reachable feature | low-medium — unknown purpose, verify it isn't load-bearing for something already "done" |
| Campaign | `.CAM` | 3 (CROUTON.CAM, GHOSTS.CAM, SIMPLE.CAM) | **pinned** (RE'd 2026-07-09) | **absent** | `docs/re/campaign.md` — reachability CONFIRMED: hidden 'C'×5 trigger on the local player-setup screen (`sub_410F81` → `sub_4015C6` picker → `sub_401085` loader → `dword_46489C` flag read at ~12 sites: level-select skip, stage auto-advance, round pacing, roster auto-fill). The "may be vestigial" hedge is resolved — feature is fully wired, just undiscoverable (easter-egg trigger, 3 joke-named `.CAM` files). Format confirmed trivial text (9 comma fields, `-C` line marker) matching RE-NOTES.md | port when wanted: `.CAM` parser in `libs/assets` (MESSAGES.TXT-tier), 'C'×5 trigger + picker + stage-sequencer above `libs/match` (no sim impact) — see campaign.md "Port implication" | low — small hidden feature, but now a real port item, not a stub |
| Palette | `.PAL` | 1 (COLOR.PAL) | pinned | ported (via PCX palette loading) | RE-NOTES.md | — | done |
| Bitmap | `.BMP` | 1 | **unchecked** | **unchecked** | not mentioned in any doc; likely a tool/icon asset, not gameplay data | identify the single `.BMP` file's role (icon export?), likely no action needed | low |
| Icon | `.ICO` | 1 (BM95.ICO) | N/A | N/A | application icon, not game data | none | N/A |
| Executable/DLL | `.EXE`/`.DLL` | 9 / 2 | N/A | N/A | tools (MAKECFG.EXE, decompile tooling), not game assets | none | N/A |
| IDB | `.IDB` | 2 | N/A | N/A | IDA database, explicitly excluded from repo per CLAUDE.md | none — must never be committed | N/A |
| Config | `.INI` | 4 | pinned | ported | `options.ini` via `load_options`/`save_options`; CFG.INI/nodename.ini are net/legacy config — confirm not needed | verify CFG.INI/nodename.ini are netplay-only before ignoring outright | low |

**Format doc gap (structural, not a missing parser):** `docs/formats/` only
has `ani.md`, `bm.md`, `fon.md`. PCX/SCH/RES-list/RSS/RMP/VALUELST are all
*parsed* (code exists, tests exist) but have no standalone `docs/formats/*.md`
writeup — the knowledge lives only in facts.md prose and code comments. Not
blocking, but inconsistent with the architecture note in CLAUDE.md ("formats
in `docs/formats/`"). Low-priority documentation debt.

## 4. Netplay boundary (ADR-0003 — excluded, not a gap)

Confirmed via `grep -n "socket\|ioctlsocket\|bind(" pseudo.c`: the transport
layer lives in the `sub_43Bxxx`–`sub_43Cxxx` cluster (Winsock, `WSOCK32.dll`
import). No DirectPlay. Functions catalogued as network-only across the RE
docs and explicitly NOT ported:

- `sub_42B0CE`, `sub_42B47D` — net-game setup screens
- `sub_40C06A` — network-mode checks (also shared with non-network callers;
  the non-network branches ARE ported, only the net branch is excluded)
- `sub_40FB44`, `sub_40FB90`, `sub_40CE27`, `sub_40FCE6` — network protocol
  calls
- `sub_40C678` — network stats dump
- `sub_43BA06`–`sub_43C668` region — socket/IPX/modem transport primitives

This matches ADR-0003's decision precisely: sim stays deterministic/pure so
lockstep netplay is possible *later*, but no netcode is written now. Nothing
to schedule here; listed for completeness so the audit's "unRE'd" counts
below aren't misread as gaps.

## 5. Known non-gameplay parked items (ROADMAP, verbatim carry-forward)

- Campaign mode — "Campaign later" (see asset-table `.CAM`/`.DAT` rows above).
  UPDATE 2026-07-09: reachability confirmed, NOT vestigial — the loading code
  is thin but fully wired behind a hidden 'C'×5 trigger; full chain in
  `docs/re/campaign.md`. Stays parked as a small low-priority port item.
- Random Start Options-row wording — intentionally left unguessed until RE'd
  (per ROADMAP note under Interactive Options screen); superseded — random
  start IS now RE'd and ported (facts.md "Options toggles", `random_start=`).
  **Stale ROADMAP phrasing, not an open gap** — worth a ROADMAP cleanup pass.

## 6. Documentation staleness found during this audit — FIXED

All five spots below have been corrected in place (worktree
`worktree-agent-a2dd02d9c78d8e344`); kept here as a record of what was stale
and what superseded it.

1. `CLAUDE.md` "Currently the only known guess: fuse pause while a bomb is
   airborne" — was **stale**. `facts.md`'s "Still guessed" table shows this
   resolved 2026-07-03; the table is now empty. **Fixed**: the note now says
   the guess was confirmed against `sub_42331C`.
2. `docs/valuelst-map.md` "Not in VALUELST — our own tunables: Flame linger
   duration (we use 10)... corner-assist threshold (we use 900)" — was
   **stale**. `facts.md` confirms flame lifetime = 10 frames from
   `sub_426d06` and states the corner_threshold guess was deleted in favor of
   `sub_41EC84`'s per-pixel resolution. **Fixed**: section reworded to state
   both are confirmed/resolved, no open tunables remain there.
3. `docs/re/frontend-flow.md` "RESULTS tally tier — RE'd, port still
   DEFERRED" — was **stale**, superseded by ROADMAP's later "Multi-round
   best-of-N loop + RESULTS tally 1:1 — DONE 2026-07-08". **Fixed**: section
   now reads "port DONE"; the adjacent "Results MUSIC" paragraph and the
   1020/1130 tunables-table rows (which carried the same VICTORY-music
   staleness as gap #4 below) were corrected in the same pass.
4. `docs/re/in-match-shell.md` "HUD ... a confirmed gap" — was **stale**,
   superseded by ROADMAP's "MM:SS clock HUD ... reconciled" (also DONE
   2026-07-08). **Fixed**: section now reads "DONE — no longer a gap", citing
   `game_app.cpp`'s clock/warning-ink/hurry-flash/SFX-2700 implementation;
   the cross-reference section's stale "deferred" phrasing was fixed too.
5. `libs/game/include/bomber/game/goldman_wheel.hpp` §8 "decision deferred
   until the wheel screen itself is built" — the wheel screen **is now
   built** (ROADMAP "Goldman Roulette wheel — DONE"), so the deferral's own
   precondition had been met; this was an actionable decision, not a blocked
   one (see table row #30). **Fixed**: comment reworded to say the
   precondition is met and the decision is open/actionable, not resolved
   (the clogs mapping itself is still `PowerupType::None` — this was a
   doc-only fix, not a feature implementation). `docs/re/goldman-roulette.md`
   §8 updated to match.

None of these were gameplay bugs — they were doc/comment lag behind later
ROADMAP entries in the same repo.

Note: `docs/re/facts.md` was intentionally left untouched by this pass (a
concurrent task was working near it) even though it is cited as the
superseding source for items 1 and 2 above.

---

## Summary counts

Counting the 39 numbered subsystem rows (§1+§2) + the asset-format rows in
§3 that represent a distinct format (14 formats, excluding pure-tooling
extensions marked N/A):

- **Covered (RE pinned + ported, "done"):** 31 subsystem rows (includes #21,
  VICTORY music — RESOLVED, see §6/top-open-items update), 11 asset
  formats — the large majority of 1:1 gameplay and front-end fidelity.
- **Partial (RE pinned/partial, port absent or partial):** 7 subsystem rows
  (flame-arm stops, flying-bomb-on-powerup, scatter occupancy, attract-mode
  demo match, `sub_4245DA` comparand, wall-slam SFX, editor chrome polish),
  2 asset formats (LEVELS.DAT, .CAM/campaign).
- **Explicit no-op / decision-deferred (now actionable):** 1 (Goldman wheel
  clogs prize).
- **N/A / excluded (netplay per ADR-0003, or non-gameplay tooling):** ~9
  functions/screens + several tooling file extensions.
- **Doc staleness (no code gap, just needs a note fixed):** 5 items (§6) —
  all fixed by this pass.

## Top open items, priority order

1. **Attract-mode live AI demo match** (§2 #18) — fully RE'd, largest single
   unported feature, work was already started once and parked.
2. **Flame-arm stop condition** (§1 #3) — core mechanic correctness gap, no
   RE yet.
3. **Flying-bomb landing on a powerup tile** (§1 #4) — core mechanic
   correctness gap, no RE yet.
4. ~~**VICTORY music track bug** (§2 #21)~~ — **RESOLVED**: `kDrawMusicId`
   (1130) already plays under DRAW/RESULTS/VICTORY in `game_app.cpp`; this
   audit's snapshot was stale on that row (fixed above).
5. **Goldman wheel clogs prize (id 13)** (§2 #30) — decision was blocked on
   the wheel screen shipping; it has, so this is now unblocked and small.

Everything else in §1/§2 is either done, explicitly out of scope (netplay),
or low-priority polish (editor chrome, debug keys, doc staleness).
