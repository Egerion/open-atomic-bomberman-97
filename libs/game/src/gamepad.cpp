#include "bomber/game/gamepad.hpp"

#include <SDL3/SDL.h>

namespace bomber::game {

GamepadMapper::~GamepadMapper() {
    for (SDL_Gamepad* pad : pads_)
        if (pad) SDL_CloseGamepad(pad);
}

void GamepadMapper::refresh() {
    for (SDL_Gamepad* pad : pads_)
        if (pad) SDL_CloseGamepad(pad);
    pads_.clear();

    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    if (!ids) return;
    pads_.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        SDL_Gamepad* pad = SDL_OpenGamepad(ids[i]);
        if (pad) pads_.push_back(pad);  // skip ones that fail to open rather than abort
    }
    SDL_free(ids);
}

const char* GamepadMapper::name(int index) const {
    if (index < 0 || index >= static_cast<int>(pads_.size()) || !pads_[static_cast<std::size_t>(index)])
        return "unknown";
    const char* n = SDL_GetGamepadName(pads_[static_cast<std::size_t>(index)]);
    return n ? n : "unknown";
}

sim::PlayerInput GamepadMapper::read(int index) const {
    sim::PlayerInput in;  // neutral by default — covers the out-of-range/disconnect case
    if (index < 0 || index >= static_cast<int>(pads_.size())) return in;
    SDL_Gamepad* pad = pads_[static_cast<std::size_t>(index)];
    if (!pad) return in;

    // D-pad OR the left stick drive the 4 directions — either source counts,
    // matching KeyboardMapper's "any key that means this direction" shape.
    constexpr Sint16 kStickDeadzone = 12000;  // ~37% of the Sint16 range, standard SDL deadzone
    Sint16 ax = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
    Sint16 ay = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
    in.up = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_UP) || ay < -kStickDeadzone;
    in.down = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN) || ay > kStickDeadzone;
    in.left = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT) || ax < -kStickDeadzone;
    in.right = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || ax > kStickDeadzone;

    // Face buttons: south (A/Cross) = action1 (bomb), east (B/Circle) = action2
    // (throw/grab/trigger/punch) — mirrors the keyboard mapper's two action keys.
    in.action1 = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH);
    in.action2 = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST);
    return in;
}

}  // namespace bomber::game
