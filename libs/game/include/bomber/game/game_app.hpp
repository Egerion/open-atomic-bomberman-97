#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

#include "bomber/assets/reslist.hpp"
#include "bomber/assets/sch.hpp"
#include "bomber/game/app_flow.hpp"
#include "bomber/game/asset_store.hpp"
#include "bomber/game/audio_engine.hpp"
#include "bomber/game/bmscreen.hpp"
#include "bomber/game/gamepad.hpp"
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

class GameApp {
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
    // exit path (e.g. a test harness that never calls run()'s tail).
    ~GameApp() { flush_options(); }

    // Runs to completion; returns the process exit code.
    int run();

private:
    bool init();
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
    // The interactive Options screen (Team Play / Conveyor Speed): random
    // GLUE<n> backdrop, FONT6 text, Up/Down select a row, Left/Right change
    // its value, Enter/Esc leave (docs/re/frontend-flow.md "Interactive
    // settings ... DEFERRED" — this is that follow-up). Persists to
    // options.ini via bomber::assets::save_options only when a setting
    // actually changed. F1 opens the original OPTIONS.BM help overlay on top
    // (present_bm_screen), same as the rest of the front end. Returns Advance
    // (both Enter/Esc route the leaf back to the menu, mirroring the other
    // .BM-backed leaves) or Quit on window close.
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
    AppInput present_menu();
    // Runs one match to its end (one player left or time up). Returns Quit if
    // the window closed mid-match, else MatchOver.
    AppInput run_match();

    // The winner of the round just ended: the sole surviving player's index, or
    // -1 for a draw (no survivor, or the clock ran out). Drives the Results
    // screen's DRAW-vs-VICTORY choice and the "player N wins" naming.
    int round_winner() const;

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
    std::array<int, sim::kMaxPlayers> setup_team_{};
    // The level chosen on the LEVEL screen (sub_406DDE dword_45E0B8/464998):
    // -1 = RANDOM (keep pick_stage over the enabled rotation), else 0..10 = a
    // specific built-in level whose stage index start_match uses directly.
    int selected_level_ = -1;
    std::uint32_t setup_lcg_ = 0x5E7C0DE5u;  // presentation RNG for the glue pick

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
    // Team Play toggle (dword_464964). Mirrors options_.team_play; threaded
    // into MatchConfig::team_play at start_match() (config-only, not consumed
    // by build_state() yet — see MatchConfig::team_play's doc comment).
    bool team_play_ = false;
    // The install-root options.ini path resolved in init(), used only by
    // flush_options() (the write-on-exit hook, §2). Empty when no game_dir
    // was resolvable (init() already failed in that case).
    std::filesystem::path options_path_;

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
    std::uint32_t next_seed_ = 0xB0BB1E5;  // per-match seed, advanced each round

    sim::Simulation sim_;
};

}  // namespace bomber::game
