#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "bomber/assets/campaign.hpp"  // assets::res::CampaignStage
#include "bomber/sim/constants.hpp"    // sim::kMaxPlayers

// Seam 2 (ADR-0009 §"shared front-end state"): the non-service state the PLAYER
// INPUT TYPE SELECTION screen (present_setup + cycle_input_type, sub_410F81)
// reads/writes, bundled so SetupScreen can be its own class without threading a
// GameApp& — GameApp owns the members and hands a fresh SetupState to the runner
// ctor alongside the ScreenContext services bundle. A cheap value type
// (references only), copied by value into the runner; the referenced members are
// GameApp members that outlive every screen. ScreenContext stays front-end-
// service-only, so the setup-specific mutable state lives here instead.
//
// The reference set is EXACTLY what present_setup + cycle_input_type touch:
//  - setup_type/setup_sub/setup_team — the 10-slot roster (dword_46481C): the
//    type list Right cycles / Left/'0' clears, each slot's KEYBOARD/JOYSTICK
//    sub-index, and the per-slot team flag 'T' toggles. reset_setup_teams reseeds
//    setup_team on entry (the alternating 0/1 default, sub_4049C0).
//  - setup_lcg — the shared presentation LCG (never sim::State::rng): advanced by
//    pick_glue for the GLUE<n> backdrop. Shared with every other pre-match screen
//    (frontend_util.hpp's pick_glue); the draw order/count is observable.
//  - campaign_trigger_count — the hidden 'C'×5 counter (sub_410F81 pseudo.c
//    15357-15365) that arms the campaign picker; lives on GameApp so it persists
//    across the per-frame event pump.
//  - team_play — the game-type TEAM gate (dword_464964): drives the per-row team
//    marker and the start-guard's "at least two teams" test.
//  - gold_player — Escape aborts the whole Play flow and forfeits the pending
//    Goldman winner (dword_46492C = -1, goldman-roulette.md §2's "Cleared to -1
//    by" list — "Esc on the player-setup screen").
//  - campaign_active/campaign_stages/campaign_stage_index — Escape also tears down
//    any campaign armed by THIS visit's 'C'×5 pick that hasn't started a match yet
//    (docs/re/campaign.md "Campaign-exit key"). These three ALSO live in
//    CampaignState, which SetupScreen carries ONLY to construct the picker; the
//    two reference bundles independently alias the same GameApp members, which is
//    benign — the body reads/writes them through state_ so every direct member
//    access on this screen goes through one seam (campaign_ is used solely as the
//    CampaignPickerScreen ctor argument).

namespace bomber::game {

struct SetupState {
    std::array<int, sim::kMaxPlayers>& setup_type;             // GameApp::setup_type_
    std::array<int, sim::kMaxPlayers>& setup_sub;              // GameApp::setup_sub_
    std::array<int, sim::kMaxPlayers>& setup_team;             // GameApp::setup_team_
    std::uint32_t& setup_lcg;                                  // GameApp::setup_lcg_ (pick_glue)
    int& campaign_trigger_count;                               // GameApp::campaign_trigger_count_
    bool& team_play;                                           // GameApp::team_play_
    int& gold_player;                                          // GameApp::gold_player_ (Esc forfeit)
    bool& campaign_active;                                     // GameApp::campaign_active_
    std::vector<assets::res::CampaignStage>& campaign_stages;  // GameApp::campaign_stages_
    int& campaign_stage_index;                                 // GameApp::campaign_stage_index_
};

}  // namespace bomber::game
