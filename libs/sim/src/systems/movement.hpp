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
    void move(Player& p, Direction d) { move(p, d, 0); }

    // As above, plus a signed extra_budget added to (or subtracted from) this
    // tick's move budget before it is spent. Used by the conveyor
    // (StageActorSystem): a belt contributes getvalue(190+idx) 1/100-px units.
    // The disease speed factors apply only to the player's own speed, exactly
    // as in sub_41F29B where the conveyor term is added AFTER those factors.
    void move(Player& p, Direction d, std::int32_t extra_budget);

private:
    State& s_;
};

}  // namespace bomber::sim
