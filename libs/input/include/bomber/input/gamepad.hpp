#pragma once

#include <cstdint>
#include <vector>

#include "bomber/sim/types.hpp"

// Forward-declared so this header stays SDL-free, like input.hpp/KeyboardMapper.
struct SDL_Gamepad;

// Gamepad input mapping (SDL3 gamepad API) — the JOYSTICK slot type on the
// PLAYER INPUT TYPE SELECTION screen (docs/re/setup-screens.md, sub_410F81
// type==3, "sub_429628(i) present" joystick pane). Mirrors KeyboardMapper's
// shape: read() -> one PlayerInput per tick for a bound stick index.
//
// D-pad OR left stick drive the 4 directions, whichever is pressed:
// sub_429628-era controllers were digital-or-analog interchangeably and the
// original has no facility to prefer one over the other. South (A/Cross) is
// action1, East (B/Circle) action2.

namespace bomber::game {

class GamepadMapper {
public:
    GamepadMapper() = default;
    ~GamepadMapper();
    GamepadMapper(const GamepadMapper&) = delete;
    GamepadMapper& operator=(const GamepadMapper&) = delete;

    // Rescans the connected pads. MUST be called after SDL_INIT_GAMEPAD and on
    // every SDL_EVENT_GAMEPAD_ADDED/REMOVED, or count() goes stale and the
    // setup screen offers sticks that are no longer there.
    void refresh();

    // How many JOYSTICK <n> slots the setup screen's type-cycle should offer
    // (sub_421E80 "joystick advances through present sticks then wraps").
    int count() const { return static_cast<int>(pads_.size()); }

    // SDL_GetGamepadName, or a generic fallback — feeds the joystick pane's
    // per-stick line (msg 41, getstring(41)+i).
    const char* name(int index) const;

    // One tick's input for JOYSTICK <index>. Out of range (unplugged mid-match,
    // or an index the pane never had) is NEUTRAL input, never a throw: a
    // disconnect must not crash the match loop.
    sim::PlayerInput read(int index) const;

private:
    std::vector<SDL_Gamepad*> pads_;
};

}  // namespace bomber::game
