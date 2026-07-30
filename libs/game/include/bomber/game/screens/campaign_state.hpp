#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "bomber/assets/campaign.hpp"
#include "bomber/assets/sch.hpp"
#include "bomber/game/frontend_util.hpp"  // reload_scheme
#include "bomber/game/results.hpp"        // seed_campaign_ai_slots
#include "bomber/sim/constants.hpp"       // sim::kMaxPlayers

// Seam 2 (ADR-0009 §"shared front-end state"): the non-service state the hidden
// campaign flow (present_campaign_picker + load_campaign_stage) reads/writes,
// bundled so CampaignPickerScreen can be its own class without threading a
// GameApp& — GameApp owns the members and hands a fresh CampaignState to the
// runner ctor (and to the free load_campaign_stage below) alongside the
// ScreenContext services bundle. A cheap value type (references only), copied by
// value; the referenced members are GameApp members that outlive every screen.
// ScreenContext stays front-end-service-only, so the campaign-specific mutable
// state lives here instead.
//
// The reference set is EXACTLY what present_campaign_picker + load_campaign_stage
// touch: the campaign-mode flags/stage list/index/banner, the roster
// setup_type/setup_sub/setup_team that seed_campaign_ai_slots resets+seeds, the
// shared presentation LCG (setup_lcg — never sim::State::rng), and the
// scheme/game_dir the stage's scheme resolve loads through.

