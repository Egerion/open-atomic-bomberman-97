#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "bomber/assets/campaign.hpp"
#include "bomber/assets/reslist.hpp"
#include "bomber/assets/sch.hpp"
#include "bomber/game/app_flow.hpp"
#include "bomber/game/asset_store.hpp"
#include "bomber/game/audio_engine.hpp"
#include "bomber/game/bmscreen.hpp"
#include "bomber/game/campaign_screen.hpp"
#include "bomber/game/editor_screen.hpp"
#include "bomber/game/gamepad.hpp"
#include "bomber/game/goldman_screen.hpp"
#include "bomber/game/input.hpp"
#include "bomber/game/keyremap_screen.hpp"
#include "bomber/game/options_screen.hpp"
#include "bomber/game/renderer.hpp"
#include "bomber/game/results.hpp"
#include "bomber/game/screen.hpp"
#include "bomber/game/sdl.hpp"
#include "bomber/game/sequences.hpp"
#include "bomber/game/sound_director.hpp"
#include "bomber/game/transition.hpp"
#include "bomber/sim/simulation.hpp"

// The playable front-end: owns the SDL window, the asset store, the
// presentation systems, and the match lifecycle around the deterministic sim.

namespace bomber::game {

// clang-analyzer-optin.performance.Padding (NOLINT below) — a singleton root
// object (one instance for the app's lifetime, apps/game/main.cpp), not a
// hashed sim/hot-path type; clang-tidy's suggested reorder touches ~48
// members by hand in a class this large, which risks a transcription bug
// (member-initializer-list order must track it) for a one-time 34-byte
// saving that has no measurable effect on a singleton.
class GameApp {  // NOLINT(clang-analyzer-optin.performance.Padding)
public:
    struct Options {
        std::filesystem::path game_dir;  // empty: auto-detect (bomber::assets)
        std::filesystem::path scheme;    // empty: DATA/SCHEMES/BASIC.SCH
        bool demo = false;               // headless scripted run + screenshot
        int demo_ticks = 0;
        std::filesystem::path demo_out;
        // Dev fast-path: skip the front-end and boot straight into a match
        // (also via env BOMBER_BOOT_MATCH). The spine still exists; this just
        // starts the app in the Match state for quick iteration.
        bool boot_match = false;
    };

    explicit GameApp(Options opts) : opts_(std::move(opts)) {}
    // Flushes options.ini on a normal shutdown if anything changed in memory
    // (docs/re/results-and-options.md §2 "Persistence — CONFIRMED via an
    // exit-time write-back": sub_405DE3 only runs through sub_410EBF's
    // atexit-style hook on normal exit, never per-edit). run() calls this
    // itself before returning; the destructor is a backstop for any other
    // exit path (e.g. a test harness that never calls run()'s tail). The
    // write does filesystem I/O that can throw; destructors are implicitly
    // noexcept, so swallow — losing an options write on a failing disk is
    // strictly better than std::terminate (bugprone-exception-escape).
    ~GameApp() {
        try {
            flush_options();
        } catch (...) {}
    }

    // Runs to completion; returns the process exit code.
    int run();

private:
    bool init();
    // PORT ENHANCEMENT (not RE'd — the original has no fullscreen concept):
    // the Alt+Enter/F11 fullscreen toggle, wired as a global SDL_EventFilter
    // (installed once in init()) so it works from every one of this file's
    // per-screen SDL_PollEvent loops without touching each of them. Returns
    // false (swallow) for the toggle keys, true (keep) for everything else —
    // matches SDL_EventFilter's contract, called via the static thunk below
    // since SDL needs a plain function pointer + void* userdata.
    bool handle_global_event(const SDL_Event& ev);
    static bool SDLCALL sdl_event_filter(void* userdata, SDL_Event* event);
    // Flips fullscreen_, applies it to the live window, and marks the choice
    // for persistence (options_dirty_ — flush_options() is the sole writer).
    void toggle_fullscreen();
    // Write-on-exit (task requirement 3 / §2): serializes every in-memory
    // option this session has touched back to options.ini, ONLY if something
    // actually changed since load (options_dirty_) and a game_dir is known.
    // Idempotent — safe to call more than once (run() and the destructor both
    // do, in case a subclass/test skips run()'s normal return path).
    void flush_options();
    void start_match(std::uint32_t seed);
    int run_demo();

