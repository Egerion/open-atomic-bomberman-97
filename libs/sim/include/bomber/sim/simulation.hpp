#pragma once

#include <cstdint>

#include "bomber/sim/match_config.hpp"
#include "bomber/sim/state.hpp"

// Facade over the deterministic 20 Hz simulation. Internally the work is done
// by focused systems (libs/sim/src/systems/*) that all operate on the shared
// State value — see CLAUDE.md "Architecture".

namespace bomber::sim {

class Simulation {
public:
    // Empty simulation (no players present). Used by tools and tests that
    // want to drive a raw State directly through state().
    Simulation() = default;

    // Builds the initial match state: arena cells, players at spawn points,
    // powerups hidden under randomly chosen bricks (seeded RNG — deterministic).
    explicit Simulation(const MatchConfig& config);

    // Advances exactly one tick.
    void tick(const TickInputs& inputs);

    // FNV-1a digest of the entire gameplay state, for tests and future netplay.
    std::uint64_t hash() const;

    // Full state access. Mutable access exists for tests, tools, and loaders;
    // the presentation layer must treat the state as read-only.
    State& state() { return state_; }
    const State& state() const { return state_; }

private:
    State state_;
};

// FNV-1a digest of a raw state (Simulation::hash forwards here).
std::uint64_t state_hash(const State& state);

// Cell queries shared with tools/tests.
bool tile_blocked(const State& state, int tx, int ty);  // walls/bricks/burning
bool tile_has_bomb(const State& state, int tx, int ty);
int alive_count(const State& state);

// Enclosement spiral: how many wall tiles a depth closes, and the grid cell
// the index-th tile lands on (clockwise rings from the outside in).
int enclose_total(int depth);
bool enclose_pos(int index, int depth, int* x, int* y);

}  // namespace bomber::sim
