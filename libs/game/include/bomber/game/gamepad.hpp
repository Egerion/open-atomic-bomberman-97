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
//   D-pad or left stick -> the 4 directions (either source, whichever is
//   pressed; sub_429628-era controllers were digital-or-analog interchangeably
//   and the original has no facility to prefer one over the other).
//   South face button (A/Cross) -> action1 (bomb), East face button (B/Circle)
//   -> action2 (throw/grab/trigger/punch) — the two action buttons the
//   keyboard mapper exposes.
//
// Hotplug (SDL_EVENT_GAMEPAD_ADDED/REMOVED) keeps the enumeration live so the
// setup screen's joystick pane and type-cycle reflect what's plugged in right
// now; a mid-match disconnect degrades to neutral input rather than crashing
// (docs task: "handle a pad disconnect mid-match gracefully").

namespace bomber::game {

class GamepadMapper {
public:
    GamepadMapper() = default;
    ~GamepadMapper();
    GamepadMapper(const GamepadMapper&) = delete;
    GamepadMapper& operator=(const GamepadMapper&) = delete;

    // Rescans the connected pads. Call once after SDL_INIT_GAMEPAD and again
    // on every SDL_EVENT_GAMEPAD_ADDED/REMOVED — SDL_GetGamepads() is cheap
    // (an id list), so a full rescan on hotplug is simpler than incremental
    // open/close bookkeeping and just as correct.
    void refresh();

    // How many gamepads are currently open, i.e. how many JOYSTICK <n> slots
    // the setup screen's type-cycle should offer (docs/re/setup-screens.md
    // sub_421E80 "joystick advances through present sticks then wraps").
    int count() const { return static_cast<int>(pads_.size()); }

    // The stick's display name (SDL_GetGamepadName), or a generic fallback —
    // feeds the joystick pane's per-stick line (msg 41, getstring(41)+i).
    const char* name(int index) const;

    // One tick's input for JOYSTICK <index>. Out-of-range (unplugged mid-
    // match, or an index the pane never had) returns neutral input rather
    // than throwing — a disconnect must never crash the match loop.
    sim::PlayerInput read(int index) const;

private:
    std::vector<SDL_Gamepad*> pads_;
};

}  // namespace bomber::game