    // The front-end screen/state-machine shell (docs/adr/0004): drives
    // Boot -> Logo -> Title -> Menu -> Match -> Results -> Menu around the
    // existing match loop. The pure transition graph lives in app_flow.hpp; the
    // methods below are the thin SDL side (render, audio, input) per state.
    int run_app();
    // Runs one asset-driven Screen (logo/title/results) to completion. sub_42A088
    // CUTS between screens (palette + blit + flip, no wipe), so there is no
    // transition out here — the next screen simply replaces this one. Returns the
    // AppInput that ended it (Advance on key/timeout, Back on Escape, Quit on
    // window close).
    AppInput present_screen(const ScreenDef& def);
    // Runs a `.BM` text-screen (Credits / Options / Network / Controllers help)
    // to completion via the BmScreen viewer: draws MAINMENU as the backdrop with
    // the parsed .BM text+images over it, scrolls on the arrow/page keys, and
    // exits on Enter/Escape (sub_41302D). Returns Back on Escape else Advance
    // (both route the leaf back to the menu), or Quit on window close.
    AppInput present_bm_screen(const std::string& bm_name);
    // The generic help-file browser (sub_41431C -> sub_414235, docs/re/
    // results-and-options.md §4): globs every `*.BM` in the install root and
    // lists them via HelpBrowser (bmscreen.hpp), opening the selection
    // through the same BmScreen viewer present_bm_screen uses. sub_41431C is
    // ONE routine the original wires to F1/row-5 everywhere — CONFIRMED
    // (2026-07-08) called from FOUR sites in our port, all sharing this one
    // non-modal entry point: the main menu's row 5 (present_menu's Enter
    // case, no wipe — mirrors sub_42B9CE's `case 5: sub_41431C(); break;`),
    // the Options screen's F1 (sub_4080DC, present_options_screen — §3), and
    // the editor chooser's F1 (sub_403184, present_editor — §5); the in-round
    // F1 key uses the separate present_help_browser_modal() below instead,
    // since it must freeze the sim rather than draw over MAINMENU (docs/re/
    // in-match-shell.md §1's sub_42A16F(1)/(0) bracket). Gated on getvalue(15)
    // ahead of the glob (HelpBrowser::enter's manual_enabled param). Owns its
    // own nested SDL event loop, same shape as present_bm_screen/
    // present_editor. Returns Quit on window close, else Advance (the browser
    // was cancelled/closed normally).
    AppInput present_help_browser();
    // The same browser, opened mid-round by run_match's F1 key (docs/re/
    // in-match-shell.md §1): identical widget/loop, but the backdrop is the
    // LAST rendered match frame (renderer_->draw_frame) instead of MAINMENU,
    // since the original composites the list dialog over whatever screen was
    // already up rather than cutting to the menu — and the sim is never
    // ticked while this runs (the caller does not call sim_.tick from
    // inside), matching the sub_42A16F(1)/(0) freeze.
    AppInput present_help_browser_modal();
    // The interactive Options screen (Team Play / Conveyor Speed): random
    // GLUE<n> backdrop, FONT6 text, Up/Down select a row, Left/Right change
    // its value, Enter/Esc leave (docs/re/frontend-flow.md "Interactive
    // settings ... DEFERRED" — this is that follow-up). Persists to
    // options.ini via bomber::assets::save_options only when a setting
    // actually changed. F1 opens the generic *.BM help browser
    // (present_help_browser) — CORRECTED 2026-07-08: sub_4080DC's own F1
    // dispatch calls sub_41431C (§4), the SAME browser row 5 opens, not a
    // fixed OPTIONS.BM cut. Returns Advance (both Enter/Esc route the leaf
    // back to the menu, mirroring the other .BM-backed leaves) or Quit on
    // window close.
    AppInput present_options_screen();
    // The key-remap sub-screen (docs/re/results-and-options.md §2,
    // sub_407B9D): a 2x6 scancode-capture grid, reached from the Options
    // screen's "Define keyboard layouts" row. Draws over whatever the caller
    // already painted (present_options_screen's own backdrop, per §2's "no
    // new backdrop call" note) rather than owning one itself. Edits a working
    // copy; on Esc/Enter-to-leave the caller applies it to the live
    // KeyboardMapper AND marks options_dirty_ (write-on-exit, requirement 3)
    // — never writes options.ini directly here.
    void present_keyremap_screen();
    // The hidden scheme editor (docs/re/results-and-options.md §5,
    // sub_403184/sub_4028D2/sub_402595): reached ONLY via present_menu()'s
    // raw Ctrl+E x6 trigger (sub_42B9CE's `++counter > 5` on key code 5) —
    // there is no menu row. Runs the chooser -> (file picker ->) editor ->
    // (powerup sub-editor) nested loop to completion and, on a confirmed
    // save, writes the edited scheme via assets::sch::write() into the
    // install's DATA/SCHEMES dir (never the repo) and reloads scheme_ so the
    // edit is immediately selectable through the existing scheme path.
    void present_editor();
    // The hidden campaign-mode picker (docs/re/campaign.md, sub_4015C6):
    // reached ONLY via present_setup()'s raw 'C'x5 trigger (mirrors
    // present_editor's Ctrl+E x6 pattern) — there is no menu row. Globs
    // `*.cam` in the install root (CampaignFilePicker, campaign_screen.hpp),
    // and on a confirmed selection parses it (assets::res::load_campaign)
    // and, if it yields at least one stage, arms campaign mode: seeds the
    // roster from stage 0's AI count (sub_40151B/sub_422928 semantics —
    // CORRECTED 2026-07-09, see load_campaign_stage below) and sets
    // campaign_active_ so the Play flow skips present_map_select() and
    // auto-advances stages (run_app's Menu/Results handlers). A cancelled
    // picker, an unreadable file, or a file with zero stages leaves campaign
    // mode untouched (port convenience — the original's own error-dialog
    // path for the analogous cases, §3).
    void present_campaign_picker();
    // Loads campaign stage `campaign_stage_index_`'s scheme by name
    // (resolves `<scheme>.SCH` case-insensitively under DATA/SCHEMES,
    // mirroring the case-insensitive glob every other picker already uses)
    // into scheme_, and seeds setup_type_ from the stage's AI count
    // (docs/re/campaign.md "Rover/ghost/AI roster — CORRECTED"): the real
    // per-stage seeder is sub_40151B (gated dword_46489C), NOT sub_42288C
    // (that only clears a per-slot UI latch) — it flips exactly `ai_count`
    // RANDOMLY-chosen OFF slots to COMPUTER (sub_422928: `rand()%10` +
    // retry-on-occupied) and separately spawns `rovers`/`ghosts` as
    // autonomous map-hazard actors (sub_401AAE/sub_401B05) in a particle
    // table libs/sim has no equivalent of — NOT folded into COMPUTER slots
    // (a prior mislabelling, corrected). Also sets campaign_banner_ to the
    // stage's display text (docs/re/campaign.md "Stage banner"). Returns
    // false (and leaves state untouched) if the scheme can't be
    // resolved/loaded, so the caller can bail out of campaign mode cleanly
    // instead of starting a match with a stale board.
    bool load_campaign_stage(int index);
    // The campaign-activation confirmation dialog (sub_4015C6, docs/re/
    // campaign.md "Campaign-activation confirmation dialog"): the REAL
    // sub_43C734-chromed two-line modal the picker shows right after a
    // successful pick/parse — getstring(95)="NOTE!" on top, getstring(1210)=
    // "Campaign Mode Activated!" below (line order CONFIRMED from
    // sub_414340's own draw order plus sub_4015C6's explicit LODWORD
    // assignment, not guessed — see the .cpp comment and the doc section).
    // Dismiss keys mirror sub_414340's key loop exactly: Enter/Space/Escape
    // confirm, every other key is a no-op (dialog stays up). Replaces the
    // former accept-sting stand-in (formerly a coverage-audit.md crumb, now closed).
    AppInput present_campaign_confirm();
    // The campaign stage-start banner (docs/re/campaign.md "Stage banner"):
    // a blocking two-line dialog, "(<stage name>)" (getstring 1235="(%s)")
    // over "Prepare to begin Campaign!" (getstring 1230), shown once per
    // stage transition (both the first stage, from present_campaign_picker,
    // and every auto-advance in run_app's Results handler). Dismissed by any
    // key or a short dwell; presentation-only — a separate sub_414340 call
    // from present_campaign_confirm's above (different getstring ids,
    // different content), but the same dialog FAMILY.
    AppInput present_campaign_banner();
    // The IPLOGO -> HSLOGO -> TITLE boot presentation (sub_42B060). LINEAR — no
    // attract re-run: each screen advances on a key OR the getvalue(12) = 7 s
    // timeout, and the title's Advance (key or timeout) returns so run_app drops
    // into the menu (sub_42B060 synthesizes Enter on timeout and returns; the
    // caller enters sub_42B9CE). Returns Advance to enter the menu, or Back/Quit
    // to short-circuit.
    AppInput run_boot_attract();
    // The navigable main menu (sub_42B9CE): MAINMENU.PCX + an up/down highlight
    // over the item rows, Enter selects, Escape quits. Resolves the highlighted
    // row into a concrete AppInput (StartMatch / OpenOptions / ... / Quit).
    //
    // ALSO owns the ATTRACT-MODE idle timer (docs/re/frontend-flow.md "Attract
    // mode", sub_42B9CE's idle path pseudo.c 30887-30894): getvalue(92) = 30 s
    // (gated > 5, per the file's own legend — < 5 disables attract) of NO
    // key/mouse/pad input resets `menu_idle_since_ms_`'s deadline; hitting it
    // sets attract_, calls roll_attract_match() (the sub_4224E2 save + the
    // roster/stage rolls), and returns StartMatch exactly as if row 0 (Play)
    // had been selected — matching the original's `v10 = 0` force. This is
    // presentation-level gating around the EXISTING Menu->StartMatch edge in
    // app_flow.hpp; no new AppState/AppInput was needed (task brief: prefer
    // the existing StartMatch edge). run_app's StartMatch handler checks
    // attract_ and skips the goldman wheel / present_setup / present_map_select
    // (doc: "neither the player screen nor the LEVEL & ROUNDS screen is
    // shown"), going straight into run_match with the rolled roster/stage.
    AppInput present_menu();
    // Attract-mode entry (sub_4224E2's save + sub_410F81's attract branch,
    // doc "Attract mode" point 1): snapshots the CURRENT roster/level/team
    // selections into attract_saved_ (so the player's own choices are
    // untouched, doc "Menu re-entry restores everything" / sub_422552), then
    // overwrites setup_type_/setup_sub_/setup_team_ with a random 3..10
    // COMPUTER-only roster and selected_level_ with a random stage that
    // BYPASSES the VALUELST 1150-1160 enable flags (input.hpp's
    // fill_attract_roster/attract_stage_pick — pure helpers, unit-tested).
    // Also forces team_play_ off (doc: "forces team play off") and clears any
    // pending campaign/goldman state so they stay inert for the duration (doc
    // point 2's "campaign trigger inert during attract" requirement + goldman
    // §2's `!dword_464938` gate — the wheel is skipped by run_app's own
    // attract_ check at the StartMatch call site, not by clearing
    // gold_player_ here, so a real pending prize still survives an attract
    // interlude). The two rolls advance attract_lcg_, a dedicated
    // presentation LCG (never State::rng) — same shape as setup_lcg_/
    // goldman_lcg_ elsewhere in this file.
    void roll_attract_match();
    // Attract-mode exit (sub_422552, doc "Menu re-entry restores everything"):
    // writes attract_saved_ back over setup_type_/setup_sub_/setup_team_/
    // selected_level_/team_play_ and clears attract_. Called on EVERY path
    // back to the menu after an attract match — both a natural round end
    // (doc point 2's "Round end skips ALL outcome screens": DRAW/RESULTS/
    // VICTORY never render, so run_app's Results branch is bypassed entirely
    // for an attract round) and an input-triggered abort (doc point 3: "ANY
    // key/mouse/pad input ... aborts immediately back to the menu", run_match
    // below). Idempotent no-op if attract_ is already false.
    void restore_from_attract();
    // Runs one match to its end (one player left or time up). Returns Quit if
    // the window closed mid-match, else MatchOver.
    //
    // In attract_ mode this ALSO returns MatchOver the instant ANY key,
    // mouse-button, or gamepad-button input arrives (docs/re/frontend-flow.md
    // "Attract mode" point 3 / doc's abort requirement, mirroring sub_42A3F6's
    // round-loop tail `if (dword_464938) goto LABEL_34` on a keypress) — a
    // real (non-attract) match only reacts to the specific keys already wired
    // above (Ctrl+Q, Esc, F1), so this abort check is additive and attract_-
    // gated, never firing for a human-played round. run_app's StartMatch
    // caller calls restore_from_attract() unconditionally once this returns,
    // whether the round ended naturally or was aborted (doc point 2 "Round
    // end skips ALL outcome screens" applies to BOTH exits — attract never
    // reaches Results).
    AppInput run_match();

