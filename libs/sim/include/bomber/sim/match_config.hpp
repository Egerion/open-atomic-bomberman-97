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
    // toggled by 'T'; docs/re/setup-screens.md). Copied verbatim into the
    // hashed Player::team at setup (setup.cpp) — team mode now gates AI
    // targeting (docs/re/ai.md §3.4/§5.3) and round-end (docs/re/ai.md TEAM
    // follow-up). Default 0 everywhere; every existing hand-built config
    // (tests/golden) leaves every slot at 0, so a fully-zeroed roster behaves
    // exactly as before this field was wired (our semantics: team mode only
    // engages when two ACTIVE players share a value).
    std::array<std::uint8_t, kMaxPlayers> team{};
    std::uint32_t seed = 0x12345678;
    Tuning tuning;
    // Per-scheme powerup overrides (-P rows): >= -999 replaces the spawn count.
    std::array<std::int32_t, kPowerupKinds> spawn_override;
    std::array<bool, kPowerupKinds> forbidden{};
    std::array<bool, kPowerupKinds> born_with{};
    // Per-player born-with OVERLAY (docs/re/goldman-roulette.md §4/§8): the
    // Goldman wheel's +1 starting-inventory award for the gold player (whole
    // team in team mode), applied at setup.cpp AFTER the global born_with
    // loop above via the same PowerupSystem::apply path. Unlike born_with
    // (global, every player), this is per-SLOT so only the gold
    // player/team receives the bump. Default all-false everywhere: a config
    // with no goldman award behaves byte-identical to before this field
    // existed (golden hashes unaffected).
    std::array<std::array<bool, kPowerupKinds>, kMaxPlayers> born_with_extra{};

    MatchConfig() {
        spawn_override.fill(kNoOverride);
        active.fill(true);  // default roster = contiguous player_count (tests/golden)
        // ActorType::None is 255, not the zero-initialised DirArrow(0).
        for (auto& row : actor_type) row.fill(ActorType::None);
    }

    static constexpr std::int32_t kNoOverride = -1000;
};

}  // namespace bomber::sim
