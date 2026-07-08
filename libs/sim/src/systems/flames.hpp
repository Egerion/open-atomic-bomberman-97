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
    // Ignites the exploding bomb's own tile unconditionally (sub_42331C
    // epicentre block). Distinct from spread_to: no bomb/powerup occupancy
    // stop applies here, only to the extending arm.
    bool ignite_epicentre(int tx, int ty, std::uint8_t owner);

    // A flame ARM reaches (tx,ty) (sub_42331C per-direction loop). Returns
    // true if the arm continues past this cell, false if it stops here
    // (bomb chain-detonated, powerup burned, solid wall, or brick ignited).
    bool spread_to(int tx, int ty, std::uint8_t owner);

    // Destroys any floor powerup at (tx,ty), with the diseases_destroyable
    // skull-relocation compensation. Shared by the epicentre and the arm.
    void burn_powerup_here(int tx, int ty);

    State& s_;
    PowerupSystem& powerups_;
};

}  // namespace bomber::sim