    // The in-round "player row" HUD strip (docs/re/in-match-shell.md "The
    // player row" — corrects that document's earlier "no score/kill HUD
    // element exists" claim, which missed this block inside sub_420F07):
    // for every slot that has ever been in this match, draws "S:<wins>
    // K:<kills>" in that player's own colour at a 5-column x 2-row grid
    // across the top of the screen (VALUELST 113-119), overlaying the
    // MISC.ANI "xxx" marker on a slot that is dead THIS round. Called once
    // per rendered frame from run_match, after Renderer::draw_frame — needs
    // GameApp's own win_count_/kill_count_/front_font_/seqs_, none of which
    // Renderer owns (CLAUDE.md's libs/game boundary: Renderer reads sim
    // State + events only).
    void draw_player_row(const sim::State& s);

    // The winner of the round just ended: the sole surviving player's index, or
    // -1 for a draw (no survivor, or the clock ran out). Drives the Results
    // screen's DRAW-vs-VICTORY choice and the "player N wins" naming.
    int round_winner() const;

    // Campaign round-pacing clauses 4-5 (docs/re/campaign.md "Round pacing",
    // sub_4016DA): true when every PRESENT, ALIVE slot is COMPUTER
    // (setup_type_[i] == 1), i.e. no human/joystick player survives this
    // round — regardless of whether an AI side is still alive and would
    // otherwise be sim::winning_side()'s pick. The original force-ends (and,
    // via `--dword_4648B0` undoing sub_40133F's next `++`, REPLAYS) the
    // stage the instant this holds, so an AI "winning" a campaign round with
    // no human left standing must NOT be credited as a win. Strictly wider
    // than round_winner()'s plain draw (mutual total wipeout) — this also
    // fires when a COMPUTER side is the sole sim-declared survivor.
    bool campaign_no_human_survivor() const;

