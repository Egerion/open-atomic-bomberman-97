#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "bomber/assets/campaign.hpp"  // assets::res::CampaignStage
#include "bomber/assets/sch.hpp"       // assets::sch::Scheme
#include "bomber/sim/constants.hpp"    // sim::kMaxPlayers

// The non-service state the PLAYER INPUT TYPE SELECTION screen (sub_410F81)
// reads and writes, bundled by reference so SetupScreen needs no GameApp&.
// GameApp owns the members and outlives every screen; ScreenContext stays
// front-end-service-only, so screen-specific mutable state lives in seams like
// this one.
//
// Three of these carry a rule the field name does not:
//  - setup_team is reseeded TWICE on entry, in the original's own order — the
//    alternating parity default, then the scheme's per-spawn "-S" team field,
//    hence `scheme` below (docs/frontend-setup-screens.md).
//  - setup_lcg is the shared presentation LCG (never sim::State::rng); its draw
//    ORDER and COUNT are observable across screens.
//  - campaign_active/stages/stage_index are ALSO aliased by CampaignState, which
//    this screen carries only to construct the picker. Benign, because the screen
//    body reads and writes them through this bundle.

namespace bomber::game {

struct SetupState {
    std::array<int, sim::kMaxPlayers>& setup_type;  // the 10-slot roster (dword_46481C)
    std::array<int, sim::kMaxPlayers>& setup_sub;   // KEYBOARD/JOYSTICK sub-index
    std::array<int, sim::kMaxPlayers>& setup_team;  // the per-slot team flag 'T' toggles
    const assets::sch::Scheme& scheme;              // the -S per-spawn teams
    std::uint32_t& setup_lcg;                       // pick_glue's backdrop roll
    int& campaign_trigger_count;                    // the hidden 'C'x5 counter
    bool& team_play;                                // the game-type TEAM gate (dword_464964)
    // Escape forfeits any pending Goldman winner (dword_46492C = -1) and tears
    // down a campaign armed by THIS visit that has not started a match yet.
    int& gold_player;
    bool& campaign_active;
    std::vector<assets::res::CampaignStage>& campaign_stages;
    int& campaign_stage_index;
};

}  // namespace bomber::game
