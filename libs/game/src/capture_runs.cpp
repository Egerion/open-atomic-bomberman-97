#include "capture_runs.hpp"

#include <algorithm>  // std::max
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "bomber/game_util/anim_pace.hpp"
#include "bomber/game_util/log.hpp"
#include "bomber/input/input.hpp"  // demo_inputs
#include "bomber/render/sprites.hpp"
#include "bomber/ui/dialog_chrome.hpp"

namespace bomber::game {

namespace fs = std::filesystem;

namespace {

// Cursor anchor over MAINMENU.PCX — CONFIRMED getvalue(700/701) (sub_42B9CE: X
// from getvalue(700), Y from getvalue(701)). Read live from VALUELST (columns of
// the multi-value row 700, legend "X, Y - first item / YS - y-spacing"); these
// fallbacks are the reference install's values so a stripped VALUELST still
// positions sanely.
constexpr int kMenuCursorXFallback = 332;  // getvalue(700)
constexpr int kMenuCursorYFallback = 140;  // getvalue(701)

// Names Event::Type for BOMBER_DEMO_TRACE. Only ever used to help a human pick
// --demo-shots tick numbers (tests/visual/README.md "Deriving new shot ticks"),
// never for gameplay logic.
const char* event_type_name(sim::Event::Type t) {
    switch (t) {
        case sim::Event::Type::BombPlaced: return "BombPlaced";
        case sim::Event::Type::BombKicked: return "BombKicked";
        case sim::Event::Type::Explosion: return "Explosion";
        case sim::Event::Type::BrickDestroyed: return "BrickDestroyed";
        case sim::Event::Type::PowerupRevealed: return "PowerupRevealed";
        case sim::Event::Type::PowerupPicked: return "PowerupPicked";
        case sim::Event::Type::PowerupBurned: return "PowerupBurned";
        case sim::Event::Type::PlayerDied: return "PlayerDied";
        case sim::Event::Type::TimeUp: return "TimeUp";
        case sim::Event::Type::Hurry: return "Hurry";
        case sim::Event::Type::WallClosed: return "WallClosed";
        case sim::Event::Type::BombPunched: return "BombPunched";
        case sim::Event::Type::BombBounced: return "BombBounced";
        case sim::Event::Type::BombGrabbed: return "BombGrabbed";
        case sim::Event::Type::BombThrown: return "BombThrown";
        case sim::Event::Type::HeadHit: return "HeadHit";
        case sim::Event::Type::Infected: return "Infected";
        case sim::Event::Type::BombStopped: return "BombStopped";
        case sim::Event::Type::JellyBounced: return "JellyBounced";
        case sim::Event::Type::TrampolineBounce: return "TrampolineBounce";
        case sim::Event::Type::WarpUsed: return "WarpUsed";
        case sim::Event::Type::RoverSpawned: return "RoverSpawned";
        case sim::Event::Type::RoverDied: return "RoverDied";
        case sim::Event::Type::RoverKilledPlayer: return "RoverKilledPlayer";
        case sim::Event::Type::TileRegrew: return "TileRegrew";
        case sim::Event::Type::DropRefused: return "DropRefused";
    }
    return "?";
}

// Read the backbuffer and write it as a BMP. Every capture path below calls this
// BEFORE presenting: SDL swaps on present, leaving the read target undefined.
bool save_screenshot(SDL_Renderer* ren, const fs::path& out) {
    SDL_Surface* shot = SDL_RenderReadPixels(ren, nullptr);
    if (!shot) return false;
    const bool ok = SDL_SaveBMP(shot, out.string().c_str());
    SDL_DestroySurface(shot);
    return ok;
}

// The scripted demo match. Both capture modes tick the SAME deterministic script
// (demo_inputs, input.cpp) — --demo-shots just adds save points along the way, so
// the legacy final-frame screenshot at a given tick count is byte-identical to
// what --demo alone would have produced.
class DemoRun {
public:
    explicit DemoRun(const CaptureSlots& slots)
        : s_(slots), trace_(std::getenv("BOMBER_DEMO_TRACE") != nullptr) {}

    int run() {
        if (trace_)
            log_warn("tuning: fuse=%d flame=%d brick_burn=%d", s_.sim.state().tuning.fuse_frames,
                     s_.sim.state().tuning.flame_frames, s_.sim.state().tuning.brick_burn_frames);
        return s_.opts.demo_shots.empty() ? run_single_shot() : run_shot_series();
    }

private:
    void advance(int t) {
        s_.sim.tick(demo_inputs(t));
        if (trace_)
            for (const auto& e : s_.sim.state().events)
                log_warn("  t=%d %s player=%d (%d,%d) data=%d", t + 1, event_type_name(e.type),
                         e.player, e.x, e.y, e.data);
        s_.sounds.on_tick(s_.sim.state());
        s_.renderer.on_events(s_.sim.state());
        s_.renderer.draw_frame(s_.sim.state());  // keeps walk-anim sampling in sync
    }

