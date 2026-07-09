#pragma once

#include "bomber/sim/state.hpp"

namespace bomber::sim {

// Per-level tile regeneration (VALUELST ids 340-350 + 695): on Haunted House
// (the file's own comment calls it "cemetary/mortuary", level index 7),
// destroyed bricks slowly regrow at random blank tiles that are clear of any
// live player. A faithful port of sub_426704 (called from the enclosure
// stepper sub_426818). Inert (zero RNG draws, zero state change) on every
// other level, whose regen_seconds is 0. See docs/re/facts.md "Per-level
// tile regeneration".
class TileRegenSystem {
public:
    explicit TileRegenSystem(State& s) : s_(s) {}

    // Tick step: counts down the per-level regen timer; at 0, makes ONE
    // regen attempt (up to 100 random candidate tiles, stopping at the
    // first eligible one) and resets the timer.
    void update();

private:
    State& s_;
};

}  // namespace bomber::sim
