#include "bomber/game/game_app.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>

#include "bomber/assets/install.hpp"
#include "bomber/match/match_factory.hpp"

namespace bomber::game {

namespace fs = std::filesystem;

bool GameApp::init() {
    fs::path game = !opts_.game_dir.empty() ? opts_.game_dir : assets::default_game_dir();
    if (game.empty() || !fs::is_directory(game / "DATA")) {
        std::fprintf(stderr,
                     "usage: bomber_game [game_dir] [scheme.sch]\n"
                     "(or set BOMBER_GAME_DIR / gamedir.txt)\n");
        return false;
    }
    opts_.game_dir = game;
    fs::path scheme_path =
        !opts_.scheme.empty() ? opts_.scheme : game / "DATA" / "SCHEMES" / "BASIC.SCH";

    try {
        scheme_ = assets::sch::load(scheme_path);
        values_ = assets::res::load_values(game / "DATA" / "RES" / "VALUELST.RES");
        if (const char* env = std::getenv("BOMBER_GAME_SECONDS"); env && *env)
            values_.values[100] = std::atoi(env);  // testing hook
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return false;
    }

    video_.emplace();
    if (!video_->ok()) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return false;
    }
    SDL_Window* win = nullptr;
    SDL_Renderer* ren = nullptr;
    if (!SDL_CreateWindowAndRenderer("Open Bomberman", kScreenW * 2, kScreenH * 2, 0, &win,
                                     &ren)) {
        std::fprintf(stderr, "SDL_CreateWindowAndRenderer: %s\n", SDL_GetError());
        return false;
    }
    window_.reset(win);
    sdl_renderer_.reset(ren);
    SDL_SetRenderLogicalPresentation(ren, kScreenW, kScreenH,
                                     SDL_LOGICAL_PRESENTATION_LETTERBOX);

    if (!assets_.load(ren, game)) return false;
    seqs_.resolve(assets_);

    if (!opts_.demo && !audio_.init(game))
        std::fprintf(stderr, "audio unavailable, continuing silent\n");

    base_tuning_ = match::build_match_config(scheme_, 2, 0, &values_).tuning;
    assets_.build_player_sets(base_tuning_.color_rgb);
    seqs_.resolve(assets_);  // re-resolve: player sprite sets exist now

    renderer_.emplace(ren, assets_, seqs_);
    return true;
}

void GameApp::start_match(std::uint32_t seed) {
    sim_ = sim::Simulation(match::build_match_config(scheme_, 2, seed, &values_));
    int stage = match::pick_stage(base_tuning_, seed);
    if (assets_.load_stage(stage)) {
        seqs_.resolve_stage(assets_, stage);
        audio_.start_music(1100 + stage);  // SOUNDLST: stage music = 1100 + n
    }
    renderer_->reset_match();
    sounds_.reset();
}

int GameApp::run_demo() {
    for (int t = 0; t < opts_.demo_ticks; ++t) {
        sim_.tick(demo_inputs(t));
        sounds_.on_tick(sim_.state());
        renderer_->on_events(sim_.state());
        renderer_->draw_frame(sim_.state());  // keeps walk-anim sampling in sync
    }
    SDL_Surface* shot = SDL_RenderReadPixels(sdl_renderer_.get(), nullptr);
    int rc = 1;
    if (shot) {
        rc = SDL_SaveBMP(shot, opts_.demo_out.string().c_str()) ? 0 : 1;
        SDL_DestroySurface(shot);
        std::printf("demo: %d ticks, alive %d, screenshot %s\n", opts_.demo_ticks,
                    sim::alive_count(sim_.state()), opts_.demo_out.string().c_str());
    }
    return rc;
}

int GameApp::run_interactive() {
    const std::uint64_t tick_ms = 1000 / sim::kTicksPerSecond;
    std::uint64_t last = SDL_GetTicks();
    std::uint64_t acc = 0;
    int over_ticks = -1;
    std::uint32_t next_seed = 0xB0BB1E5;
    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) running = false;
            if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE) running = false;
        }

        std::uint64_t now = SDL_GetTicks();
        acc += now - last;
        last = now;
        while (acc >= tick_ms) {
            acc -= tick_ms;
            sim_.tick(keyboard_.read());
            sounds_.on_tick(sim_.state());
            renderer_->on_events(sim_.state());

            const sim::State& s = sim_.state();
            if (over_ticks < 0 && (sim::alive_count(s) <= 1 || s.ticks_left == 0)) {
                over_ticks = 3 * sim::kTicksPerSecond;
                if (s.ticks_left == 0) {
                    std::printf("time up — draw!\n");
                } else {
                    audio_.play_random_in_range(2000, 2299);  // "we have a winner"
                    for (int i = 0; i < sim::kMaxPlayers; ++i)
                        if (s.players[i].present && s.players[i].alive)
                            std::printf("player %d wins!\n", i);
                }
            }
            if (over_ticks > 0 && --over_ticks == 0) {
                start_match(++next_seed);
                over_ticks = -1;
            }
        }

        audio_.update_music();
        renderer_->draw_frame(sim_.state());
        SDL_RenderPresent(sdl_renderer_.get());
        SDL_Delay(2);
    }
    return 0;
}

int GameApp::run() {
    if (!init()) return opts_.game_dir.empty() ? 2 : 1;
    start_match(0xB0BB1E5);
    return opts_.demo ? run_demo() : run_interactive();
}

}  // namespace bomber::game
