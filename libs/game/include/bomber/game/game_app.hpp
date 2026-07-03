#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <utility>

#include "bomber/assets/reslist.hpp"
#include "bomber/assets/sch.hpp"
#include "bomber/game/asset_store.hpp"
#include "bomber/game/audio_engine.hpp"
#include "bomber/game/input.hpp"
#include "bomber/game/renderer.hpp"
#include "bomber/game/sdl.hpp"
#include "bomber/game/sequences.hpp"
#include "bomber/game/sound_director.hpp"
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
    };

    explicit GameApp(Options opts) : opts_(std::move(opts)) {}

    // Runs to completion; returns the process exit code.
    int run();

private:
    bool init();
    void start_match(std::uint32_t seed);
    int run_demo();
    int run_interactive();

    Options opts_;

    assets::sch::Scheme scheme_;
    assets::res::ValueList values_;
    sim::Tuning base_tuning_;  // VALUELST-applied (colors, stage rotation, taunts)

    std::optional<sdl::VideoSubsystem> video_;
    sdl::WindowPtr window_;
    sdl::RendererPtr sdl_renderer_;

    AssetStore assets_;
    SequenceSet seqs_;
    AudioEngine audio_;
    SoundDirector sounds_{audio_};
    std::optional<Renderer> renderer_;
    KeyboardMapper keyboard_;

    sim::Simulation sim_;
};

}  // namespace bomber::game
