#pragma once

#include "bomber/sim/types.hpp"

// Input mapping: keyboard state -> TickInputs.
//   Player 0: arrows + Right Ctrl/Space (bomb), Right Shift (action2)
//   Player 1: WASD + Left Ctrl/E (bomb), Left Shift (action2)

namespace bomber::game {

class KeyboardMapper {
public:
    sim::TickInputs read() const;
};

// Scripted inputs for the headless --demo mode (both players walk squares
// and drop bombs periodically).
sim::TickInputs demo_inputs(int t);

// The PLAYER INPUT TYPE SELECTION slot categories (docs/re/setup-screens.md,
// sub_421DD2's player byte +16). Mirrors the original's type numbering so the
// setup-screen switch statements (present_setup) read directly against it.
enum class SlotInputType { Off = 0, Computer = 1, Keyboard = 2, Joystick = 3, Other = 4 };

// Cycle a slot's (type, sub) one step FORWARD (sub_421E80 @0x421E80, CONFIRMED
// shape): off -> computer -> keyboard set 0 -> keyboard set 1 -> joystick 0 ..
// joystick (joystick_count-1) -> off. `joystick_count` is the number of
// CONNECTED sticks (GamepadMapper::count()) — when it is 0 the cycle skips
// straight from keyboard 1 back to off, exactly like the original wraps past
// an empty joystick list (sub_429628 finds none present). Pure/SDL-free so it
// is unit-testable without a window or a physical pad.
constexpr void cycle_slot_input_type(int& type, int& sub, int joystick_count) {
    auto t = static_cast<SlotInputType>(type);
    if (t == SlotInputType::Off) {
        type = static_cast<int>(SlotInputType::Computer);
        sub = 0;
    } else if (t == SlotInputType::Computer) {
        type = static_cast<int>(SlotInputType::Keyboard);
        sub = 0;
    } else if (t == SlotInputType::Keyboard && sub == 0) {
        sub = 1;
    } else if (t == SlotInputType::Keyboard && sub == 1) {
        if (joystick_count > 0) {
            type = static_cast<int>(SlotInputType::Joystick);
            sub = 0;
        } else {
            type = static_cast<int>(SlotInputType::Off);
            sub = 0;
        }
    } else if (t == SlotInputType::Joystick && sub + 1 < joystick_count) {
        ++sub;
    } else {
        // Joystick's last present stick (or any type==4 OTHER, which the
        // original never routes into this cycle either) wraps to off.
        type = static_cast<int>(SlotInputType::Off);
        sub = 0;
    }
}

}  // namespace bomber::game
