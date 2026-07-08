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
    // Team Play toggle, set from the interactive Options screen / options.ini
    // ("team_play="). CONFIG-ONLY, like the rest of MatchConfig: build_state()
    // does not read it and it is never mixed into state_hash() (docs/re/
    // setup-screens.md notes the confirmed CONSUMER is the roster/setup
    // screen's team-mode display, dword_464964, which this codebase has not
    // built yet — see that doc's "Determinism / golden — NO IMPACT" section).
    // A full team MODE (hashed Player::team + sim win/friendly-fire/AI-target
    // logic) remains the documented follow-up in docs/re/ai.md.
    bool team_play = false;
    std::uint32_t seed = 0x12345678;
    Tuning tuning;
    // Per-scheme powerup overrides (-P rows): >= -999 replaces the spawn count.
    std::array<std::int32_t, kPowerupKinds> spawn_override;
    std::array<bool, kPowerupKinds> forbidden{};
    std::array<bool, kPowerupKinds> born_with{};

    MatchConfig() {
        spawn_override.fill(kNoOverride);
        // ActorType::None is 255, not the zero-initialised DirArrow(0).
        for (auto& row : actor_type) row.fill(ActorType::None);
    }

    static constexpr std::int32_t kNoOverride = -1000;
};

}  // namespace bomber::sim
