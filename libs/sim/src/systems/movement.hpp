#pragma once

#include "bomber/sim/state.hpp"

namespace bomber::sim {

// Player movement — a faithful port of the original per-pixel stepper
// (sub_41EC84 in BM95.EXE). See docs/re/facts.md "Player movement".
class MovementSystem {
public:
    explicit MovementSystem(State& s) : s_(s) {}

    // Moves one player for one tick in direction d (also sets facing).
    // Positions are integer field pixels; the speed budget is added per tick
    // and spent 100 units per one-pixel step, remainder carried over.
    void move(Player& p, Direction d);

private:
    State& s_;
};

}  // namespace bomber::sim
