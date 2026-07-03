#include "bomber/game/input.hpp"

#include <SDL3/SDL.h>

namespace bomber::game {

sim::TickInputs KeyboardMapper::read() const {
    const bool* keys = SDL_GetKeyboardState(nullptr);
    sim::TickInputs in;
    auto& p0 = in.players[0];
    p0.up = keys[SDL_SCANCODE_UP];
    p0.down = keys[SDL_SCANCODE_DOWN];
    p0.left = keys[SDL_SCANCODE_LEFT];
    p0.right = keys[SDL_SCANCODE_RIGHT];
    p0.action1 = keys[SDL_SCANCODE_RCTRL] || keys[SDL_SCANCODE_SPACE];
    p0.action2 = keys[SDL_SCANCODE_RSHIFT];
    auto& p1 = in.players[1];
    p1.up = keys[SDL_SCANCODE_W];
    p1.down = keys[SDL_SCANCODE_S];
    p1.left = keys[SDL_SCANCODE_A];
    p1.right = keys[SDL_SCANCODE_D];
    p1.action1 = keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_E];
    p1.action2 = keys[SDL_SCANCODE_LSHIFT];
    return in;
}

sim::TickInputs demo_inputs(int t) {
    sim::TickInputs in;
    int phase = (t / 25) % 4;
    auto& p0 = in.players[0];
    p0.right = phase == 0;
    p0.down = phase == 1;
    p0.left = phase == 2;
    p0.up = phase == 3;
    p0.action1 = (t % 50) == 24;
    auto& p1 = in.players[1];
    p1.left = phase == 0;
    p1.up = phase == 1;
    p1.right = phase == 2;
    p1.down = phase == 3;
    p1.action1 = (t % 60) == 30;
    return in;
}

}  // namespace bomber::game