    // True when at least two ACTIVE players share a MatchConfig team
    // (docs/re/setup-screens.md dword_464964). Factored out so run_app's
    // Results handler (the VICTORY-vs-scoreboard decision) and
    // present_scoreboard (the scoreboard's own clinch/outcome-line render)
    // agree on the SAME team_mode/win_by_kills gate — a divergence here would
    // let the two disagree about whether the match is over.
    bool is_team_mode() const;
    // The §1 v73 match-clinch check, factored so run_app's Results handler
    // and present_scoreboard call the identical predicate: the default
    // win-count clinch, or (team mode + options_.win_by_kills) the
    // kill_count_ clinch via results.hpp's win_by_kills_clinch(). Returns the
    // clinching player's index, or -1 if the match is not yet decided.
    int match_clinch() const;

    // Reset the per-match win tally + read the win target getvalue(310) at the
    // start of a fresh match (Menu -> StartMatch). Best-of-N, N = 2 by default.
    void reset_match_scores();
    // The between-round RESULTS scoreboard (sub_42A3F6): RESULTS.PCX + the
    // running per-player win counts at the getvalue(785) list positions. Shown
    // after a round that did not end the match; returns the dismiss input.
    AppInput present_scoreboard();
    // Screen 1 of the pre-match flow — PLAYER INPUT TYPE SELECTION (sub_410F81):
    // the 10-slot input-type list (OFF / COMPUTER / KEYBOARD) at getvalue 705-713,
    // each slot tinted with its intrinsic colour (VALUELST 200-247), a per-slot
    // team flag ('T'). Right cycles a slot's type, Left/'0' set it OFF. Returns
    // Advance to go on to the level screen, Back to cancel to the menu, Quit on
    // window close. (docs/re/setup-screens.md.)
    AppInput present_setup();
    // The Goldman Roulette wheel (docs/re/goldman-roulette.md), sub_4034BC:
    // run at the head of the Play flow, before present_setup(), whenever
    // goldman is on, we're not in attract, it's a local game, AND a gold
    // player is pending (gold_player_ >= 0, doc §2's re-entry gate — the
    // wheel is a silent no-op with no pending winner). Awards +1 born-with
    // inventory (MatchConfig::born_with_extra, doc §4) to the gold player
    // (whole team in team mode) at every subsequent round init for the
    // following match. Returns Advance to continue into present_setup, Back
    // if Esc aborted the wheel (the caller must then skip the whole Play
    // flow and forfeit the gold player, doc §2/§5), Quit on window close.
    AppInput present_goldman_wheel();
    // Screen 2 — LEVEL & ROUNDS (sub_406DDE, the VALUELST "OPTIONS SCREEN"): the
    // RANDOM + 11 named levels and the win target, at getvalue 735-738. Left/Right
    // cycle the highlighted row, Up/Down switch rows, Enter commits the level
    // (selected_level_) + win target (win_target_), Escape backs to present_setup.
    // Returns Advance to start the match, Back to the player screen, Quit on close.
    AppInput present_map_select();
    // A random GLUE<n> backdrop name (sub_4148E5: getvalue(16) count, rand()%%n).
    // Shared by both pre-match screens; uses the presentation LCG, not State::rng.
    std::string pick_glue();
    // Advance a slot's input type one step in the setup cycle (sub_421E80):
    // OFF -> COMPUTER -> KEYBOARD sub 0 -> KEYBOARD sub 1 -> JOY0..JOY<n-1> ->
    // OFF, where n = gamepads_.count() (docs/re/setup-screens.md). Delegates to
    // the pure cycle_slot_input_type (input.hpp) so the wrap order is unit-
    // tested without SDL.
    void cycle_input_type(int slot);
    // A slot bound to JOYSTICK sub reads GamepadMapper::read(sub); a slot bound
    // to KEYBOARD sub 0/1 reads the shared KeyboardMapper's player 0/1 half;
    // OFF/COMPUTER slots get neutral input (AI/absent drives them elsewhere).
    // Assembles the full TickInputs for sim_.tick() each match tick.
    sim::TickInputs collect_inputs() const;
    // GENERIC fallback name for built-in level `idx` (the real names load from the
    // user's MESSAGES.TXT via getstring(150+idx); these are ours, never committed).
    static const char* level_fallback(int idx);