namespace bomber::game {

struct CampaignState {
    bool& campaign_active;                                     // GameApp::campaign_active_
    std::vector<assets::res::CampaignStage>& campaign_stages;  // GameApp::campaign_stages_
    int& campaign_stage_index;                                 // GameApp::campaign_stage_index_
    std::string& campaign_banner;                              // GameApp::campaign_banner_
    std::array<int, sim::kMaxPlayers>& setup_type;             // GameApp::setup_type_
    std::array<int, sim::kMaxPlayers>& setup_sub;              // GameApp::setup_sub_
    std::array<int, sim::kMaxPlayers>& setup_team;             // GameApp::setup_team_
    std::uint32_t& setup_lcg;                                  // GameApp::setup_lcg_
    assets::sch::Scheme& scheme;                               // GameApp::scheme_
    const std::filesystem::path& game_dir;                     // GameApp::opts_.game_dir
};

// Loads campaign stage `index`'s scheme by name (resolves `<scheme>.SCH`
// case-insensitively under DATA/SCHEMES, mirroring the case-insensitive glob
// every other picker already uses) into scheme_, and seeds setup_type_ from the
// stage's AI count (docs/re/campaign.md "Rover/ghost/AI roster — CORRECTED"):
// the real per-stage seeder is sub_40151B (gated dword_46489C), NOT sub_42288C
// (that only clears a per-slot UI latch) — it flips exactly `ai_count`
// RANDOMLY-chosen OFF slots to COMPUTER (sub_422928: `rand()%10` +
// retry-on-occupied) and separately spawns `rovers`/`ghosts` as autonomous
// map-hazard actors (sub_401AAE/sub_401B05) in a particle table libs/sim has no
// equivalent of — NOT folded into COMPUTER slots (a prior mislabelling,
// corrected). Also sets campaign_banner_ to the stage's display text
// (docs/re/campaign.md "Stage banner"). Returns false (and leaves state
// untouched) if the scheme can't be resolved/loaded, so the caller can bail out
// of campaign mode cleanly instead of starting a match with a stale board.
//
// NOT a screen (no draw loop): a free function in this shared header so BOTH
// callers reach the one copy — present_campaign_picker (CampaignPickerScreen,
// which has a CampaignState) and run_app's Results auto-advance handler (GameApp,
// which builds one via campaign_state()).
inline bool load_campaign_stage(int index, CampaignState state) {
    if (index < 0 || index >= static_cast<int>(state.campaign_stages.size())) return false;
    const assets::res::CampaignStage& stage = state.campaign_stages[static_cast<std::size_t>(index)];

    // Resolve the stage's "scheme to use" name to a DATA/SCHEMES/<name>.SCH
    // path, case-insensitively (DOS filenames are case-insensitive), and
    // load it — the same reload_scheme_from_name the Options scheme picker
    // and init()'s schemefilename= resolution use.
    if (!reload_scheme(state.scheme, state.game_dir, stage.scheme)) return false;

    // AI roster auto-fill — CORRECTED 2026-07-09 (docs/re/campaign.md
    // "Rover/ghost/AI roster — CORRECTED"). sub_40151B (the real per-stage
    // starter gated dword_46489C, not sub_42288C as previously mislabelled)
    // is the actual roster/actor seeder: it calls sub_422928 once per AI, as
    // many times as the stage's ai_count, and sub_422928 picks a RANDOM
    // currently-OFF slot (rand() modulo 10, retried up to 100 times) and flips
    // it to COMPUTER — not
    // a sequential fill from slot 0. Only the AI COUNT (field 7) seeds
    // player slots at all; rovers/ghosts are NOT player slots (see below),
    // so folding them into COMPUTER slots (the prior port behaviour) was a
    // mislabelling, now removed. Exactly `ai_count` distinct FREE slots are
    // flipped to COMPUTER at random, matching sub_422928's rand()-modulo-10 +
    // retry-on-occupied shape but using the presentation LCG (state.setup_lcg),
    // never State::rng — this only steers which slot ids get the pre-supplied
    // roster, no sim RNG draw.
    //
    // The roster is NOT cleared first — CORRECTED 2026-07-30, and this one was
    // load-bearing rather than cosmetic. sub_40151B @0x40151B has no reset in
    // it: the per-slot loop it runs before seeding calls sub_42288C, which only
    // clears a UI latch, and sub_422928 @0x422977 refuses any slot whose type
    // byte is already non-zero. So the human the player set up on the PLAYER
    // INPUT screen survives every stage transition, and each stage's AI is
    // ADDED to the standing roster. The port used to reset all ten slots to OFF
    // here, which is invisible while the picker arms stage 0 (the setup screen
    // still follows) but DELETES THE PLAYER on every stage advance after it —
    // leaving an AI-only roster, which the round pacing then reads as "no human
    // survivor" and replays forever. It stayed hidden only because the advance
    // used to require winning a whole best-of-N match first.
    std::array<bool, sim::kMaxPlayers> occupied{};
    for (int slot = 0; slot < sim::kMaxPlayers; ++slot)
        occupied[static_cast<std::size_t>(slot)] = state.setup_type[slot] != 0;
    for (int slot : seed_campaign_ai_slots(state.setup_lcg, stage.ai_count, occupied))
        state.setup_type[slot] = 1;
    // Rovers/ghosts (fields 3-6, docs/re/campaign.md "Rover/ghost/AI
    // roster") are NOT player slots — they are autonomous roaming map-hazard
    // actors, now a real libs/sim actor kind (RoverSystem: spawn, wander AI,
    // flame death + kill-score, landing-tile player kill; hashed
    // State::rovers). start_match reads stage.rovers/rover_speed/ghosts/
    // ghost_speed straight off state.campaign_stages[campaign_stage_index_] into
    // MatchConfig::campaign_rovers/etc (this function only prepares the
    // scheme/roster/banner, not the sim config, so the counts are read
    // there, not stashed here). ai_difficulty (field 8) is CONFIRMED dead
    // code (grep of the whole binary: dword_45E010's field-8 slot, reached in
    // the loader as element 27 of the per-stage record, is written once there
    // and read NOWHERE else),
    // matching the .CAM format's own "(unused at present)" comment exactly
    // — not a guess, a confirmed negative. Round pacing clauses 1/3/4/5
    // (docs/re/campaign.md "Round pacing") are now wired too: clause 1 by
    // RoverSystem::tick itself (simulation.cpp), clause 3 by run_match's
    // hazard_clear_timer edge-check, clauses 4-5 by the Results DRAW branch
    // (run_app) — see each site's own comment for the exact mechanism.

    // Stage display banner (docs/re/campaign.md "Stage banner — CONFIRMED"):
    // sub_40133F formats getstring(1235)="(%s)" with the stage's OWN name
    // (campaign record field 0, the first bytes of the 112-byte record) and
    // shows it alongside getstring(1230)="Prepare to begin Campaign!" as a
    // blocking two-line dialog at every stage transition (same sub_414340
    // dialog family present_campaign_picker's OWN confirm dialog,
    // present_campaign_confirm, now ports with the real chrome). Stored here
    // for present_setup/run_app to draw;
    // levelno (field 1) has no further consumer beyond this banner and the
    // scheme/roster application above — selected_level_ stays -1
    // (RANDOM/pick_stage) since a campaign stage supplies its own SCHEME,
    // not one of the 11 built-in level tilesets.
    state.campaign_banner = "(" + stage.name + ")";
    return true;
}

}  // namespace bomber::game
