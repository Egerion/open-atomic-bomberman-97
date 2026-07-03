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
    std::vector<SpawnPoint> spawns;   // indexed by player number
    int player_count = 2;
    std::uint32_t seed = 0x12345678;
    Tuning tuning;
    // Per-scheme powerup overrides (-P rows): >= -999 replaces the spawn count.
    std::array<std::int32_t, kPowerupKinds> spawn_override;
    std::array<bool, kPowerupKinds> forbidden{};
    std::array<bool, kPowerupKinds> born_with{};

    MatchConfig() { spawn_override.fill(kNoOverride); }

    static constexpr std::int32_t kNoOverride = -1000;
};

}  // namespace bomber::sim