    int menu_index_ = 0;  // highlighted main-menu row (persists across visits)
    // The hidden scheme-editor trigger's same-key repeat counter (§5,
    // sub_42B9CE pseudo.c 30876-30883): raw key code 5 (Ctrl+E) increments
    // it; ANY OTHER key resets it to 0; `++counter > 5` (the 6th consecutive
    // press) opens the editor. Lives here (not a local in present_menu)
    // because it must persist across that function's per-frame event pump.
    int editor_trigger_count_ = 0;
    // The hidden campaign picker's same-key repeat counter (docs/re/
    // campaign.md §4, sub_410F81 pseudo.c 15357-15365): raw key 'C' (0x43)
    // increments it; ANY OTHER key resets it to 0; the 5th CONSECUTIVE press
    // (`== 5`, not editor_trigger_count_'s `> 5` — the doc pins "5 consecutive
    // 'C'", not a 6th) opens the campaign picker. Lives here for the same
    // reason editor_trigger_count_ does: it must persist across
    // present_setup's per-frame event pump. The original also gates this on
    // "not net mode" (sub_40C06A()); this port has no netplay (ADR-0003
    // defers it), so that guard is always-true here and simply omitted.
    int campaign_trigger_count_ = 0;

    // Multi-round match state (sub_42A3F6): best-of-N. win_count_ tallies round
    // wins per player; reaching win_target_ ends the MATCH (VICTORY). A draw
    // scores nobody and replays. Presentation-only state — never sim::State,
    // never hashed. win_target_ is seeded from options.ini's "num_to_win_match="
    // (docs/re/results-and-options.md §3/§5) when present, else getvalue(310),
    // by reset_match_scores(), and then owned by the LEVEL & ROUNDS screen
    // (present_map_select, WINS row 1..100, docs/re/setup-screens.md); the
    // in-class 2 only covers the dev fast-path (--match / BOMBER_BOOT_MATCH),
    // which skips the pre-match screens entirely. A round that does not decide
    // the match routes Results -> Match via AppInput::RoundContinue through the
    // pure flow graph (app_flow.hpp) — run_app folds the scoreboard/draw
    // dismissal into that event; there is no side-channel state override.
    std::array<int, sim::kMaxPlayers> win_count_{};
    int win_target_ = 2;
    // Kill tally (docs/re/results-and-options.md §1, sub_421B0F's field):
    // the RESULTS row shows this alongside the match win count. §1's
    // "Reproduction status" paragraph is explicit that this counter, like the
    // win count, is "carried across rounds within one match" — i.e. despite
    // being called the "round-kill count", it is CUMULATIVE for the whole
    // match (packed in the same per-player 152-byte record as the win count),
    // NOT reset every round. So this resets only in reset_match_scores() (a
    // fresh match), exactly like win_count_. Tallied from the sim's
    // PlayerDied events (Event::data = killer index, event.hpp) once per tick
    // in run_match via results.hpp's tally_kills() — self-kills are excluded
    // (our semantics; §1 does not pin this — see results.hpp's doc comment).
    std::array<int, sim::kMaxPlayers> kill_count_{};
    // options.ini "num_to_win_match=" (§3/§5), read once in init(). Seeds
    // reset_match_scores()'s win_target_ default when getvalue(310) is
    // absent; the LEVEL & ROUNDS screen's WINS row still overrides per-match.
    std::optional<int> num_to_win_match_;

