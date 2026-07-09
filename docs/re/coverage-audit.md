# Coverage audit — RE + port completeness vs BM95.EXE

Snapshot at 2026-07-09 (worktree `worktree-agent-afc56c379215a828e`, after
merge with `main` @ `fd811d5`, "Merge rover/ghost hazard actors"). Refreshes
the 2026-07-08 snapshot: attract-mode demo match, the Goldman-wheel clogs
booby prize, campaign mode (parser/trigger/flow/banner/AI-seeding), and the
campaign rover/ghost hazard actors all shipped since then. Netplay is out of
scope per ADR-0003 — listed here only to mark the boundary, not as work to
schedule.

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
| 3 | Flame-arm stops at first target hit | pinned | ported | facts.md "Flame-arm stops — CONFIRMED (`sub_42331C` per-direction arm loop)"; landed in "Merge fidelity gaps" (5370bbf) | — | done |
| 4 | Flying-bomb (thrown/punched) landing on a powerup tile | pinned | ported | facts.md "Flying-bomb landing on powerups — CONFIRMED (`sub_42331C` flight-landing check)"; landed in "Merge fidelity gaps" (5370bbf) | — | done |
| 5 | Scatter occupancy test (head-hit powerup scatter) | pinned | ported | facts.md "Scatter occupancy test — CONFIRMED (`sub_4255B2` re-roll predicate)"; landed in "Merge fidelity gaps" (5370bbf) | — | done |
| 6 | Powerup pickup dispatcher, mutual exclusions, random reroll | pinned | ported | facts.md, `powerups.cpp`, `test_random.cpp` | — | done |
| 7 | Disease system (9 diseases, contagion/cure/visual) | pinned | ported | facts.md "Disease system", `diseases.cpp`, `test_disease.cpp`, `test_stomped_diseases.cpp` | — | done |
| 8 | HURRY enclosement wall | pinned | ported | `docs/re/enclosure.md` (no open gaps stated), `enclosure.cpp` | — | done |
| 9 | Stage actors: conveyor/trampoline/dirarrow/warphole | pinned | ported | `docs/re/stage-actors.md` ("all four actor types IMPLEMENTED/VERIFIED"), `test_stage_actors/conveyor/trampoline.cpp` | — | done |
| 10 | Options toggles → sim (stomped-bombs-detonate, diseases-destroyable, random-start) | pinned | ported | facts.md "Options toggles", `test_options.cpp` | — | done |
| 11 | Team mode (sim side: `Player::team`, round-end side logic) | pinned | ported | ROADMAP "TEAM MODE, sim side — DONE 2026-07-08", `test_team.cpp` | — | done |
| 12 | AI — all 8 behaviours (`ai.c`/VALUELST 900-series) | pinned | ported | `docs/re/ai.md`, ADR-0005, `ai.cpp`, `test_ai.cpp` | — | done (Phase 2 complete) |
| 13 | AI: campaign "rover"/"ghost" mover (`sub_401AAE`/`sub_401B05` spawn, `sub_401B5C` per-tick mover) | pinned | ported | `docs/re/campaign.md` "Rover/ghost/AI roster — CORRECTED", `libs/sim/src/systems/rovers.cpp/.hpp` (wander AI, human-avoidance bias, flame-death + kill-score, landing-tile player kill), hashed `State::rovers`, `tests/test_rovers.cpp`; wired via `simulation.cpp`, rendered (`renderer.cpp`), campaign pacing clause 1/3 consumed in `game_app.cpp` | — | done |
| 14 | AI: `sub_4245DA` "column-guard" comparand | pinned | ported | ai.md §9.3 RESOLVED (2026-07-09, raw-disasm re-pin): comparand = max-bombs byte +86, and `sub_4245DA` counts the actor's OWN live bombs (owner word at bomb +62), not bombs-in-column; port CORRECTED to the spare-capacity gate (`ai.cpp` behaviours 3/4, `test_ai.cpp` pins) | — | done |
| 15 | Wall-slam SFX ids 140–146 | pinned | ported (fixed) | facts.md "Wall-slam SFX — CONFIRMED (2026-07-09, `sub_426818`/`sub_4278F2`)": call site is the enclosure stepper's drop loop, `dword_462244 = rand()%3` drawn ONCE per arm; SOUNDLST.RES's own "hard-coded to play one of the three below" comment corroborates 140/141/142-only, 143-146 dead. `SoundDirector`/`AudioEngine::roll` fixed to latch one id per round instead of re-picking per drop | — | done |

