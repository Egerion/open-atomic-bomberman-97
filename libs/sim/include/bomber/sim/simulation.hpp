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

    // PORT-ONLY, NON-DETERMINISTIC live-feel path (F9 native-cadence mode, see
    // simulation.cpp). Advances the movement/AI pass by one DISPLAYED frame of
    // `delta_ms` measured wall-clock time and drains the 50 ms systems pass off
    // an internal accumulator. Reproduces the original's per-frame gameplay
    // driver (sub_42A191) for responsiveness testing; NOT for tests/oracle,
    // which must use tick() (the deterministic fixed 20 Hz entry).
    void frame(const TickInputs& inputs, std::int32_t delta_ms);

    // Real-time carried in frame()'s 50 ms systems accumulator, in ms [0,50).
    // The F9 render path uses accum/kMsPerTick as the interpolation fraction for
    // 50 ms-stepped entities (flying/sliding bombs, rovers) so they glide
    // instead of stepping at 20 Hz while movement runs per frame. Meaningless on
    // the tick() path (stays 0).
    std::int32_t systems_accum_ms() const { return systems_accum_ms_; }

    // FNV-1a digest of the entire gameplay state, for tests and future netplay.
    std::uint64_t hash() const;

    // Full state access. Mutable access exists for tests, tools, and loaders;
    // the presentation layer must treat the state as read-only.
    State& state() { return state_; }
    const State& state() const { return state_; }

private:
    State state_;
    // Real-time carry for frame()'s 50 ms systems accumulator. Non-hashed,
    // non-deterministic scratch — only the F9 live path touches it.
    std::int32_t systems_accum_ms_ = 0;
};

// FNV-1a digest of a raw state (Simulation::hash forwards here).
std::uint64_t state_hash(const State& state);

// Cell queries shared with tools/tests.
bool tile_blocked(const State& state, int tx, int ty);  // walls/bricks/burning
bool tile_has_bomb(const State& state, int tx, int ty);
int alive_count(const State& state);

// Round-end, team-aware (our semantics — not RE'd beyond the +84 byte's
// existence; docs/re/ai.md TEAM follow-up / docs/re/setup-screens.md). Players
// with the same NONZERO Player::team value count as one side; team 0 never
// merges with another team-0 player, so every player is its own side on an
// all-zero roster — identical to the old "one player left" rule. Use these
// instead of alive_count() for round-over / winner decisions so a match with
// teammates does not end the round while two teammates are the only survivors.
//
// sides_remaining: the number of distinct alive sides still present. A round
// is over when this is <= 1, exactly where alive_count() <= 1 used to gate it
// (and identical to it on an all-zero roster, since side == player there).
int sides_remaining(const State& state);

// The representative player slot of the sole remaining side (the lowest slot
// index on that side), or -1 if the round is not decided as a win (zero or
// more than one side alive — mutual wipe-out or an ongoing round). Mirrors
// GameApp::round_winner()'s single-survivor contract, generalised to sides:
// solo players are their own side, so a solo match's winner is unchanged.
int winning_side(const State& state);

// Enclosement spiral: how many wall tiles a depth closes, and the grid cell
// the index-th tile lands on (clockwise rings from the outside in).
int enclose_total(int depth);
bool enclose_pos(int index, int depth, int* x, int* y);

}  // namespace bomber::sim
