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
    // VALUELST start-with baseline back onto random floor tiles. No kind roll,
    // no count roll — the only RNG is the per-token tile selection, in kind
    // order. Called from every death site (flame, wall crush, rover). See the
    // .cpp for the deliberate death-tick vs death-anim-end timing note.
    void death_scatter(Player& p);

private:
    // Mutual-exclusion eviction (sub_41E16A): scatters the evicted token back
    // onto the floor and, for Trigger, downgrades the player's live trigger
    // bombs (sub_424C47). Only ever called with the five flag kinds.
    void evict(Player& p, PowerupType t);

    // Current accumulated count of a powerup kind (player bytes +86..+96 in the
    // original): a small integer for bombs/flame/skate, 0/1 for the flag kinds,
    // 0 for kinds with no per-kind count (disease/superdisease/random). Shared
    // by head_hit's surplus test and death_scatter's surplus loop.
    int held_count(const Player& p, int kind) const;

    // Reset a kind's accumulated count to its start-with baseline (mirrors
    // sub_41DBFE's write-back: it touches ONLY the count byte, not the derived
    // speed stat — a dead player's speed is never read again).
    void reset_to_baseline(Player& p, int kind, int baseline);

    State& s_;
};

}  // namespace bomber::sim
