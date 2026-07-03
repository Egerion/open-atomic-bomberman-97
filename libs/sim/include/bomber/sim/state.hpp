#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "bomber/sim/bomb.hpp"
#include "bomber/sim/constants.hpp"
#include "bomber/sim/event.hpp"
#include "bomber/sim/player.hpp"
#include "bomber/sim/tuning.hpp"
#include "bomber/sim/types.hpp"

namespace bomber::sim {

// The complete deterministic gameplay state (ADR-0003). Value semantics:
// copying a State is a legal snapshot, state_hash() digests every gameplay
// field, and identical (State, inputs) sequences replay identically.
struct State {
    std::uint64_t tick = 0;
    std::uint32_t rng = 0x12345678;
    std::int32_t ticks_left = 0;  // match countdown; 0 = time up (draw)
    bool hurry = false;           // walls are closing in
    std::int32_t enclose_index = 0;
    std::int32_t enclose_timer = 0;
    std::int32_t enclose_interval = 0;
    Tuning tuning;

    std::array<std::array<Cell, kGridWidth>, kGridHeight> cells{};
    // Powerup hidden under a brick (revealed when the brick burns away).
    std::array<std::array<PowerupType, kGridWidth>, kGridHeight> hidden{};
    // Powerup lying revealed on the floor.
    std::array<std::array<PowerupType, kGridWidth>, kGridHeight> floor{};
    // Remaining ticks of flame in a cell (0 = none).
    std::array<std::array<std::uint8_t, kGridWidth>, kGridHeight> flame{};
    // Which player's bomb produced the flame (valid while flame > 0).
    std::array<std::array<std::uint8_t, kGridWidth>, kGridHeight> flame_owner{};
    // Remaining ticks of a brick crumbling (blocks until it reaches 0).
    std::array<std::array<std::uint8_t, kGridWidth>, kGridHeight> burning{};

    std::array<Player, kMaxPlayers> players{};
    std::vector<Bomb> bombs;

    // Cleared at the start of every tick; excluded from state_hash().
    std::vector<Event> events;
};

}  // namespace bomber::sim
