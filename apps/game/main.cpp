// bomber_game — playable front-end: renders the deterministic sim with the
// original game's assets (loaded at runtime from the player's own install).
//
//   bomber_game [game_dir] [scheme.sch]
//     Boots the front-end: logos -> title (attract loop) -> navigable menu ->
//     match -> results (DRAW / VICTORY<player>) -> menu.
//     Menu: Up/Down (or W/S) highlight, Enter/Space select, Esc quits.
//       Start -> match, Quit -> exit; Options/Controllers/Network/Credits are
//       stubs pending the .BM text-screen parser.
//     Player 0: arrows + Right Ctrl (bomb)     Player 1: WASD + Left Ctrl (bomb)
//     In a screen: Enter/Space advances; Esc backs out (quits from title/menu).
//
//   bomber_game --match [game_dir] [scheme.sch]
//     Dev fast-path: skip the front-end and boot straight into a match (same
//     as env BOMBER_BOOT_MATCH=1). Returns to the menu when the match ends.
//
//   bomber_game --demo <ticks> <out.bmp> [game_dir] [scheme.sch]
//     Headless verification: scripted inputs, renders frames, saves the last.
//
//   bomber_game --demo-shots <label:tick,label:tick,...> <outdir> [game_dir] [scheme.sch]
//     Visual golden harness (tests/visual/): runs the SAME scripted demo
//     match as --demo, saving a named "<label>.bmp" under <outdir> at each
//     requested tick instead of one final frame. All shots share one run, so
//     they stay consistent with each other. See tests/visual/README.md.

#include <SDL3/SDL_main.h>

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "bomber/game/game_app.hpp"

namespace {

// Parses "label:tick,label:tick,..." into (label, tick) pairs. A malformed
// entry (missing ':' or a non-numeric tick) is skipped rather than aborting
// the whole run — the caller simply never reaches that tick's save point.
std::vector<std::pair<std::string, int>> parse_demo_shots(const std::string& spec) {
    std::vector<std::pair<std::string, int>> shots;
    std::size_t start = 0;
    while (start <= spec.size()) {
        std::size_t comma = spec.find(',', start);
        std::string entry = spec.substr(start, comma == std::string::npos ? comma : comma - start);
        std::size_t colon = entry.find(':');
        if (colon != std::string::npos && colon > 0) {
            std::string label = entry.substr(0, colon);
            int tick = std::atoi(entry.c_str() + colon + 1);
            if (tick > 0) shots.emplace_back(std::move(label), tick);
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return shots;
}

}  // namespace

int main(int argc, char** argv) {
    bomber::game::GameApp::Options opts;
    std::vector<std::string> rest;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--demo" && i + 2 < argc) {
            opts.demo = true;
            opts.demo_ticks = std::atoi(argv[++i]);
            opts.demo_out = argv[++i];
        } else if (a == "--demo-shots" && i + 2 < argc) {
            opts.demo = true;
            opts.demo_shots = parse_demo_shots(argv[++i]);
            opts.demo_shot_dir = argv[++i];
        } else if (a == "--match") {
            opts.boot_match = true;  // skip the front-end, boot straight into a match
        } else {
            rest.push_back(a);
        }
    }
    if (!rest.empty()) opts.game_dir = rest[0];
    if (rest.size() > 1) opts.scheme = rest[1];

    bomber::game::GameApp app(std::move(opts));
    return app.run();
}
