#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "bomber/assets/campaign.hpp"
#include "bomber/assets/sch.hpp"
#include "bomber/game_util/frontend_util.hpp"  // reload_scheme
#include "bomber/game_util/results.hpp"        // seed_campaign_ai_slots
#include "bomber/sim/constants.hpp"       // sim::kMaxPlayers

// The non-service state the hidden campaign flow reads and writes, bundled by
// reference so neither the picker nor load_campaign_stage needs a GameApp&.

namespace bomber::game {

struct CampaignState {
    bool& campaign_active;
    std::vector<assets::res::CampaignStage>& campaign_stages;
    int& campaign_stage_index;
    std::string& campaign_banner;
    // The roster seed_campaign_ai_slots adds to (it does NOT reset it — see
    // load_campaign_stage below).
    std::array<int, sim::kMaxPlayers>& setup_type;
    std::array<int, sim::kMaxPlayers>& setup_sub;
    std::array<int, sim::kMaxPlayers>& setup_team;
    std::uint32_t& setup_lcg;  // the shared presentation LCG, never sim::State::rng
    assets::sch::Scheme& scheme;
    const std::filesystem::path& game_dir;
};

// Loads campaign stage `index`'s scheme and seeds the roster from its AI count.
// Returns false, leaving state untouched, if the scheme can't be resolved, so the
// caller can bail out of campaign mode instead of starting on a stale board. A
// free function so BOTH callers — the picker and the Results auto-advance — reach
// one copy.
inline bool load_campaign_stage(int index, CampaignState state) {
    if (index < 0 || index >= static_cast<int>(state.campaign_stages.size())) return false;
    const assets::res::CampaignStage& stage = state.campaign_stages[static_cast<std::size_t>(index)];

    if (!reload_scheme(state.scheme, state.game_dir, stage.scheme)) return false;

    // AI roster auto-fill — CORRECTED 2026-07-09 (docs/re/campaign.md). The real
    // per-stage seeder is sub_40151B, NOT sub_42288C, and it picks a RANDOM
    // currently-OFF slot per AI rather than filling sequentially. Only the AI
    // COUNT seeds player slots; folding rovers/ghosts in was a mislabelling. The
    // randomness runs on the presentation LCG, never State::rng.
    //
    // THE ROSTER IS NOT CLEARED FIRST — CORRECTED 2026-07-30, and load-bearing.
    // sub_40151B has no reset in it and sub_422928 refuses any slot already
    // non-zero, so the human survives every stage transition and each stage's AI
    // is ADDED. The port used to reset all ten slots to OFF here, which is
    // invisible while the picker arms stage 0 but DELETES THE PLAYER on every
    // stage advance after it, leaving an AI-only roster the round pacing reads as
    // "no human survivor" and replays forever. It stayed hidden only because the
    // advance used to require winning a whole best-of-N match first.
    std::array<bool, sim::kMaxPlayers> occupied{};
    for (int slot = 0; slot < sim::kMaxPlayers; ++slot)
        occupied[static_cast<std::size_t>(slot)] = state.setup_type[slot] != 0;
    for (int slot : seed_campaign_ai_slots(state.setup_lcg, stage.ai_count, occupied))
        state.setup_type[slot] = 1;
    // ai_difficulty (field 8) is a CONFIRMED negative: a grep of the whole binary
    // finds dword_45E010's field-8 slot written once by the loader and read
    // nowhere, matching the .CAM format's own "(unused at present)" comment.
    // Rovers/ghosts (fields 3-6) are read straight off the stage record by
    // start_match, so they are NOT stashed here.
    //
    // Stage banner (sub_40133F, CONFIRMED): getstring(1235)="(%s)" with the
    // stage's own name. levelno (field 1) has no consumer beyond it —
    // selected_level stays RANDOM, since a campaign stage supplies its own SCHEME.
    state.campaign_banner = "(" + stage.name + ")";
    return true;
}

}  // namespace bomber::game
