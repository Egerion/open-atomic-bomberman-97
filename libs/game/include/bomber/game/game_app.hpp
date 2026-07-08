#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <utility>

#include "bomber/assets/reslist.hpp"
#include "bomber/assets/sch.hpp"
#include "bomber/game/app_flow.hpp"
#include "bomber/game/asset_store.hpp"
#include "bomber/game/audio_engine.hpp"
#include "bomber/game/bmscreen.hpp"
#include "bomber/game/input.hpp"
#include "bomber/game/options_screen.hpp"
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

    int menu_index_ = 0;  // highlighted main-menu row (persists across visits)

    Options opts_;

    assets::sch::Scheme scheme_;
    assets::res::ValueList values_;
    sim::Tuning base_tuning_;  // VALUELST-applied (colors, stage rotation, taunts)
    // The Conveyor Speed game-option index parsed from the install's options.ini
    // ("conveyor_speed="; dword_464930). Empty when the file/key is absent, in
    // which case the sim keeps the binary's confirmed default (1 = medium). See
    // start_match() and docs/re/stage-actors.md §3.
    std::optional<int> conveyor_speed_index_;
    // Team Play toggle, loaded from options.ini ("team_play=") at startup and
    // editable live from the interactive Options screen (present_options_screen).
    // Threaded into MatchConfig::team_play at start_match() (config-only, not
    // consumed by build_state() yet — see MatchConfig::team_play's doc comment).
    bool team_play_ = false;

    std::optional<sdl::VideoSubsystem> video_;
    sdl::WindowPtr window_;
    sdl::RendererPtr sdl_renderer_;

    AssetStore assets_;
    SequenceSet seqs_;
    AudioEngine audio_;
    SoundDirector sounds_{audio_};
    std::optional<Renderer> renderer_;
    KeyboardMapper keyboard_;

    // Front-end presentation (constructed after assets_ is loaded in init()).
    std::optional<Screen> screen_;
    std::optional<Transition> transition_;
    FontTextures front_font_;  // FONT6.FON glyph textures for the .BM screens
    std::uint32_t next_seed_ = 0xB0BB1E5;  // per-match seed, advanced each round

    sim::Simulation sim_;
};

}  // namespace bomber::game