## 2. Front-end / presentation subsystems (libs/game)

| # | Subsystem | RE status | Port status | Evidence | Next task | Priority |
|---|---|---|---|---|---|---|
| 16 | Boot flow (IPLOGO→HSLOGO→TITLE→menu), `sub_42B060`/`sub_42B9CE` | pinned | ported | frontend-flow.md, ADR-0004, `app_flow.hpp`, `test_frontend.cpp` | — | done |
| 17 | Attract mode boot-loop restart (no attract on boot) | pinned | ported | frontend-flow.md "boot flow is STRAIGHT-LINE" | — | done |
| 18 | Attract mode: menu-idle LIVE AI-only demo match | pinned | ported | frontend-flow.md table (this row, "Merge attract-mode demo match" 96be2e4): `attract_` flag (`dword_464938` equivalent), `input.hpp`'s `attract_computer_count`/`fill_attract_roster`/`attract_stage_pick` helpers, roster/level/team save-restore (`GameApp::AttractSaved`), AI-only dispatch bypassing the goldman wheel/setup/map-select screens, any key/mouse/pad input aborts immediately, DRAW/RESULTS/VICTORY suppressed | — | done |
| 19 | Main menu (7 rows, cursor, navigation) | pinned | ported | frontend-flow.md "main-menu items", `present_menu` | — | done |
| 20 | Results tail: DRAW/RESULTS/VICTORY tiers | pinned | ported | frontend-flow.md, ROADMAP "Multi-round best-of-N loop + RESULTS tally 1:1 — DONE 2026-07-08" | — | done — frontend-flow.md's older "port still DEFERRED" note fixed to match (§RESULTS tally tier) | done |
| 21 | VICTORY music using track 1020 instead of 1130 | pinned | **RESOLVED** | `game_app.cpp`: `kDrawMusicId` (1130) is started for DRAW, RESULTS, **and** VICTORY/TEAM (`audio_.start_music(kDrawMusicId)` in every outcome branch); `kWinMusicId` (1020) is scoped to the Play/setup path only. frontend-flow.md's "Results MUSIC" section and the 1020/1130 tunables rows updated to match — this audit's snapshot was stale on this row | — | done |
| 22 | `.BM` generic help browser (menu row 5 + in-round F1) | pinned | ported | ROADMAP "The generic .BM help BROWSER — DONE 2026-07-08" | — | done |
| 23 | Controllers/INPUT.BM menu-row binding | pinned | **N/A — confirmed negative** | frontend-flow.md "INPUT.BM menu-row binding — CONFIRMED NEGATIVE (2026-07-09)": no "controller"/"INPUT.BM" string anywhere in pseudo.c; INPUT.BM is just one of ~10 topics in the generic `*.BM` help browser (row 5/F1, table row #22) both the original and this port already glob/list — nothing to bind | none — the real reachability path (generic help browser) is already ported; `AppState::Controllers` stays an inert, never-triggered leaf | done |
| 24 | Pre-match SETUP screens (player input type, level/rounds) | pinned | ported | setup-screens.md, ROADMAP "Pre-match SETUP screens... DONE 2026-07-05" | — | done |
| 25 | Net-game setup screens (`sub_42B0CE`, `sub_42B47D`) | pinned (addresses identified) | **N/A — netplay, excluded (ADR-0003)** | setup-screens.md: "explicitly NOT reproduced (kept for reference)" | none — out of scope | N/A |
| 26 | Player colour `.RMP` remap pipeline | pinned | ported | player-colour.md (no open gaps), `rmp.cpp`, `test_rmp.cpp` | — | done |
| 27 | Options screen (19-row `sub_4080DC`) | pinned | ported (most rows) | results-and-options.md, ROADMAP "Options screen + key-remap aligned to the RE — DONE 2026-07-08"; net/modem/memory rows explicitly omitted (netplay/dead HW, N/A) | — | done for in-scope rows |
| 28 | Key-remap UI (`sub_407B9D`) | pinned | ported | results-and-options.md, `KeyRemapScreen` | — | done |
| 29 | Goldman Roulette wheel | pinned | ported | goldman-roulette.md, ROADMAP "Goldman Roulette wheel — DONE 2026-07-08" | — | done |
| 30 | Goldman wheel prize id 13 (clogs, speed-penalty booby prize) | pinned | ported | goldman-roulette.md §9 "RESOLVED 2026-07-08": hashed `Player::clogs` count + `MatchConfig::born_with_clogs` overlay (not a 14th `PowerupType` — permanent design decision, not a stub), speed-penalty folded into the walk-speed term per `tuning.hpp`'s `clogs_speed_penalty` (id 91); `f374e22`/`228a33f`. Wheel prize-icon render for slot 13 also done: goldman-roulette.md §9.5 "RESOLVED 2026-07-09" pins `sub_425C7F`/`off_45BE50[13]="clog"` and confirms `DATA/ANI/POWERS.ANI` ships a real `"power clog"` sequence; `SequenceSet::clogs_anim` + `GoldmanScreen::draw()`'s slot-13 branch (landed in `f374e22`) draw it | — | done |
| 31 | Hidden scheme/map editor (`sub_4028D2`) | pinned | ported | results-and-options.md, ROADMAP "Hidden scheme editor — DONE 2026-07-08" | — | done |
| 32 | Editor: Ctrl+B reset, '0' dead-tileset toggle, brush-preview-at-cursor, exact dialog chrome | pinned | **absent** | results-and-options.md: "Still NOT reproduced" | low-value polish pass on editor_screen.cpp/editor_grid.cpp | low |
| 33 | Editor: powerup sub-editor mouse-only interaction | pinned | ported (keyboard substitute) | results-and-options.md: "documented deviation" | none required — accepted deviation, note if mouse support is later added | low |
| 34 | In-round HUD (clock/warning ink/hurry flash/SFX 2700) | pinned | ported | in-match-shell.md "Port status: DONE — no longer a gap" (already fixed by the prior §6 pass), ROADMAP "MM:SS clock HUD... reconciled" | — | done |
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
| Text data (misc) | `.TXT` | 18 (16 install-shipped — see note) | pinned | ported | MESSAGES.TXT via `messages.cpp` (the only one BM95.EXE parses — confirmed via the `"Unable to open messages.txt file."` diagnostic string, the sole `.txt` literal in pseudo.c); spot-checked 2026-07-09: none of the other 17 is silently-unparsed gameplay data — see below | none — spot-check complete | done |
| Palette remap | `.RMP` | 10 | pinned | ported | `docs/re/player-colour.md`, `rmp.hpp/.cpp`, `test_rmp.cpp` | — | done |
| BM help-screen | `.BM` | 10 | pinned | ported | `docs/formats/bm.md`, `bmscreen`/HelpBrowser | — | done |
| Resource list | `.RES` | 8 (incl. VALUELST.RES, SOUNDLST.RES, EXTRA*.RES) | pinned | ported | `reslist.hpp`, `extra.hpp`, facts.md, stage-actors.md (EXTRA*.RES actor registry) | — | done |
| Font | `.FON` | 3 | pinned | ported | `docs/formats/fon.md`, `bmfont.hpp`, `test_bmfont.cpp` | — | done |
| Misc data | `.DAT` | 3 (LEVELS.DAT, bmstats.dat, WINEREG/EReg058.dat) | pinned — all 3 identified | N/A | `docs/re/facts.md`: `LEVELS.DAT` "NOT READ by the shipped game" (installer artifact, never opened); `bmstats.dat`/`bmstats.txt` "write-only play-telemetry dump, no reader, no reachable UI" — `sub_40200C` (0x40200C-0x40214F) writes a 100×`int32` counter blob + a `messages.txt`-labelled (ids 900/905/910-928) text report at shutdown (registered via the generic deinit mechanism, `sub_410EBF`/`sub_410F00`), fed by live inline counters (e.g. `sub_410F81`'s match-start `inc`), but exhaustively confirmed **never read back** by any shipped binary and **never displayed** by any reachable menu/screen; the 3rd `.DAT` — **`WINEREG/EReg058.dat`** (1079 bytes) — identified 2026-07-09: hexdump shows a plain-text `[Public User Data]` INI-style key list (`Salutation=`, `FirstName=`, `LastName=`, `Company=`, `Address1/2=`, `Phone=`, `EMailAddress=`, `HaveModem=`, `HaveJoystick=`, etc., all blank — the wizard's un-filled registration-form template), sitting alongside `WINEREG.EXE`/`EREGUI32.DLL`/`EREG3201.DLL`/`INTER.BMP` in the bundled product-registration-wizard's own `WINEREG/` folder; grepped `pseudo.c` for "EReg", "058.dat", "registration" — zero hits, confirming BM95.EXE never opens it. Same non-gameplay tooling class as the already-closed `.BMP` row (`WINEREG/INTER.BMP`) | none — `LEVELS.DAT` is dead/tooling data; `bmstats.dat`/`.txt` is live but write-only debug telemetry with no player-facing consumer; `EReg058.dat` belongs to the separate bundled WINEREG tool, not the game. No parser/writer warranted for any of the three | done — all 3 `.DAT` files identified and closed 2026-07-09 |
| Campaign | `.CAM` | 3 (CROUTON.CAM, GHOSTS.CAM, SIMPLE.CAM) | pinned | ported | `docs/re/campaign.md` — reachability CONFIRMED: hidden 'C'×5 trigger on the local player-setup screen (`sub_410F81` → picker → `sub_401085` loader → `dword_46489C` flag read at ~12 sites). `.CAM` parser (`libs/assets/campaign.hpp/.cpp`, `test_campaign.cpp`); `CampaignFilePicker` (`libs/game/campaign_screen.hpp/.cpp`); stage sequencing (`AppInput::CampaignContinue`, `GameApp::load_campaign_stage`), AI-count roster seeding (`sub_40151B` CORRECTED, `seed_campaign_ai_slots`), stage banner (`present_campaign_banner`), the campaign-activation confirmation dialog (`sub_4015C6`, `present_campaign_confirm`, PORTED 2026-07-09), round pacing (`sub_4016DA` PINNED, all 5 clauses now ported — `hazard_clear_timer` + `campaign_round_needs_replay`/`campaign_no_human_survivor` for the mutual-wipeout replay fallback, doctested in `test_frontend.cpp`), and the rover/ghost hazard actors themselves (RoverSystem, table row #13) are all ported. field-8 (`ai_difficulty`) CONFIRMED dead code — grepped every read site, none exists beyond the loader's own write. The original's campaign-exit key is CONFIRMED negative (no dedicated key exists — `dword_46489C` has exactly two writes total, `sub_4015C6`'s `=1` and `sub_42A3F6`'s entry `=0`); the port's Esc-clears-flag plus a matching entry-point reset at Menu→StartMatch reproduce the same observable behaviour | none — every previously-open edge case above is closed | done |
| Palette | `.PAL` | 1 (COLOR.PAL) | pinned | ported (via PCX palette loading) | RE-NOTES.md | — | done |
| Bitmap | `.BMP` | 1 | pinned | N/A | `WINEREG/INTER.BMP` — confirmed 2026-07-09: lives inside the bundled `WINEREG.EXE`/`EREGUI32.DLL` registration-wizard tool's own folder (product registration, not the game), not referenced anywhere in `pseudo.c` (BM95.EXE never opens it). Non-gameplay, same class as the `.EXE`/`.DLL` tooling row | none | done |
| Icon | `.ICO` | 1 (BM95.ICO) | N/A | N/A | application icon, not game data | none | N/A |
| Executable/DLL | `.EXE`/`.DLL` | 9 / 2 | N/A | N/A | tools (MAKECFG.EXE, decompile tooling), not game assets | none | N/A |
| IDB | `.IDB` | 2 | N/A | N/A | IDA database, explicitly excluded from repo per CLAUDE.md | none — must never be committed | N/A |
| Config | `.INI` | 4 | pinned | ported | `options.ini` via `load_options`/`save_options`; CFG.INI/nodename.ini are net/legacy config — confirm not needed | verify CFG.INI/nodename.ini are netplay-only before ignoring outright | low |

**`.TXT` spot-check detail (2026-07-09).** The `find`-reported "18" `.TXT`
files break down as:

- `MESSAGES.TXT` — parsed (`messages.cpp`); the only one BM95.EXE reads (the
  sole `.txt` literal in `pseudo.c` is the `"Unable to open messages.txt
  file."` error string for it).
- `README.TXT` — plain human-readable readme prose, not consumed by the game.
- `bmstats.txt` / `critlog.txt` — game-**generated** output (a formatted stats
  dump — "Bomberman Statistics File: ... Matches Started/Bombs Dropped/
  Bricks Destroyed" — and a network CritPacket diagnostic log respectively),
  not input; no literal filename string for either exists in `pseudo.c`
  (built from a runtime path), but their content is self-evidently a
  write-only report/log, not silently-unparsed gameplay data.
- `TOOLS/{ANIMS,FREDSPIT,GAME,PLAYSH,PSS,STAGES,TOOLHELP}.TXT` (7) and
  `WINEREG/{PRTBODY,PRTFAX,PRTMAIL,PRTRCRD,XMT}.TXT` (5) — belong entirely to
  the SEPARATE bundled tools shipped alongside the game (`FREDIT.EXE`/
  `PSS.EXE`/`PLAYSH.EXE`/`NUMBER.EXE`/`EXTPSS.EXE` level-editor toolkit;
  `WINEREG.EXE` registration wizard), not to BM95.EXE — grepped every one of
  these 12 filenames against `pseudo.c`, zero hits. Confirmed non-gameplay.
- `decompile_done.txt` / `idalog.txt` — **not part of the original 1997
  install at all**: they're this repo's own RE-tooling scratch output
  (`decompile.bat`/`decompile_all.py`, also present in the same folder,
  dated the same session), left behind in the install directory from a prior
  IDA decompile run. The genuine install-shipped `.TXT` count is **16**, not
  18; CLAUDE.md's "never commit exe-derived material" already keeps these out
  of the repo, but the asset-table count above is corrected to note the
  discrepancy rather than silently double as an inventory of our own tooling
  byproducts.

**Format doc gap (structural, not a missing parser) — CLOSED 2026-07-09.**
`docs/formats/` used to have only `ani.md`, `bm.md`, `fon.md`; PCX/SCH/
RES-list/RSS/RMP/VALUELST were all *parsed* (code exists, tests exist) but
had no standalone `docs/formats/*.md` writeup — the knowledge lived only in
facts.md prose and code comments, inconsistent with the architecture note in
CLAUDE.md ("formats in `docs/formats/`"). Fixed: six new writeups added,
each consolidating the existing parser (`libs/assets/src/*.cpp`) + the
relevant `docs/re/*.md`/`docs/valuelst-map.md` prose into a standalone
byte/line-layout reference, matching the style of the three existing files —
`docs/formats/pcx.md`, `docs/formats/sch.md`, `docs/formats/res.md` (covers
both the commented `id,value` grammar shared by VALUELST.RES/SOUNDLST.RES and
the unrelated dash-command `EXTRA<N>.RES` actor-placement grammar),
`docs/formats/rss.md`, `docs/formats/rmp.md`, `docs/formats/valuelst.md`
(VALUELST.RES's `getvalue` runtime-lookup contract specifically; the id
meaning table itself stays at `docs/valuelst-map.md`, not duplicated).
`docs/formats/` now has 9 files, one per parsed asset format. While writing
`valuelst.md` this pass found and flagged (not fixed, out of this task's
scope) a pre-existing address mislabel in `facts.md`'s "VALUELST lookup
mechanism" section — see that file's own note for detail; every OTHER
mention of `getvalue`/`getstring` in the repo's docs is self-consistent and
was used instead.

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

- Campaign mode — CLOSED 2026-07-09. Fully ported: `.CAM` parser, 'C'×5
  trigger, picker, activation confirmation dialog, stage sequencing,
  AI-count roster seeding, stage banner, round pacing (all 5 clauses,
  including the mutual-wipeout stage-replay fallback), and the rover/ghost
  hazard actors themselves (a new `libs/sim` actor system with hashed
  state). The original's campaign-exit key is CONFIRMED negative (no
  dedicated key exists). See asset-table `.CAM` row above and table row
  #13. No longer a parked item, no remaining residuals.
- Random Start Options-row wording — intentionally left unguessed until RE'd
  (per ROADMAP note under Interactive Options screen); superseded — random
  start IS now RE'd and ported (facts.md "Options toggles", `random_start=`).
  **Stale ROADMAP phrasing, not an open gap** — fixed in this pass (see §6
  item 9) alongside the other stale ROADMAP lines this snapshot found.

## 6. Documentation staleness found during this audit — FIXED

Items 1-5 below were corrected in an earlier pass (worktree
`worktree-agent-a2dd02d9c78d8e344`); items 6-9 were found and fixed in this
2026-07-09 pass (worktree `worktree-agent-afc56c379215a828e`), triggered by
the attract-mode, clogs, and campaign/rover-ghost merges landing since the
prior snapshot. Kept here as a record of what was stale and what superseded
it.

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
   §8 updated to match. **Superseded 2026-07-08 same day**: the clogs
   decision itself was then made and shipped (`f374e22`/`228a33f`, table row
   #30) — `goldman_wheel.hpp` now documents the permanent design (clogs is
   never a `PowerupType`), not an open decision. Only the wheel's cosmetic
   prize-icon render for slot 13 remains open (see row #30's residual note).
6. `libs/game/src/game_app.cpp` (~line 2365, in the VICTORY-screen campaign
   stage-advance path) — "rovers/ghosts don't exist in libs/sim yet (see
   load_campaign_stage's TODO), so there is nothing for that clause to gate
   on" — was **stale**, left behind by the rover/ghost merge (`fd811d5`)
   which added `RoverSystem` and wired `hazard_clear_timer` into the exact
   grace-timer check this comment claims is unimplemented (see the edge-check
   ~30 lines above the stale comment, in the same function's tick loop).
   **Fixed**: comment reworded to state clause 3 (the hazard-clear grace
   timer) is wired via `State::hazard_clear_timer`, matching
   `load_campaign_stage`'s own up-to-date comment on the same topic just
   above it in the file.
7. `docs/re/frontend-flow.md` "Attract mode" §"Port status": "our boot chain
   is faithful; the **menu-idle attract match is a documented gap**" — was
   **stale**, superseded by "Port attract-mode demo match" (`d83cd9a`,
   merged `96be2e4`). **Fixed**: reworded to "port DONE" citing
   `GameApp::run_boot_attract`/the `AttractSaved` save-restore contract and
   `input.hpp`'s pure roster/stage-pick helpers, matching table row #18.
8. `docs/ROADMAP.md` "Remaining front-end: attract-mode demo match (partial
   work parked in a worktree; task was user-stopped)... Known parked
   fidelity gaps... flame-arm stops, flying-bomb landing on powerups, scatter
   occupancy test" — was **stale** on both counts: attract mode shipped (see
   item 7 above), and all three fidelity gaps were already closed by "Merge
   fidelity gaps" (`5370bbf`) well before this snapshot — the unchecked `[ ]`
   boxes were simply never ticked/removed. **Fixed**: both bullets replaced —
   attract mode moved to the Done section's dated summary paragraph, the
   fidelity-gap bullet removed (superseded by the `[x]` Phase 1 items #3/#4
   /#5 already recording the fix).
9. `docs/ROADMAP.md` "F1 mid-round help browser = TODO (needs the generic
   .BM glob browser, same as menu row 5)" (end of the "In-round shell" `[x]`
   item) — was **stale**, superseded by "Merge help-browser sweep" (`e265541`)
   which unified menu row 5 and in-round F1 onto the same `sub_41431C`
   browser (table row #22, in-match-shell.md). **Fixed**: sentence reworded
   to state F1 is DONE, citing the help-browser sweep. Also fixed the
   adjacent Random Start Options-row wording flagged stale in §5 above.

None of these were gameplay bugs — they were doc/comment lag behind later
ROADMAP/merge commits in the same repo.

Note: `docs/re/facts.md` was intentionally left untouched by this pass (a
concurrent task was working near it in the prior 2026-07-08 snapshot; this
pass re-checked it and found no new staleness — its "Still guessed" table is
still correctly empty).

---

## Summary counts

Counting the 39 numbered subsystem rows (§1+§2) + the 14 distinct
asset-format rows in §3 (excluding pure-tooling extensions marked N/A:
.ICO/.EXE/.DLL/.IDB):

- **Covered (RE pinned + ported, "done"):** 37 of 39 subsystem rows (up from
  31 at the 2026-07-08 snapshot — #13 rover/ghost mover, #18 attract mode,
  and #30 clogs prize (effect + wheel icon render) closed earlier in this
  2026-07-09 pass; #15 wall-slam SFX and #23 INPUT.BM menu-row binding
  closed in the same day's SFX/audit sweep) + **14 of 14** asset formats (up
  from 11 — .CAM/campaign, then `.BMP`, then the `.DAT` row's 3rd file
  (`WINEREG/EReg058.dat`) closed this pass) — the large majority of 1:1
  gameplay and front-end fidelity, and every asset-format row now closed.
- **Partial/open (RE pinned, port absent or a small residual):** 2 subsystem
  rows — #32 editor chrome polish, #35 in-round debug/cheat keys. No asset
  format rows remain open: the `.DAT` row closed 2026-07-09 (`LEVELS.DAT`
  confirmed dead/tooling data, `bmstats.dat`/`.txt` confirmed
  live-but-write-only debug telemetry with no reader/reachable UI, and the
  3rd file — `WINEREG/EReg058.dat`, a blank registration-wizard user-data
  template — identified and confirmed non-gameplay tooling, same class as
  the `.BMP` row).
- **N/A / excluded (netplay per ADR-0003, or non-gameplay tooling):** 1
  subsystem row (#25 net-game setup screens) + the §4 netplay function
  cluster + several tooling file extensions (.ICO/.EXE/.DLL/.IDB/.BMP).
- **Doc staleness (no code gap, just needs a note fixed):** 9 items (§6),
  items 1-5 fixed in the 2026-07-08 pass, items 6-9 found and fixed in the
  2026-07-09 pass.

## Top open items, priority order

Everything that was "open" at the 2026-07-08 snapshot's top of this list
(attract mode, the three fidelity gaps, the VICTORY music row, the clogs
prize decision) is now **done** — see §6 items 7-9 and table rows #13/#18/
#30. `LEVELS.DAT` and `bmstats.dat`/`.txt` (formerly item 1 and part of the
"low-priority polish" bucket) are now also **done** — both RE'd 2026-07-09:
`LEVELS.DAT` confirmed dead/tooling data; `bmstats.dat`/`.txt` confirmed live
write-only debug telemetry (a real writer exists, `sub_40200C`, run
automatically at shutdown) but with no reader anywhere and no reachable UI,
so neither is load-bearing; see §3 and `docs/re/facts.md`. The Goldman wheel
clogs prize-icon render (§2 #30's former residual) is likewise **done**:
goldman-roulette.md §9.5 pins `sub_425C7F`'s `"power %s"` +
`off_45BE50[13]="clog"` lookup and the shipped `DATA/ANI/POWERS.ANI`
"power clog" sequence; `SequenceSet::clogs_anim` + `GoldmanScreen::draw()`'s
slot-13 branch draw it, landed in `f374e22`. The wall-slam SFX call site and
the INPUT.BM menu-row binding were closed in the same day's SFX/audit sweep
— see table rows #15/#23 and facts.md/frontend-flow.md for the evidence.
Campaign round-pacing clause 5 (mutual-wipeout stage-replay fallback,
campaign.md "Round pacing"), the campaign-activation confirmation dialog
(`sub_4015C6`), and the campaign-exit key targeted RE pass — the three
residuals this list previously tracked — are **all CLOSED 2026-07-09**: see
`docs/re/campaign.md`'s "Round pacing"/"Campaign-activation confirmation
dialog"/"Campaign-exit key" sections and the `.CAM` table row above.

What remains open, in priority order:

1. Low-priority polish: editor chrome (#32: Ctrl+B reset, '0' toggle,
   brush-preview, exact dialog chrome), in-round debug/cheat keys (#35,
   developer/QA-only).

`.BMP`/`.TXT`/`.DAT` asset spot-checks (§3) — **CLOSED 2026-07-09**: the
single `.BMP` (`WINEREG/INTER.BMP`) belongs to the bundled
registration-wizard tool, not the game; all 18 (16 install-shipped) `.TXT`
files are accounted for (1 parsed, 1 readme, 2 game-generated output logs,
12 belonging to separate bundled tools, 2 our own RE-tooling scratch files
miscounted in the "18"); the 3rd `.DAT` file is `WINEREG/EReg058.dat` (1079
bytes, a blank `[Public User Data]` registration-form template), also
belonging to the bundled registration-wizard tool, not BM95.EXE — grepped
`pseudo.c` for "EReg"/"058.dat"/"registration", zero hits. See §3's detail
note and the `.DAT` row.

The **format-doc gap is CLOSED 2026-07-09**: `docs/formats/pcx.md`,
`sch.md`, `res.md`, `rss.md`, `rmp.md`, `valuelst.md` added, bringing
`docs/formats/` to 9 files (one per parsed asset format) alongside the
pre-existing `ani.md`/`bm.md`/`fon.md`. See §3's "Format doc gap" note.

See the "TODO(RE) / TODO(§) crumbs still in the tree" list below for the
exact file:line inline markers a future session can pick off directly.

## TODO(RE) / TODO(§) crumbs still in the tree

Grepped `TODO(RE)` and `TODO(§` across `docs/`, `libs/`, `apps/`, `tests/`
(re-checked 2026-07-09, after closing the campaign confirmation-dialog and
campaign-exit-key crumbs — see below). The two campaign crumbs this list
previously tracked (`game_app.cpp`'s former lines 993 and 1832) are now
**CLOSED**: the confirmation dialog is ported (`present_campaign_confirm`,
`docs/re/campaign.md` "Campaign-activation confirmation dialog") and the
campaign-exit key is a CONFIRMED negative (`docs/re/campaign.md`
"Campaign-exit key") — both comments were rewritten in place, removing the
literal `TODO(RE)` markers. Two crumbs remain, both pointing at the SAME
unresolved gap (not two independent ones):

1. `libs/game/src/game_app.cpp:52` and `:1781` — the `sub_43C734` dialog
   family's explicit horizontal-centering X value: `sub_43D398`'s own
   internal X computation is a Hex-Rays "possibly undefined" register the
   decompile alone can't resolve; every call site's visible intent is a
   horizontally-centered dialog (matching the port's `dialog_rect`
   convention), but pinning the EXACT source register/expression needs a
   disassembler pass this environment doesn't have. Low priority: the port's
   centering behaviour already matches every dialog's visible on-screen
   intent, so this is a provenance gap, not an observable-behaviour gap.
2. `libs/game/include/bomber/game/editor_grid.hpp:102` — `// earlier "brush
   sizes 1/2/3, anchor rule TODO(RE)" is resolved by`. Not a live TODO: this
   is a comment *referencing* a past TODO(RE) that was already resolved (the
   original has no multi-cell brush, confirmed). Matched by the grep but not
   actionable — safe to leave as historical context, or reword to drop the
   literal "TODO(RE)" substring if a future pass wants the grep clean.

No other `TODO(RE)`/`TODO(§` markers exist in the tree. (Plain `TODO` without
those tags also appears at `libs/game/include/bomber/game/options_screen.hpp`
lines 49/53 — Network screen and Goldman-wheel-consumer follow-ups already
tracked via table rows #23/#29 — and is not double-counted here.)
