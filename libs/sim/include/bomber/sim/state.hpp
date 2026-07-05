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
    // Next tick a dud roll may fire (global rate limiter, dword_464AF4 in
    // the original — armed at setup, re-armed on every open-gate placement).
    std::uint64_t dud_gate = 0;
    Tuning tuning;
    // Per-scheme forbidden powerups (-P rows). Static per-match config like
    // tuning — excluded from state_hash(). The Random powerup consults it
    // when rerolling (sub_41E21E case 0xC).
    std::array<bool, kPowerupKinds> forbidden{};

    std::array<std::array<Cell, kGridWidth>, kGridHeight> cells{};
    // Stage "extra" actors (EXTRA<N>.RES → docs/re/stage-actors.md). A static
    // per-match layer like cells: parsed at setup, never mutated by the sim,
    // but gameplay-affecting (conveyors push, trampolines bounce) so it IS
    // mixed into state_hash(). actor_dir is a godir (0=Up,1=Right,2=Down,
    // 3=Left) and is only meaningful where actor_type is Conveyor/DirArrow.
    std::array<std::array<ActorType, kGridWidth>, kGridHeight> actor_type{};
    std::array<std::array<std::uint8_t, kGridWidth>, kGridHeight> actor_dir{};
    // Warphole exit tile, one entry per Warphole cell (docs/re/stage-actors.md
    // §5): the partner resolved by sub_405A81's idno/linkto scan, pre-computed
    // at setup so the sim draws no RNG for a warp. A static, hashed per-match
    // input like actor_type; meaningless (0) where actor_type != Warphole.
    std::array<std::array<std::uint8_t, kGridWidth>, kGridHeight> warp_dest_x{};
    std::array<std::array<std::uint8_t, kGridWidth>, kGridHeight> warp_dest_y{};
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
