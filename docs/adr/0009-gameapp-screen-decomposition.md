# ADR-0009 — Decomposing the GameApp god-object into screens

Status: accepted (in progress)
Date: 2026-07-23
Follows: ADR-0008 (modernization package structure)

## Context

`libs/game`'s `GameApp` grew into a ~4600-line god-object: one class owning the
SDL window lifecycle, options persistence, ~25 front-end screen methods (each a
self-contained nested `SDL_PollEvent` loop returning an `AppInput`), the match
runtime, attract mode, campaign mode, and demo/screenshot capture — with ~40
data members shared across all of it. Adding a new screen or map means editing
the god-class and threading its member set by hand (the user's stated pain:
"yeni map eklemek çok karışık … developer çok rahat anlasın").

Constraints from CLAUDE.md and the determinism contract:

- The extraction must be **behaviour-preserving (verbatim)**. Each moved body is
  reproduced statement-for-statement, changing only member access to the new
  seam. Extractions are adversarially verified 1:1 before they land.
- `run_match`/`start_match` are **golden-sensitive** (they drive the deterministic
  sim). `tests/test_golden.cpp` must stay byte-identical.
- Many screens are **visual-golden / RE-faithful** — they mirror a specific
  `sub_XXXX` and their pixels are pinned by `tests/visual/`.

## Decision

Keep `run_app` (the `AppState` + `next()` pure-flow-graph driver, ADR-0004) as
the state-machine spine. Lift each `present_*` screen into its own class in
`libs/game/src/screens/`, constructed from two seams, so the god-object shrinks
to the SDL lifecycle + the driver.

### Seam 1 — `ScreenContext` (stable services)

A cheap value bundle of references + raw pointers to the app's stable
presentation services (`assets`, `audio`, `sounds`, `keyboard`, `gamepads`,
`front_font`, `cursor_blink`, `values`, `sdl`, `window`; `seqs` and the
asset-screen presenter added as their consumers are extracted). `GameApp::sctx()`
builds a fresh one on demand; screens store it **by value** so none holds a
reference into a temporary. Match-coupled screens that draw the live frame as a
backdrop take `Renderer&` + `const sim::State&` **separately** — `ScreenContext`
stays front-end-service-only.

### Seam 2 — shared front-end state (the future `Session`)

~30 non-service members are shared across screens: the roster
(`setup_type_/setup_sub_/setup_team_/team_play_`), level selection
(`selected_level_/win_target_/win_count_/kill_count_`), `options_`/`options_dirty_`,
attract state, campaign state, goldman state, the presentation LCGs
(`setup_lcg_/attract_lcg_/goldman_lcg_/next_seed_`), `menu_index_`, and the hidden
trigger counters. These will be grouped into cohesive structs — `RosterState`,
`AttractState`, `CampaignState`, `GoldmanState`, `MatchScores` — that `GameApp`
owns and passes to the screens that need them (each screen takes only the groups
it uses, respecting the ≤4-param rule). Until a screen is extracted, `GameApp`
keeps using the members directly, so un-extracted code is never churned.

### Shared helpers become free functions

Helpers called across screens are promoted out of `GameApp`, so a screen does not
need a `GameApp&` to reach them and their shared mutable state stays single-owner:

- **`pick_glue(std::uint32_t& setup_lcg, const ValueList&)`** → `frontend_util.hpp`.
  It advances the shared presentation LCG in place; **7 call sites** depend on the
  one copy — a per-screen copy would desync the GLUE-pick sequence (a visible
  backdrop difference). (Done.)
- `reload_scheme_from_name` (writes `scheme_`, reads `game_dir`) → a shared helper
  (also used by `init()` and the campaign loader).
- The music/sting id constants (`kBootMusicId`/`kMenuMusicId`/`kWinMusicId`/
  `kDrawMusicId`/`kStageMusicFallback`, the sting ranges, `kResultsDwellMs`) →
  a shared ids header (used across boot/menu/setup/goldman/scoreboard/match).
- `ScreenDef` factories that are single-screen (`logo_screen`/`title_screen` →
  boot; `draw_screen`/`victory_screen` → results) move **with** their screen;
  the shared asset-screen presenter (`present_screen`) becomes a shared helper the
  driver, boot, and results all call.

`fmt_u/fmt_s/fmt_us` were already promoted to `hud_format.hpp`.

