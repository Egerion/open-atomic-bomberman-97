// bomber_game — playable front-end: renders the deterministic sim with the
// original game's assets (loaded at runtime from the player's own install).
//
//   bomber_game [game_dir] [scheme.sch]
//     Player 0: arrows + Right Ctrl (bomb)     Player 1: WASD + Left Ctrl (bomb)
//     Esc quits. When one player remains, the match restarts after 3 s.
//
//   bomber_game --demo <ticks> <out.bmp> [game_dir] [scheme.sch]
//     Headless verification: scripted inputs, renders frames, saves the last.

#include <SDL3/SDL_main.h>

#include <cstdlib>
#include <string>
#include <vector>

#include "bomber/game/game_app.hpp"

int main(int argc, char** argv) {
    bomber::game::GameApp::Options opts;
    std::vector<std::string> rest;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--demo" && i + 2 < argc) {
            opts.demo = true;
            opts.demo_ticks = std::atoi(argv[++i]);
            opts.demo_out = argv[++i];
        } else {
            rest.push_back(a);
        }
    }
    if (!rest.empty()) opts.game_dir = rest[0];
    if (rest.size() > 1) opts.scheme = rest[1];

    bomber::game::GameApp app(std::move(opts));
    return app.run();
}
