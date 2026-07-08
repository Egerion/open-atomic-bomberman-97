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
#include "bomber/game/renderer.hpp"
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

    // Runs to completion; returns the process exit code.
    int run();

private:
    bool init();
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

    // Multi-round match state (sub_42A3F6): best-of-getvalue(310) = 2 rounds.
    // win_count_ tallies round wins per player; reaching win_target_ ends the
    // MATCH (VICTORY). A draw scores nobody and replays. match_continues_ routes
    // Results -> the next round instead of the menu.
    std::array<int, sim::kMaxPlayers> win_count_{};
    int win_target_ = 2;
    bool match_continues_ = false;

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
    // The Conveyor Speed game-option index parsed from the install's options.ini
    // ("conveyor_speed="; dword_464930). Empty when the file/key is absent, in
    // which case the sim keeps the binary's confirmed default (1 = medium). See
    // start_match() and docs/re/stage-actors.md §3.
    std::optional<int> conveyor_speed_index_;

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
