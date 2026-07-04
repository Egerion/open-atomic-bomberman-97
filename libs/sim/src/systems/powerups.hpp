#pragma once

#include "bomber/sim/state.hpp"

namespace bomber::sim {

// Powerup effects on players: gaining, losing, and scattering them back onto
// the field (head hits, VALUELST ids 670/671).
class PowerupSystem {
public:
    explicit PowerupSystem(State& s) : s_(s) {}

    // Grants one powerup, respecting the per-kind accumulation limits
    // (VALUELST 550..562). Disease/SuperDisease are handled by DiseaseSystem.
    void apply(Player& p, PowerupType t);

    // Removes one accumulated upgrade of kind t (inverse of apply).
    void remove(Player& p, PowerupType t);

    // Drops a powerup token onto a RANDOM free floor tile (sub_4255B2);
    // the token is lost when placement keeps failing, as in the original.
    void scatter(PowerupType t);

    // A bomb bonks a player on the head (sub_421F7E): 16-tick stun and
    // powers_lost_min + rand % powers_lost_rand kind-rolled drops.
    void head_hit(int victim, int tx, int ty);

private:
    State& s_;
};

}  // namespace bomber::sim
