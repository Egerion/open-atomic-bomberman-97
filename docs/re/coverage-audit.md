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

**"Is the attributed set actually complete?" — see `docs/re/dark-matter.md`
(2026-07-09).** That sweep starts from a fuller `pseudo.c` snapshot (1134
decompiled functions, a later/larger batch-decompile than the 330-function
figure above) and a broader attributed-address scan (337 addresses, adding
`docs/formats/*.md`/`docs/adr/*.md`/`docs/valuelst-map.md`/`docs/ROADMAP.md`/
`docs/RE-NOTES.md`/code comments to this file's `docs/re/*.md`-only sweep),
then call-graph-walks every unattributed function to find the ones no
documented code ever calls. Verdict: no unattributed *gameplay* code
remains — the entire unreachable-from-known-code pool resolves to CRT/
runtime, gfx/sound engine internals, netplay transport (plus a previously
uncatalogued modem/serial-COM driver cluster, worth folding into this file's
§4 netplay list), and generic dialog plumbing, with a single exception: a
3-function AI direction-scoring routine that turned out to be **dead code**
(zero callers anywhere in the decompiled corpus, not part of the documented
8-slot AI behavior table) rather than a live unRE'd mechanic. This file's
own subsystem/asset-format counts are unaffected.

**Complementary pass:** `docs/re/id-audit.md` (2026-07-09) is a *data-driven*
audit at the id level rather than the function level — every VALUELST.RES
and SOUNDLST.RES id cross-checked against every `getvalue`/sound-play call
site and every port consumer. It found and fixed a real SOUNDLST range bug
(post-death taunt playing the wrong sound group) and surfaced a handful of
small, self-contained, previously-undocumented gaps (see its own "Verdict /
work queue") that this subsystem-level table doesn't itemize individually —
folded into item 2 below rather than duplicated as new numbered rows here.

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
| 13 | AI: campaign "rover"/"ghost" mover (`sub_401AAE`/`sub_401B05` spawn, `sub_401B5C` per-tick mover) | pinned | ported (2 minor documented deviations, both dormant) | `docs/re/campaign.md` "Rover/ghost/AI roster — CORRECTED", `libs/sim/src/systems/rovers.cpp/.hpp` (wander AI, human-avoidance bias, flame-death + kill-score, landing-tile player kill), hashed `State::rovers`, `tests/test_rovers.cpp`; wired via `simulation.cpp`, rendered (`renderer.cpp`), campaign pacing clause 1/3 consumed in `game_app.cpp`. Two known deviations, both documented in campaign.md's "Port status" section: `RoverSystem::spawn`'s distance-3 gate filters `!present`/`!alive` slots (the disassembly's `sub_422351` doesn't) — negligible practical effect; the one-shot rover-spawn powerup-relocation cleanup (`sub_42583B`/`sub_425704`, mover step 0) is NOT ported — unreachable in practice (no scenario spawns a rover onto an already-floored powerup) | low — wire the powerup-relocation cleanup if a golden scenario ever needs it | done |
| 14 | AI: `sub_4245DA` "column-guard" comparand | pinned | ported | ai.md §9.3 RESOLVED (2026-07-09, raw-disasm re-pin): comparand = max-bombs byte +86, and `sub_4245DA` counts the actor's OWN live bombs (owner word at bomb +62), not bombs-in-column; port CORRECTED to the spare-capacity gate (`ai.cpp` behaviours 3/4, `test_ai.cpp` pins) | — | done |
| 15 | Wall-slam SFX ids 140–146 | pinned | ported (fixed) | facts.md "Wall-slam SFX — CONFIRMED (2026-07-09, `sub_426818`/`sub_4278F2`)": call site is the enclosure stepper's drop loop, `dword_462244 = rand()%3` drawn ONCE per arm; SOUNDLST.RES's own "hard-coded to play one of the three below" comment corroborates 140/141/142-only, 143-146 dead. `SoundDirector`/`AudioEngine::roll` fixed to latch one id per round instead of re-picking per drop | — | done |
| 45 | Per-level tile regeneration (VALUELST 340-350/695, Haunted House) | pinned | ported | facts.md "Per-level tile regeneration — CONFIRMED (2026-07-09, `sub_426704`, called from `sub_426818`)", `libs/sim/src/systems/tile_regen.cpp`, `tests/test_regen.cpp`; one-time hash-layout growth (`State::regen_timer`), recaptured in `test_golden.cpp` | — | done |
| 46 | Ice / input-lag (VALUELST 450-460, Hockey Rink) | pinned | ported | facts.md "Ice / input-lag — CONFIRMED (2026-07-09, `sub_41F29B` ~23058-23078)", `MovementSystem::ice_delay` (`libs/sim/src/systems/movement.cpp`), `tests/test_ice.cpp`; one-time hash-layout growth (`Player::ice_history`), recaptured in `test_golden.cpp` | — | done |

## 2. Front-end / presentation subsystems (libs/game)

| # | Subsystem | RE status | Port status | Evidence | Next task | Priority |
|---|---|---|---|---|---|---|
| 16 | Boot flow (IPLOGO→HSLOGO→TITLE→menu), `sub_42B060`/`sub_42B9CE` | pinned | ported | frontend-flow.md, ADR-0004, `app_flow.hpp`, `test_frontend.cpp` | — | done |
| 17 | Attract mode boot-loop restart (no attract on boot) | pinned | ported | frontend-flow.md "boot flow is STRAIGHT-LINE" | — | done |
| 18 | Attract mode: menu-idle LIVE AI-only demo match | pinned | ported | frontend-flow.md table (this row, "Merge attract-mode demo match" 96be2e4): `attract_` flag (`dword_464938` equivalent), `input.hpp`'s `attract_computer_count`/`fill_attract_roster`/`attract_stage_pick` helpers, roster/level/team save-restore (`GameApp::AttractSaved`), AI-only dispatch bypassing the goldman wheel/setup/map-select screens, any key/mouse/pad input aborts immediately, DRAW/RESULTS/VICTORY suppressed | — | done |
| 19 | Main menu (7 rows, cursor, navigation) | pinned | ported | frontend-flow.md "main-menu items", `present_menu`; audited against `sub_42B9CE` again 2026-07-09 (row labels baked into MAINMENU.PCX confirmed — none drawn, matching; cursor anchor/sounds/dispatch already matched) — row #42 below (found the same pass) later fully traced and closed N/A, not a port gap | — | done |
| 20 | Results tail: DRAW/RESULTS/VICTORY tiers | pinned | ported | frontend-flow.md, ROADMAP "Multi-round best-of-N loop + RESULTS tally 1:1 — DONE 2026-07-08" | — | done — frontend-flow.md's older "port still DEFERRED" note fixed to match (§RESULTS tally tier) | done |
| 21 | VICTORY music using track 1020 instead of 1130 | pinned | **RESOLVED** | `game_app.cpp`: `kDrawMusicId` (1130) is started for DRAW, RESULTS, **and** VICTORY/TEAM (`audio_.start_music(kDrawMusicId)` in every outcome branch); `kWinMusicId` (1020) is scoped to the Play/setup path only. frontend-flow.md's "Results MUSIC" section and the 1020/1130 tunables rows updated to match — this audit's snapshot was stale on this row | — | done |
| 22 | `.BM` generic help browser (menu row 5 + in-round F1) | pinned | ported | ROADMAP "The generic .BM help BROWSER — DONE 2026-07-08" | — | done |
| 23 | Controllers/INPUT.BM menu-row binding | pinned | **N/A — confirmed negative** | frontend-flow.md "INPUT.BM menu-row binding — CONFIRMED NEGATIVE (2026-07-09)": no "controller"/"INPUT.BM" string anywhere in pseudo.c; INPUT.BM is just one of ~10 topics in the generic `*.BM` help browser (row 5/F1, table row #22) both the original and this port already glob/list — nothing to bind | none — the real reachability path (generic help browser) is already ported; `AppState::Controllers` stays an inert, never-triggered leaf | done |
| 24 | Pre-match SETUP screens (player input type, level/rounds) | pinned | ported | setup-screens.md, ROADMAP "Pre-match SETUP screens... DONE 2026-07-05" | — | done |
| 25 | Net-game setup screens (`sub_42B0CE`, `sub_42B47D`) | **fully RE'd 2026-07-25 — `docs/re/network-screens.md`** (was: addresses only) | **back IN scope under ADR-0010** — the port now ships netplay and needs both lobby screens | network-screens.md (geometry, per-state lines, keys, inks, the join/wait state machine) | the port's invented "waiting room" should be replaced by these two | HIGH |
| 26 | Player colour `.RMP` remap pipeline | pinned | ported | player-colour.md (no open gaps), `rmp.cpp`, `test_rmp.cpp` | — | done |
| 27 | Options screen (19-row `sub_4080DC`) | pinned | ported (full 19-row audit, 2026-07-09) | results-and-options.md §3 "2026-07-09 full 1:1 audit": all 19 rows now draw (previously 8 net/modem/legacy rows were hidden — CORRECTED, the original shows them too, in the same ink); real `cursor1` MISC.ANI selection sprite replaces the earlier text-recolour stand-in; no invented "OPTIONS" header (none in the decompile); row-nav wrap faithfully reproduces the original's off-by-one row-count literal of 18 (row 18 "Adjust Audio" permanently unreachable, matching the shipped binary); uniform SFX 20 (no invented accept jingle); Team Play toggle now also resets the pending Goldman winner, matching row 6; rows 10/12/17 upgraded from omitted to LIVE round-tripped toggles | — | done |
| 28 | Key-remap UI (`sub_407B9D`) | pinned | ported | results-and-options.md, `KeyRemapScreen` | — | done |
| 29 | Goldman Roulette wheel | pinned | ported | goldman-roulette.md, ROADMAP "Goldman Roulette wheel — DONE 2026-07-08". 2026-07-09 full gold sweep (§2.1): fixed two previously-missed `dword_46492C`-clear sites — the LEVEL & ROUNDS screen's Esc was wrongly ported as "back to the player screen" (original aborts the whole Play flow, same as the wheel's own Esc) and the Options screen's Team Play toggle never forfeited a pending gold player (only the Gold Bomberman row did) — plus pinned §6.1, the twinkle's actual RENDER mechanism (`sub_420E39`, the `"goldman"` MISC.ANI sequence, particle lifetime bound by the ANI's own frame count rather than getvalue(1010), non-additive draw); the parallel sparkle-render port (`Renderer::update_gold_sparkles`/`draw_world`, `GameApp::set_gold_player`) landed the same day (`33669ec`) and matches §6.1's pins | — | done |
| 30 | Goldman wheel prize id 13 (clogs, speed-penalty booby prize) | pinned | ported | goldman-roulette.md §9 "RESOLVED 2026-07-08": hashed `Player::clogs` count + `MatchConfig::born_with_clogs` overlay (not a 14th `PowerupType` — permanent design decision, not a stub), speed-penalty folded into the walk-speed term per `tuning.hpp`'s `clogs_speed_penalty` (id 91); `f374e22`/`228a33f`. Wheel prize-icon render for slot 13 also done: goldman-roulette.md §9.5 "RESOLVED 2026-07-09" pins `sub_425C7F`/`off_45BE50[13]="clog"` and confirms `DATA/ANI/POWERS.ANI` ships a real `"power clog"` sequence; `SequenceSet::clogs_anim` + `GoldmanScreen::draw()`'s slot-13 branch (landed in `f374e22`) draw it | — | done |
| 31 | Hidden scheme/map editor (`sub_4028D2`) | pinned | ported | results-and-options.md, ROADMAP "Hidden scheme editor — DONE 2026-07-08" | — | done |
| 32 | Editor: Ctrl+B reset, '0' dead-tileset toggle, brush-preview-at-cursor, exact dialog chrome | pinned | ported | results-and-options.md §5/§5d "2026-07-09: the four remaining items closed out" (Ctrl+B's `dirty_` gate, mirroring the original's own modified flag — including the Esc/Q silent-exit-when-untouched corollary — `editor_grid.hpp`'s `toggle_editor_tileset` + doctest, `EditorScreen::on_mouse_move`, `dialog_chrome.{hpp,cpp}` shared confirm/text-entry chrome wired into `EditorScreen::draw`/`PowerupRulesScreen::draw`) | — | done |
| 33 | Editor: powerup sub-editor mouse-only interaction | pinned | ported (keyboard substitute) | results-and-options.md: "documented deviation" | none required — accepted deviation, note if mouse support is later added | low |
| 34 | In-round HUD (clock/warning ink/hurry flash/SFX 2700) | pinned | ported | in-match-shell.md "Port status: DONE — no longer a gap" (already fixed by the prior §6 pass), ROADMAP "MM:SS clock HUD... reconciled" | — | done |
| 35 | In-round debug/cheat keys (1/4/18/274-305/288) | pinned | **N/A — confirmed negative, per-key (2026-07-09)** | in-match-shell.md "Row #35 closure — CONFIRMED N/A for every key, individually justified (2026-07-09)": key `1`'s dump target/payload is decompiler-unresolvable ("possibly undefined" filename register); key `4` arms raw VGA text-page memory (`0xB0000`/`0xB8000`), a hardware primitive with no modern-OS equivalent; key `18` opens a live HSL colour-remap tuning dialog whose colour math (`sub_414A65`) is already ported (player-colour.md row #26) but whose tool needs two things the port has neither of (a debug-mode flag, live texture regeneration) for a zero-player-value feature; key `288`'s 5 stat fields are 3/5 netplay-only (ADR-0003) and 2/5 Watcom/DirectSound engine counters (mem, audio-cache-hit%) with no port equivalent — showing them would mean fabricating displayed numbers; keys `274-305` are the already-established netplay stats dump (ADR-0003) | none — every key individually traced and closed; not a residual gap | N/A |
| 36 | Pause (Ctrl+Q forfeit, no real pause) | pinned | ported | in-match-shell.md negative finding ("NO pause exists") | — | done |
| 37 | Multi-round best-of-N + win_by_kills clinch | pinned | ported | ROADMAP "Multi-round best-of-N loop", "Kill attribution + win_by_kills — DONE 2026-07-08" | — | done |
| 38 | Faithful screen inks (RGB555 LUT) | pinned | ported | ROADMAP "Faithful screen inks — DONE 2026-07-08" | — | done |
| 39 | SDL3 gamepad support | pinned (`sub_421E80` cycle order) | ported | ROADMAP "SDL3 gamepad support — DONE 2026-07-08" | — | done |
| 40 | Team Play colour split (the red/white sprite override) | pinned | ported | player-colour.md "Team Play colour override" (`sub_4214BC` round-init +60 override, CONFIRMED 2026-07-09), `bomber::match::team_render_colour` (`libs/match/include/bomber/match/team_colour.hpp`), `Renderer::render_colour` (`renderer.cpp`, every player/bomb/flame/carried-bomb/death-anim colour site), `present_setup`'s trailing team-marker glyph (`game_app.cpp`), `test_team.cpp` | — | done |
| 41 | In-round "player row" HUD (S:/K: score+kill grid, "xxx" dead-slot marker) | pinned | ported | in-match-shell.md "The player row — CONFIRMED (`sub_420F07`, corrects the point above)" (2026-07-09) — corrects row #34's/this doc's own earlier "no score/kill HUD" claim; `GameApp::draw_player_row`, `SequenceSet::eliminated_marker` | — | done |
| 42 | In-round "cornerhead" face bubble (`KFACE.ANI`, follows one designated player slot `dword_45BE3C`) | pinned (fully traced, 2026-07-09) | **N/A — confirmed negative** | in-match-shell.md "the cornerhead face bubble — CONFIRMED N/A for a same-screen port (2026-07-09, corrected same day — `21bc184`)": exhaustive 5-site trace of `dword_45BE3C` (declaration, round-reset `sub_421793`, joystick-chord-only write `sub_41E61E` case 3 gated on the RAW `dwButtons` bitmask being exactly 74 (set) / 138 (clear), network-send `sub_4101F1` msg 57, receive-mirror `sub_40E2D8`→`sub_4226F6`) — netplay-replicated, joystick-chord-exclusive, no stable "which slot" identity to translate | none — netplay-only broadcast with no local-multiplayer analogue (the port DOES drive type-3 joystick slots via SDL3 `GamepadMapper`, table row #39, but the mapper only exposes d-pad/left-stick + south/east buttons, no raw button-bitmask surface on which "exactly 74/138" could be reproduced faithfully; no "peer" to broadcast to on one machine either, and no fixed slot identity to reuse without inventing one) | N/A |
| 43 | Main-menu (and other front-end loops') animated-cursor pacing vs. an uncapped render loop | pinned | **RESOLVED** | frontend-flow.md "Cursor pacing — CORRECTED (2026-07-09)": `sub_42B9CE`'s cursor-frame counter advances once per menu-loop iteration with no separate throttle (the DirectDraw flip's own vsync IS the pacing); our port's equivalent `++frame` was uncapped (`SDL_Delay(2)` only, ~500 Hz, ~8x too fast) — fixed with one `SDL_SetRenderVSync(ren, 1)` call in `GameApp::init()`, which also corrects the same pattern in the Goldman wheel spin / boot logos / attract idle | — | done |
| 44 | Team Play match-clinch outcome screen (TEAM0/1.PCX vs VICTORY\<player\>.PCX) | pinned | **fixed 2026-07-09** | frontend-flow.md "VICTORY" §3 (`aTeamU`/`aVictoryU`) — the port's `victory_screen()` always resolved `VICTORY<player>` even under Team Play, a real end-to-end gap left over from before `Player::team` landed (frontend-flow.md's "Spine mapping" note used to say "TEAM%u is a documented future hook"); fixed with `victory_background_name()` (`results.hpp`/`game_app.cpp`, TEAM0/TEAM1.PCX confirmed shipped in the install), gated on `is_team_mode()` at the match-clinch call site, naming the clinching player's raw setup-screen team id (same id `present_scoreboard`'s "TEAM %u WINS" line and the setup-screen marker already use) | — | done |
| 47 | Team Play setup-screen TEAM default (per-slot `+84` byte's initial value) | pinned | **fixed 2026-07-09** | setup-screens.md "TEAM default — CORRECTED 2026-07-09": `sub_410F81`'s unconditional `sub_4046CC()`→`sub_403EEE()`→`sub_4049C0()` chain resets every slot's TEAM byte to **alternating `slot & 1`** (0,1,0,1,...) on every entry to the setup screen (pseudo.c line 6716), not to a flat 0; the port's `setup_team_` defaulted (and stayed) all-0, so Team Play ON without anyone pressing 'T' put every player on the SAME side — the user-reported "only a white team ever forms, match ends instantly" bug (all players got the WHITE `0.RMP` override and `sim::sides_remaining()` read `<=1` from tick 0). Fixed with a `for (slot) setup_team_[slot] = slot & 1;` reset at the top of `GameApp::present_setup()`; round-end semantics (`sides_remaining() <= 1`) were correct and untouched — they only misfired on the degenerate all-one-side roster. `test_frontend.cpp` covers the alternating default | — | done |

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
| Config | `.INI` | 4 | pinned | ported | `options.ini` via `load_options`/`save_options`; **CFG.INI CONFIRMED 2026-07-09 NOT netplay-only** — `docs/re/facts.md` "CFG.INI / soundonoff" traces all 4 keys BM95 reads from it (`hdhome`/`cdhome` install-path detection, `debug`, and `soundonoff` — a boot-time audio-HAL bring-up gate, read unconditionally in local play too); it's a DOS-era install/hardware config (generated by `MAKECFG.EXE`) with no analogue in the SDL3 port (`AudioEngine::init` brings up audio unconditionally) — correctly left unported. `nodename.ini` still unexamined | `nodename.ini` still unconfirmed netplay-only-or-not | low |

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
`valuelst.md` this pass found (but did not fix, out of this task's scope) a
pre-existing address mislabel in `facts.md`'s "VALUELST lookup mechanism"
section: `sub_4124A4` was mislabelled `getvalue` when it is actually
`getstring` (the real `getvalue` is `sub_412135`). **Fixed later the same
day** (`7f76c4c`, verified against pseudo.c arithmetic vs. text-drawer call
sites) — see `facts.md`'s own correction note at the top of the "VALUELST
lookup mechanism" section for detail; every OTHER mention of
`getvalue`/`getstring` in the repo's docs was already self-consistent.

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

Counting the 47 numbered subsystem rows (§1+§2) + the 14 distinct
asset-format rows in §3 (excluding pure-tooling extensions marked N/A:
.ICO/.EXE/.DLL/.IDB):

- **Covered (RE pinned + ported):** 44 of 47 subsystem rows — 43 rows marked
  "done" in the table above, plus #33 (editor powerup sub-editor,
  keyboard-substitute for mouse-only interaction — ported with an accepted,
  documented deviation, so counted as covered despite its "low" priority
  label). Up from 31 at the 2026-07-08 snapshot: #13 rover/ghost mover, #18
  attract mode, and #30 clogs prize (effect + wheel icon render) closed earlier in this
  2026-07-09 pass; #15 wall-slam SFX and #23 INPUT.BM menu-row binding
  closed in the same day's SFX/audit sweep; #32's editor Ctrl+B reset/'0'
  tileset toggle/brush-preview/dialog-chrome polish closed in the same
  day's editor pass; #40 Team Play colour split closed in the comprehensive
  team-mode RE pass; #41 the in-round "player row" S:/K: HUD and #43 the
  vsync cursor-pacing fix closed in the user-findings pass the same day;
  #44 the Team Play TEAM0/1.PCX match-clinch outcome screen — a real
  end-to-end gap the user-reported "Team Play is still broken" pass found
  and fixed the same day; #45 per-level tile regeneration and #46
  ice/input-lag — new sim rows, closed the same day the id-audit surfaced
  them, see docs/re/facts.md) + **14 of 14** asset formats (up from 11 —
  .CAM/campaign, then `.BMP`, then the `.DAT` row's 3rd file
  (`WINEREG/EReg058.dat`) closed this pass) — the large majority of 1:1
  gameplay and front-end fidelity, and every asset-format row now closed.
- **Partial/open (RE pinned, port absent or a small residual):** none. The
  last row in this bucket, #35 in-round debug/cheat keys, was closed
  2026-07-09 (moved to N/A below — see that row's evidence). No asset format
  rows remain open: the `.DAT` row closed 2026-07-09 (`LEVELS.DAT` confirmed
  dead/tooling data, `bmstats.dat`/`.txt` confirmed live-but-write-only debug
  telemetry with no reader/reachable UI, and the 3rd file —
  `WINEREG/EReg058.dat`, a blank registration-wizard user-data template —
  identified and confirmed non-gameplay tooling, same class as the `.BMP`
  row).
- **N/A / excluded (netplay per ADR-0003, or non-gameplay tooling):** 3
  subsystem rows (#25 net-game setup screens; #35 in-round debug/cheat keys,
  closed 2026-07-09 — each of the five key groups [1/4/18/274-305/288]
  individually traced and found N/A: decompiler-unresolvable dump target,
  a raw-VGA-text-page hardware primitive with no modern equivalent, a
  live colour-tuning dialog whose math is ported but whose tool needs a
  debug-mode flag and live texture regeneration the port has neither of,
  a diagnostics window that is 3/5 netplay and 2/5 unmappable engine
  counters, and the already-established netplay stats dump — see
  in-match-shell.md's "Row #35 closure"; #42 the in-round "cornerhead"
  KFACE.ANI face bubble, closed 2026-07-09 after a full trace of
  `dword_45BE3C` showed it to be a netplay-replicated, joystick-exclusive
  global with no local-multiplayer analogue — in-match-shell.md) + the §4
  netplay function cluster + several tooling file extensions
  (.ICO/.EXE/.DLL/.IDB/.BMP).
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

The id-level audit's (`docs/re/id-audit.md`) top three presentation-side
items are now **done**, ported 2026-07-09 (`docs/re/facts.md` "id-audit.md
presentation-side follow-ups"): the VALUELST 650/651 + SOUNDLST 1200-1299
"Fire In The Hole" taunt (`sound_director.cpp`'s `BombPlaced` handler), the
VALUELST 1010 gold-player "twinkle" sparkle (`Renderer::update_gold_sparkles`/
`draw_world`, fed by `GameApp::set_gold_player`), and the VALUELST
500/502/504/506 bomb-pickup carry arc (`Renderer::draw_world`'s carried-bomb
draw, replacing the previous unpinned `sy - 78.0f` guess). All three landed
in `libs/game` only — `libs/sim` untouched, golden hashes byte-identical.

**Nothing remains open.** Every subsystem row is now either **done** or
**N/A (confirmed negative, individually justified)**:

1. (none — every id-audit gap is now closed: the "Fire In The Hole" taunt
   650/651+1200-1299, the gold twinkle 1010, the 500-506 carry arc, and the
   single-level 340-350/695 tile-regen + 449-460 ice-delay mechanics — see
   table rows #45/#46 and `docs/re/facts.md`.)
2. In-round debug/cheat keys (#35) — **CLOSED 2026-07-09**, not deferred:
   each of the five key groups (`1`/`4`/`18`/`274-305`/`288`) was traced to
   its actual function bodies in `pseudo.c` and individually classified N/A
   — a decompiler-unresolvable dump target, a raw-VGA-text-page hardware
   primitive, a live colour-tuning dialog whose math is already ported but
   whose developer-tool chrome needs capabilities (a debug-mode flag, live
   texture regeneration) the port doesn't have for zero player-facing value,
   a diagnostics window that's 3/5 netplay-only and 2/5 unmappable engine
   counters, and the already-established netplay stats dump. See
   in-match-shell.md's "Row #35 closure" section and table row #35 above.
   (Editor chrome #32 — Ctrl+B reset, '0' toggle, brush-preview, exact
   dialog chrome — closed in the same day's editor pass; campaign clause 5
   closed per the paragraph above. The in-round "cornerhead" `KFACE.ANI`
   face bubble, #42, is likewise closed N/A — 2026-07-09's full
   `dword_45BE3C` trace closed it: a netplay-only, joystick-gated broadcast
   with no same-screen-multiplayer analogue, see table row #42 and
   in-match-shell.md.)

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
(re-checked 2026-07-09 end-of-day — the line numbers below drifted since the
editor-polish pass extracted the dialog-chrome primitives out of
`game_app.cpp` into `dialog_chrome.hpp` (`43fe187`/`50f076e`), and this list
had not been refreshed since). The campaign crumbs this list previously
tracked are **CLOSED**: the confirmation dialog is ported
(`present_campaign_confirm`, `docs/re/campaign.md` "Campaign-activation
confirmation dialog", which itself notes "coverage-audit.md TODO(RE) crumb,
now closed") and the campaign-exit key is a CONFIRMED negative
(`docs/re/campaign.md` "Campaign-exit key"). Two genuine crumbs remain,
plus one historical false-positive:

1. **The `sub_43C734`/`sub_43D398` dialog-chrome X-placement gap** — one
   provenance gap, cited at multiple call sites now that the chrome lives in
   one shared header: `libs/game/include/bomber/game/dialog_chrome.hpp:33`
   (the primary note), `:75` and `:92` (cross-references from the
   two-line-prompt ordering and the compact-dialog width baseline, "same
   class of gap"), `libs/game/src/game_app.cpp:1842` (the quit-confirm modal
   comment, cross-reference only), `docs/re/results-and-options.md:796`
   (cross-reference), and the RE source itself,
   `docs/re/frontend-flow.md:148` (`sub_43D398`'s exact X-default formula —
   a Hex-Rays "possibly undefined" register the decompile alone can't
   resolve). Every call site's visible intent is a horizontally-centered
   dialog (matching the port's `dialog_rect` convention), but pinning the
   EXACT source register/expression needs a disassembler pass this
   environment doesn't have. Low priority: the port's centering behaviour
   already matches every dialog's visible on-screen intent, so this is a
   provenance gap, not an observable-behaviour gap.
2. **`sub_43D080`'s exact role** — `docs/re/frontend-flow.md:263`, a
   separate, smaller RE gap in the LOADING dialog's progress-bar readout: a
   call between the caption and the readout whose third geometry argument is
   read from the window's own stored fields (same class of register-spill
   loss as item 1, but a different function/call site). Already resolved as
   a deliberate omission, not open work: "visually inconsequential either
   way (the bar redraws that same band), so the port omits it rather than
   guess a specific 1px line."
3. `libs/game/include/bomber/game/editor_grid.hpp:119` (line number
   corrected from a stale `:102` citation) — `// earlier "brush sizes 1/2/3,
   anchor rule TODO(RE)" is resolved by`. Not a live TODO: this is a comment
   *referencing* a past TODO(RE) that was already resolved (the original has
   no multi-cell brush, confirmed). Matched by the grep but not actionable —
   safe to leave as historical context, or reword to drop the literal
   "TODO(RE)" substring if a future pass wants the grep clean.

No plain `TODO` (without the `(RE)`/`(§` tag) exists anywhere in `libs/`,
`apps/`, or `tests/` as of this pass — the `options_screen.hpp` lines 49/53
this list previously cited no longer carry one (Network screen and
Goldman-wheel-consumer follow-ups are tracked via table rows #23/#29
instead, with no inline marker needed).
