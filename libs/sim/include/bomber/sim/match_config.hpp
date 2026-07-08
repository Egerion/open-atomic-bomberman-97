#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "bomber/sim/constants.hpp"
#include "bomber/sim/tuning.hpp"
#include "bomber/sim/types.hpp"

namespace bomber::sim {

struct SpawnPoint {
    int x = 0, y = 0;
};

// Everything needed to set up one match deterministically. Built by hand in
// tests or from a .SCH scheme + VALUELST via bomber::match::build_match_config.
struct MatchConfig {
    std::array<std::array<Cell, kGridWidth>, kGridHeight> cells{};
    // Stage "extra" actors (conveyors/trampolines/dirarrows/warpholes), parsed
    // from EXTRA<N>.RES (docs/re/stage-actors.md). Default None everywhere. Copied
    // verbatim into State at setup; actor_dir is a godir where meaningful.
    std::array<std::array<ActorType, kGridWidth>, kGridHeight> actor_type{};
    std::array<std::array<std::uint8_t, kGridWidth>, kGridHeight> actor_dir{};
    // Warphole exit tile per Warphole cell (sub_405A81 idno/linkto resolution),
    // pre-computed by apply_actors so the sim needs no RNG for a warp. 0 where
    // actor_type != Warphole. Copied into State::warp_dest_* at setup.
    std::array<std::array<std::uint8_t, kGridWidth>, kGridHeight> warp_dest_x{};
    std::array<std::array<std::uint8_t, kGridWidth>, kGridHeight> warp_dest_y{};
    std::vector<SpawnPoint> spawns;   // indexed by player number
    int player_count = 2;
    // Per-player computer-AI flag (ADR-0005): true → the AISystem drives this
    // slot's PlayerInput instead of a human. Copied to Player::ai at setup.
    std::array<bool, kMaxPlayers> ai{};
    // Per-player active flag (setup screen): false = OFF (slot excluded from the
    // match). Filled all-true in the ctor so hand-built configs (tests/golden)
    // that only set player_count keep their contiguous 0..count-1 roster.
    std::array<bool, kMaxPlayers> active{};
    // Per-player team, from the PLAYER INPUT screen's +84 byte (sub_4223E7,
    // toggled by 'T'; docs/re/setup-screens.md). CONFIG ONLY and deliberately
    // NOT copied into any hashed Player field: team MODE (win/friendly-fire/AI
    // logic on a hashed Player::team) is a separate deferred effort (docs/re/
    // ai.md), so this stays out of state_hash() — golden byte-identical. Default
    // 0 (single team) everywhere; captured now so the roster is complete and the
    // sim side can be wired later without another setup-screen pass.
    std::array<std::uint8_t, kMaxPlayers> team{};
    std::uint32_t seed = 0x12345678;
    Tuning tuning;
    // Per-scheme powerup overrides (-P rows): >= -999 replaces the spawn count.
    std::array<std::int32_t, kPowerupKinds> spawn_override;
    std::array<bool, kPowerupKinds> forbidden{};
    std::array<bool, kPowerupKinds> born_with{};

    MatchConfig() {
        spawn_override.fill(kNoOverride);
        active.fill(true);  // default roster = contiguous player_count (tests/golden)
        // ActorType::None is 255, not the zero-initialised DirArrow(0).
        for (auto& row : actor_type) row.fill(ActorType::None);
    }

    static constexpr std::int32_t kNoOverride = -1000;
};

}  // namespace bomber::sim
