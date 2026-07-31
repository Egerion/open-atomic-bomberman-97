#pragma once

#include "bomber/sim/state.hpp"

namespace bomber::sim {

// The HURRY phase: when the match clock reaches the threshold, solid wall
// tiles drop in a clockwise spiral from the top-left, crushing whatever they
// land on (VALUELST ids 27/46/101).
//
// It takes no FlameSystem, deliberately. It used to hold one and never read it:
// a wall landing on a bomb sets that bomb's fuse to 1 rather than calling
// queue_chain (see drop_wall's justification), so the dependency was declared,
// injected and dead. A constructor that asks for a collaborator it never uses
// misreports the dependency graph.
class EnclosureSystem {
public:
    explicit EnclosureSystem(State& s) : s_(s) {}

    // Tick step: arms the hurry phase at the threshold and drops due walls.
    void update();

    // Spiral geometry (also exposed as sim::enclose_total / enclose_pos).
    static int total(int depth);
    static bool position(int index, int depth, int* x, int* y);

private:
    void drop_wall(int wx, int wy);

    State& s_;
};

}  // namespace bomber::sim
