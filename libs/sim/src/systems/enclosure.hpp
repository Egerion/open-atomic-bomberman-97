#pragma once

#include "bomber/sim/state.hpp"

namespace bomber::sim {

class FlameSystem;

// The HURRY phase: when the match clock reaches the threshold, solid wall
// tiles drop in a clockwise spiral from the top-left, crushing whatever they
// land on (VALUELST ids 27/46/101).
class EnclosureSystem {
public:
    EnclosureSystem(State& s, FlameSystem& flames) : s_(s), flames_(flames) {}

    // Tick step: arms the hurry phase at the threshold and drops due walls.
    void update();

    // Spiral geometry (also exposed as sim::enclose_total / enclose_pos).
    static int total(int depth);
    static bool position(int index, int depth, int* x, int* y);

private:
    void drop_wall(int wx, int wy);

    State& s_;
    FlameSystem& flames_;
};

}  // namespace bomber::sim
