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
    std::vector<SpawnPoint> spawns;  // indexed by player number
    int player_count = 2;
    // Per-player computer-AI flag (ADR-0005): true → the AISystem drives this
    // slot's PlayerInput instead of a human. Copied to Player::ai at setup.
    std::array<bool, kMaxPlayers> ai{};
    // Per-player active flag (setup screen): false = OFF (slot excluded from the
    // match). Filled all-true in the ctor so hand-built configs (tests/golden)
    // that only set player_count keep their contiguous 0..count-1 roster.
    std::array<bool, kMaxPlayers> active{};
    // Per-player team, from the PLAYER INPUT screen's +84 byte (sub_4223E7,
    // toggled by 'T'). Copied verbatim into the hashed Player::team; team mode
    // only engages when two ACTIVE players share a NONZERO value.
    std::array<std::uint8_t, kMaxPlayers> team{};
    std::uint32_t seed = 0x12345678;
    Tuning tuning;
    // Per-scheme powerup overrides (-P rows): >= -999 replaces the spawn count.
    std::array<std::int32_t, kPowerupKinds> spawn_override;
    std::array<bool, kPowerupKinds> forbidden{};
    // There is deliberately NO `born_with` array. The scheme's -P "born with"
    // field is a COUNT that REPLACES the VALUELST starting inventory — the
    // original expresses it by writing the value-table id the baseline is read
    // from — so it lands in tuning.start_with[], not in a channel of its own
    // (facts.md "The .SCH -P row's 2nd field is a COUNT that REPLACES the
    // starting inventory"; match::build_match_config is the writer).
    //
    // The Goldman wheel's award IS a genuine post-baseline increment
    // (sub_4214BC's `++inventory[86 + prize]`, goldman-roulette.md §4/§8) and is
    // per-SLOT, so it is a separate overlay applied at setup AFTER the baseline.
    std::array<std::array<bool, kPowerupKinds>, kMaxPlayers> born_with_extra{};
    // Clogs (wheel prize id 13) sit outside the kPowerupKinds space — never a
    // scheme/-P/spawn/forbid kind (§9.2) — so they are their own per-player
    // COUNT rather than part of born_with_extra. A count and not a bool because
    // sub_4214BC's per-round inventory RESET runs BEFORE the grant, making the
    // gold player's clogs exactly 0-or-1 EACH round and never a cross-round
    // running total (§9.3, "reset-then-+1", not accumulation).
    std::array<std::int32_t, kMaxPlayers> born_with_clogs{};

    // Campaign rover/ghost hazard counts + speeds (.CAM fields 3-6,
    // docs/re/campaign.md). Zero on every non-campaign config, and
    // RoverSystem::spawn with count <= 0 draws no RNG.
    std::int32_t campaign_rovers = 0;
    std::int32_t campaign_rover_speed = 0;
    std::int32_t campaign_ghosts = 0;
    std::int32_t campaign_ghost_speed = 0;

    MatchConfig() {
        spawn_override.fill(kNoOverride);
        active.fill(true);  // default roster = contiguous player_count (tests/golden)
        // ActorType::None is 255, not the zero-initialised DirArrow(0).
        for (auto& row : actor_type) row.fill(ActorType::None);
    }

    static constexpr std::int32_t kNoOverride = -1000;
};

}  // namespace bomber::sim