    // Per-slot input type chosen in the PLAYER INPUT screen (sub_410F81):
    // 0 = OFF, 1 = COMPUTER, 2 = KEYBOARD, 3 = JOYSTICK (human) — the original's
    // player byte +16 (docs/re/setup-screens.md). Default: P1 keyboard + P2
    // computer. SlotInputType (input.hpp) names these.
    std::array<int, sim::kMaxPlayers> setup_type_{2, 1};
    // Per-slot input SUB-index (the original's +17): for KEYBOARD, which key-set
    // (0 or 1, both bound to the single physical KeyboardMapper); for JOYSTICK,
    // which CONNECTED gamepad index (GamepadMapper::read(sub)).
    std::array<int, sim::kMaxPlayers> setup_sub_{};
    // Per-slot TEAM (the original's +84, toggled by 'T'): 0 or 1. Fed into the
    // config's non-hashed MatchConfig::team[]; team MODE itself is deferred.
    // Defaulted here to all-0 (matches "every slot OFF" at construction /
    // campaign roster reset); present_setup() re-derives the REAL default
    // (alternating slot & 1, sub_4049C0) on every entry to the setup screen
    // — see that function's comment.
    std::array<int, sim::kMaxPlayers> setup_team_{};
    // The level chosen on the LEVEL screen (sub_406DDE dword_45E0B8/464998):
    // -1 = RANDOM (keep pick_stage over the enabled rotation), else 0..10 = a
    // specific built-in level whose stage index start_match uses directly.
    int selected_level_ = -1;
    // Presentation RNG for the glue pick (and the LEVEL & ROUNDS preview
    // swatch's per-cell tile re-roll). The literal below is only a
    // construction-time placeholder: GameApp::init() overwrites it (and the
    // three sibling LCGs in this file) with a real per-process seed from
    // random_boot_seed() (game_app.cpp), matching the original's boot-time
    // `time_(); srand_();` (sub_41095A, pseudo.c 14610-14611/14639-14640 —
    // the same wall-clock reseed docs/re/facts.md "Per-match brick fill"
    // already cites). A hardcoded literal here would replay the exact same
    // "random" sequence on every launch; `next_seed_` below has the same
    // shape and feeds `match::pick_stage`, so leaving it constant was the
    // root cause of the reported "RANDOM level always picks the same map"
    // bug. Presentation-only: never bomber::sim::State::rng.
    std::uint32_t setup_lcg_ = 0x5E7C0DE5u;

