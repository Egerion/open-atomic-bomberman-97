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

    // A player dies (sub_41DBFE): scatter EVERY accumulated powerup above the
    // start-with baseline, in kind order. Called from every death site (flame,
    // wall crush, rover). See the .cpp for the timing divergence.
    void death_scatter(Player& p);

private:
    // One head-hit drop: roll a kind the player holds above its baseline (rand %
    // 15, up to 200 tries — every try draws), remove it, scatter its token.
    void scatter_one_rolled_surplus(Player& p);

    // Mutual-exclusion eviction (sub_41E16A). Only ever called with flag kinds.
    void evict(Player& p, PowerupType t);

    // Player bytes +86..+96: a small integer for bombs/flame/skate, 0/1 for the
    // flag kinds, 0 for kinds with no per-kind count. Shared by head_hit's
    // surplus test and death_scatter's surplus loop.
    int held_count(const Player& p, int kind) const;

    // Mirrors sub_41DBFE's write-back, which touches ONLY the count byte and not
    // the derived speed stat — a dead player's speed is never read again.
    void reset_to_baseline(Player& p, int kind, int baseline);

    // sub_41F29B's per-tick `base + skates*gv(90) - clogs*gv(91)`, baked. Called
    // on any Skate apply/remove; NOT by the death reset (byte-write-only).
    void recompute_speed(Player& p);

    State& s_;
};

}  // namespace bomber::sim
