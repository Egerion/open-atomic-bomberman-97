#pragma once

#include <cstddef>
#include <cstdint>

#include "bomber/sim/state.hpp"

namespace bomber::sim {

class PowerupSystem;

// Explosions and their aftermath: flame spread, chain reactions, brick
// burning, and the per-tick fade of flames / crumbling bricks.
class FlameSystem {
public:
    // powerups: the skull-relocation compensation when a flame destroys a
    // Disease token while diseases_destroyable is off reuses
    // PowerupSystem::scatter (the same sub_4255B2 the head-hit drop uses).
    FlameSystem(State& s, PowerupSystem& powerups) : s_(s), powerups_(powerups) {}

    // Detonates the bomb at bombs[bomb_index] (no-op if already inactive):
    // frees the owner's slot, spreads flame in all four directions, and chains
    // into any bomb the flame reaches within the same tick.
    void explode(std::size_t bomb_index);

    // Tick step: flames fade; crumbling bricks finish and reveal powerups.
    void age_flames_and_bricks();

private:
    // A flame reaches (tx,ty). Returns true if it continues past this cell.
    bool spread_to(int tx, int ty, std::uint8_t owner);

    State& s_;
    PowerupSystem& powerups_;
};

}  // namespace bomber::sim