    // ATTRACT MODE (docs/re/frontend-flow.md "Attract mode", sub_42B9CE's idle
    // path + sub_410F81's attract branch, dword_464938). Presentation/config-
    // only, like campaign_active_ below — never sim::State, never hashed; the
    // sim runs the demo match through the ordinary start_match seed path, so
    // determinism (ADR-0003) is untouched.
    //
    // `menu_idle_since_ms_` is present_menu's own idle clock, reset to "now"
    // on every real key/mouse/pad event it sees — separate from a Screen's
    // getvalue(12)=7s dwell (this is getvalue(92)=30s, a different id/timer).
    // present_menu compares elapsed time against it every frame and fires
    // attract once it exceeds getvalue(92)*1000 ms (gated > 5 s, doc: "< 5
    // disables attract"). 0 is a sentinel meaning "not yet initialised for
    // this menu visit" — present_menu seeds it to the current tick on entry.
    std::uint64_t menu_idle_since_ms_ = 0;
    // dword_464938: true for the duration of an attract demo match. Set by
    // roll_attract_match() (present_menu's idle-timeout branch), read by
    // run_app's StartMatch handler (skip the goldman wheel / present_setup /
    // present_map_select / the Results outcome screens) and by run_match
    // (abort on any input). Cleared by restore_from_attract().
    bool attract_ = false;
    // The roster/level/team snapshot roll_attract_match() saves before
    // overwriting them for the demo roster (sub_4224E2), restored by
    // restore_from_attract() (sub_422552) — doc: "Menu re-entry restores
    // everything", so the player's own pre-attract choices survive untouched.
    struct AttractSaved {
        std::array<int, sim::kMaxPlayers> type{};
        std::array<int, sim::kMaxPlayers> sub{};
        std::array<int, sim::kMaxPlayers> team{};
        int level = -1;
        bool team_play = false;
    };
    AttractSaved attract_saved_{};
    // Dedicated presentation LCG (never State::rng) for the two attract rolls
    // (roster-count, stage) — same shape as setup_lcg_/goldman_lcg_. Reseeded
    // per-process by GameApp::init() (random_boot_seed(), see setup_lcg_'s
    // comment above); the literal is only the construction-time placeholder.
    std::uint32_t attract_lcg_ = 0x0A77AC70u;

    // Campaign mode (docs/re/campaign.md, dword_46489C): armed only by the
    // 'C'x5 trigger + a successful *.cam pick on present_setup
    // (present_campaign_picker). Presentation/config-only, like
    // setup_type_/selected_level_ above — never sim::State, never hashed.
    // While active, campaign_stages_[campaign_stage_index_]'s scheme/roster
    // REPLACE the manual setup_type_/selected_level_ values for the
    // duration (the doc's "replacing the normal manual level-pick and
    // roster-pick screens"), and run_app's Menu/Results handlers skip
    // present_map_select() and auto-advance dword_4648B0 between stages
    // instead of returning to the menu.
    bool campaign_active_ = false;                              // dword_46489C
    std::vector<assets::res::CampaignStage> campaign_stages_;    // parsed .CAM (dword_45E010)
    int campaign_stage_index_ = 0;                               // dword_4648B0
    // Stage display banner text, "(<stage name>)" (sub_40133F, getstring
    // 1235="(%s)" — docs/re/campaign.md "Stage banner"), set by
    // load_campaign_stage each time a campaign stage is (re)loaded. Drawn by
    // present_setup for one frame-cycle at stage start alongside getstring
    // 1230="Prepare to begin Campaign!"; empty when campaign mode is off.
    std::string campaign_banner_;

