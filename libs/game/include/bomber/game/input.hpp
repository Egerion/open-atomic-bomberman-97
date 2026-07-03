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

}  // namespace bomber::game