## Extraction order (each increment compiles clean, app stays runnable)

1. **Leaf modals** — `BmTextScreen`, `HelpBrowserScreen`, `DebugInfoScreen`,
   `VideoSettingsScreen`. (Done, verified verbatim, committed.)
2. **`pick_glue` → free function** (the shared-helper unblocker). (Done.)
3. **Shared ids + `present_screen` presenter + boot** (`present_screen`,
   `run_boot_attract`) — low state coupling; `logo_screen`/`title_screen` move here.
4. **Options cluster** — `present_options_screen` + its sub-screens
   `present_keyremap_screen`, `present_scheme_picker`, `present_scheme_filename_prompt`
   (+ `reload_scheme_from_name` promotion). Needs `options_`/`scheme_`.
5. **`Session` state groups** — introduce `RosterState`/`AttractState`/
   `CampaignState`/`GoldmanState`/`MatchScores` (mechanical field regroup). The
   enabler for the coupled core screens.
6. **Menu + attract** — `present_menu` (split <300 lines), `roll_attract_match`,
   `restore_from_attract`. `present_menu` is a hub (calls the leaf/editor screens),
   extracted after its callees.
7. **Setup + map-select + `LevelRegistry`** — the map-extensibility feature (below).
8. **Scoreboard + goldman + campaign** — need `Renderer&`+`sim::State&` threaded.
9. **Editor** — `present_editor`.
10. **`MatchRunner`** — `run_match` (split <300), `start_match`, `collect_inputs`,
    `draw_player_row`, `draw_fps_overlay`, and the outcome predicates
    (`round_winner`/`match_clinch`/`is_team_mode`/`campaign_no_human_survivor`/
    `auto_advance_results`/`reset_match_scores`). **GOLDEN-sensitive** — verify
    `test_golden` byte-identical after. Split `init()` (~515 lines) into focused
    helpers; `GameApp` ends as SDL lifecycle + `run_app` driver, ≤300 lines.
11. **tests/ restructure** — mirror the module layout (ADR-0008), lands last.

## LevelRegistry (map extensibility)

Today a "level" is an index 0-10 whose three stage assets are derived by string
concat (`FIELD<n>.PCX`/`TILES<n>.ANI`/`XBRICK<n>.ANI`, `AssetStore::load_stage`/
`stage_preview`), whose name is `getstring(150+n)` (`level_fallback` otherwise),
and whose rotation enable is `Tuning::level_enabled[n]` (VALUELST 1150-1160).
`match::pick_stage` picks over the enabled set with `seed % allowed.size()`.
Adding a 12th level touches `pick_stage`'s `< 11`, the `level_enabled` array,
`level_fallback`, `AssetStore`, and the map-select screen — scattered.

Introduce a `LevelRegistry` (SDL-free data, `libs/match`) listing `LevelDef {
int index; std::string name_fallback; std::string field_asset; std::string
tiles_asset; std::string xbrick_asset; bool enabled; bool builtin; }`,
default-populated with the 11 built-ins (index → asset base names, enabled from
`Tuning::level_enabled[i]`). `pick_stage`, `AssetStore::load_stage`/
`stage_preview`, and the map-select screen all consult the registry. Adding a
level = append one `LevelDef`. **Determinism**: as long as no custom level is
enabled, the enabled list is identical in order to today's, so `seed %
allowed.size()` is byte-identical — custom levels are strictly opt-in. The
built-in path stays golden.

## Verification discipline

Each increment: (a) verbatim move (statements identical modulo the mechanical
member→seam substitution); (b) `bomber_game_core` compiles clean under MSVC /W4;
(c) for golden/visual-sensitive increments, `ctest` golden byte-identical and a
`tests/visual` eyeball once an install is free; (d) periodic live eyeball of the
extracted screens. Increment 1's four extractions were adversarially verified 1:1
by an independent pass before landing.

## Consequences

- `GameApp` shrinks toward a thin app shell; each screen is a self-contained file
  a developer can read and edit in isolation; adding a screen = add a class + one
  `run_app` edge; adding a map = one `LevelDef`.
- A transitional period where `GameApp` still owns the shared state (as members,
  then as `Session` groups) and leaves thin forwarders behind; these are inlined
  away once every caller is itself extracted.
- No behaviour change and no determinism change at any step — the whole series is
  a structural refactor pinned by the golden + visual goldens.