    // The Goldman wheel's pending gold player (dword_46492C, docs/re/goldman-
    // roulette.md §2): -1 = none pending, else a player index (solo) or a
    // RAW 0/1 team id (team mode — our port's team-id space, unlike the
    // original's internal 0/2 encoding; see doc §2) whose match-win, under
    // the goldman option, arms the next Play entry's wheel spin. Default -1
    // (boot init, doc's "Cleared to -1 by ... boot init 14661").
    //
    // ASSIGNMENT (doc §2, pseudo.c 30004-30022): written in run_app's
    // Results case on every SURVIVOR round (w >= 0 — a draw never reaches
    // the original's dword_46492C write and leaves this untouched), from
    // match_clinch()'s v73 — the MATCH-CLINCH winner (win_target_/
    // win_by_kills reached), NOT the per-round winner `w`. In team mode it's
    // setup_team_[clinched], the raw team byte of the clinching player
    // (mirrors present_scoreboard's own clinched_player -> setup_team_[]
    // lookup). Cleared here on the documented events this file owns (Esc on
    // the wheel, Esc on present_setup, the Options-screen Gold Bomberman
    // toggle).
    int gold_player_ = -1;
    // The prize awarded by the last successful (non-aborted) wheel spin, or
    // -1 (doc §4: "dword_45E02C is never reset on consumption"). Consumed by
    // start_match() into MatchConfig::born_with_extra every round while
    // gold_player_ stays the same match's winner.
    int gold_prize_ = -1;
    // Presentation RNG seed for the wheel's 5 draws; reseeded per-process by
    // GameApp::init() (random_boot_seed(), see setup_lcg_'s comment above).
    std::uint32_t goldman_lcg_ = 0x60D1BEEFu;

    Options opts_;

    assets::sch::Scheme scheme_;
    assets::res::ValueList values_;
    sim::Tuning base_tuning_;  // VALUELST-applied (colors, stage rotation, taunts)

    // The full options.ini snapshot this session is editing in memory
    // (docs/re/results-and-options.md §2/§3). Loaded once in init(); every
    // Options-screen row edits `options_` (via OptionsSnapshot round-trips in
    // present_options_screen) and sets options_dirty_ rather than writing the
    // file — flush_options() (run()'s tail / the destructor) is the ONLY
    // writer, matching the confirmed exit-time write-back semantics.
    OptionsSnapshot options_{};
    bool options_dirty_ = false;
    // The Conveyor Speed game-option index actually applied to a fresh match's
    // Tuning (dword_464930). Mirrors options_.conveyor_speed_index once
    // loaded/edited; kept as a separate optional so "never set" (no
    // options.ini key, ever) still falls back to Tuning's own confirmed
    // default (1 = medium) rather than OptionsSnapshot's arbitrary default.
    std::optional<int> conveyor_speed_index_;
    // Team Play toggle (dword_464964). Mirrors options_.team_play; the
    // game-type-level GATE, separate from each slot's own setup_team_[]
    // (+84) byte. start_match() zeroes every slot's MatchConfig::team[] when
    // this is false, regardless of what setup_team_[] holds (there is no
    // separate MatchConfig::team_play field — team[]'s all-zero/non-zero
    // state IS the hashed Player::team gate, docs/re/setup-screens.md
    // "Roster/level -> match"). is_team_mode() also gates on this directly.
    bool team_play_ = false;
    // The install-root options.ini path resolved in init(), used only by
    // flush_options() (the write-on-exit hook, §2). Empty when no game_dir
    // was resolvable (init() already failed in that case).
    std::filesystem::path options_path_;
    // PORT ENHANCEMENT — "fullscreen=" (see init()'s window-creation comment
    // and toggle_fullscreen()): not one of the original's confirmed 22
    // options.ini keys, since the 1997 binary has no fullscreen mode at all.
    // Loaded once in init(), applied to the window there, flipped by
    // Alt+Enter/F11 (handle_global_event), persisted by flush_options().
    bool fullscreen_ = false;

    std::optional<sdl::VideoSubsystem> video_;
    sdl::WindowPtr window_;
    sdl::RendererPtr sdl_renderer_;

    AssetStore assets_;
    SequenceSet seqs_;
    AudioEngine audio_;
    SoundDirector sounds_{audio_};
    std::optional<Renderer> renderer_;
    KeyboardMapper keyboard_;
    // JOYSTICK <n> slots (docs/re/setup-screens.md type==3). Refreshed once
    // after SDL_INIT_GAMEPAD in init() and again on every hotplug event so the
    // setup screen's joystick pane / type-cycle count stays live.
    GamepadMapper gamepads_;

    // Front-end presentation (constructed after assets_ is loaded in init()).
    std::optional<Screen> screen_;
    std::optional<Transition> transition_;
    FontTextures front_font_;  // FONT6.FON glyph textures for the .BM screens
    // Per-match seed, advanced each round (`start_match(next_seed_++)`) and
    // fed to `match::build_match_config`'s per-candidate brick fill/spawn
    // shuffle AND `match::pick_stage`'s RANDOM level pick. GameApp::init()
    // reseeds this from random_boot_seed() (see setup_lcg_'s comment above);
    // the literal below is only the construction-time placeholder — leaving
    // it fixed was the bug that made a fresh process's first RANDOM level
    // pick (and first match's brick layout) identical on every launch.
    std::uint32_t next_seed_ = 0xB0BB1E5;

    sim::Simulation sim_;
};

}  // namespace bomber::game