    // Every shot whose tick is the one just reached. A tick may carry more than
    // one label, and a label is written the moment its tick lands.
    bool save_shots_at(int reached) {
        bool all_ok = true;
        for (const auto& [label, tick] : s_.opts.demo_shots) {
            if (tick != reached) continue;
            const fs::path out = s_.opts.demo_shot_dir / (label + ".bmp");
            const bool ok = save_screenshot(s_.sdl, out);
            all_ok = all_ok && ok;
            std::printf("demo-shots: tick %d (%s) alive %d -> %s%s\n", reached, label.c_str(),
                        sim::alive_count(s_.sim.state()), out.string().c_str(),
                        ok ? "" : " (FAILED)");
        }
        return all_ok;
    }

    int run_shot_series() {
        int max_tick = 0;
        for (const auto& [label, tick] : s_.opts.demo_shots) max_tick = std::max(max_tick, tick);
        int rc = 0;
        for (int t = 0; t < max_tick; ++t) {
            advance(t);
            if (!save_shots_at(t + 1)) rc = 1;
        }
        return rc;
    }

    int run_single_shot() {
        for (int t = 0; t < s_.opts.demo_ticks; ++t) advance(t);
        const bool ok = save_screenshot(s_.sdl, s_.opts.demo_out);
        if (ok)
            std::printf("demo: %d ticks, alive %d, screenshot %s\n", s_.opts.demo_ticks,
                        sim::alive_count(s_.sim.state()), s_.opts.demo_out.string().c_str());
        return ok ? 0 : 1;
    }

    const CaptureSlots& s_;
    bool trace_;
};

void draw_menu_backdrop(const CaptureSlots& s) {
    SDL_SetRenderDrawColor(s.sdl, 0, 0, 0, 255);
    SDL_RenderClear(s.sdl);
    const Sprite& bg = s.assets.frontend_pcx("MAINMENU");
    if (!bg.tex) return;
    SDL_FRect d{0, 0, static_cast<float>(bg.w), static_cast<float>(bg.h)};
    SDL_RenderTexture(s.sdl, bg.tex, nullptr, &d);
}

}  // namespace

int run_demo_capture(const CaptureSlots& s) {
    return DemoRun(s).run();
}

int run_bm_capture(const CaptureSlots& s) {
    BmScreen bm(s.assets, s.front_font);
    bm.enter(s.opts.bm_shot_name);
    for (int i = 0; i < s.opts.bm_shot_scroll; ++i) bm.on_key(SDLK_DOWN);
    draw_menu_backdrop(s);
    bm.draw(s.sdl);
    const bool ok = save_screenshot(s.sdl, s.opts.bm_shot_out);
    if (ok)
        std::printf("bm-shot: %s scroll %d -> %s\n", s.opts.bm_shot_name.c_str(),
                    s.opts.bm_shot_scroll, s.opts.bm_shot_out.string().c_str());
    return ok ? 0 : 1;
}

int run_menu_capture(const CaptureSlots& s) {
    draw_menu_backdrop(s);
    s.front_font.draw_outlined(s.sdl, "V1.0", SDL_FPoint{0, 0},
                               OutlinedTextStyle{{168, 168, 164}, {}, 50.0f});
    const int cx = static_cast<int>(s.values.column_or(700, 0, kMenuCursorXFallback));
    const int cy = static_cast<int>(s.values.column_or(700, 1, kMenuCursorYFallback));
    const Anim cur = resolve_sequence(s.assets.trigbomb(-1), "bomb trigger green");
    if (!cur.steps.empty()) {
        const Sprite& sp = cur.steps[anim_step_index(0, cur.steps.size())];
        if (sp.tex) {
            SDL_FRect d{static_cast<float>(cx - sp.hx), static_cast<float>(cy - sp.hy),
                        static_cast<float>(sp.w), static_cast<float>(sp.h)};
            SDL_RenderTexture(s.sdl, sp.tex, nullptr, &d);
        }
    }
    const bool ok = save_screenshot(s.sdl, s.opts.menu_shot_out);
    if (ok) std::printf("menu-shot -> %s\n", s.opts.menu_shot_out.string().c_str());
    return ok ? 0 : 1;
}

}  // namespace bomber::game
