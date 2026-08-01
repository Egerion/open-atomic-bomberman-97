#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

#include "bomber/assets/sch.hpp"  // assets::sch::Scheme
#include "bomber/frontend/campaign_state.hpp"
#include "bomber/frontend/map_select_state.hpp"
#include "bomber/frontend/match_backdrop.hpp"
#include "bomber/frontend/match_runner_state.hpp"
#include "bomber/frontend/options_screen.hpp"  // OptionsSnapshot
#include "bomber/frontend/results_state.hpp"
#include "bomber/frontend/setup_state.hpp"
#include "bomber/render/renderer.hpp"  // Renderer
#include "bomber/sim/constants.hpp"    // sim::kMaxPlayers
#include "bomber/sim/simulation.hpp"   // sim::Simulation
#include "bomber/sim/tuning.hpp"       // sim::Tuning
#include "bomber/ui/screen_context.hpp"

// Seam (ADR-0009 §"shared front-end state"): what the NETPLAY ORCHESTRATION —
// the CLI entry, the connect/lobby leaves, the online setup stage and the
// match/round/rematch loop — reads and writes on the app shell, bundled by
// reference so NetplayRunner can be its own class without a GameApp&. GameApp
// owns the members and hands a fresh NetplayState + NetplaySeams to the runner's
// constructor alongside the ScreenContext services bundle.
//
// NOT GOLDEN-SENSITIVE in itself — the netplay path is unreachable from every
// test/golden/demo entry — but `sim` and the roster arrays below ARE the same
// members MatchRunner ticks, so the read/write ORDER here still decides what
// both peers simulate. Nothing in this seam may be snapshotted by value.

namespace bomber::game {

// The seam BUILDERS, not the seams themselves. Every one is invoked FRESH at
// each use, because the round loop REPLACES the simulation between rounds (`sim
// = Simulation(cfg)`) and rebuilds the presentation roster under it: a bundle
// captured once would be describing a match that no longer exists.
struct NetplaySeams {
    std::function<ScreenContext()> sctx;
    std::function<MatchRunnerState()> match_runner_state;
    std::function<ScoreboardState()> scoreboard_state;
    std::function<SetupState()> setup_state;
    std::function<CampaignState()> campaign_state;
    std::function<MatchBackdrop()> match_backdrop;
    std::function<MapSelectState()> map_select_state;
};

struct NetplayState {
    // --- Mutated by a netplay match ---
    sim::Simulation& sim;  // GameApp::sim_ (re-seeded from the agreed config each round)
    Renderer& renderer;    // GameApp::*renderer_ (reset_match per round)
    std::array<int, sim::kMaxPlayers>& win_count;   // GameApp::win_count_ (fresh MATCH tally)
    std::array<int, sim::kMaxPlayers>& kill_count;  // GameApp::kill_count_ (fresh MATCH tally)
    // The PRESENTATION roster, rewritten from the agreed config at the head of
    // every round (local seat -> KEYBOARD, AI -> COMPUTER, everyone else -> the
    // original's type 4 OTHER). Also what the host's own setup screens edit.
    std::array<int, sim::kMaxPlayers>& setup_type;  // GameApp::setup_type_
    std::array<int, sim::kMaxPlayers>& setup_sub;   // GameApp::setup_sub_
    std::array<int, sim::kMaxPlayers>& setup_team;  // GameApp::setup_team_
    // The team GATE (dword_464964). Driven from the AGREED config for the
    // duration of a match and from the host's preview for the duration of the
    // guest's setup stage — and restored on the way out of both by
    // TeamPlayScope (netplay_runner.hpp), so a following LOCAL game keeps the
    // player's own Options setting.
    bool& team_play;           // GameApp::team_play_
    std::uint32_t& setup_lcg;  // GameApp::setup_lcg_ (pick_glue, the [WAIT] backdrop)

    // --- Read-only ---
    const int& win_target;                  // GameApp::win_target_ (match_clinch)
    const assets::sch::Scheme& scheme;      // GameApp::scheme_ (canonical_config)
    const sim::Tuning& base_tuning;         // GameApp::base_tuning_ (pick_stage)
    const OptionsSnapshot& options;         // GameApp::options_ (music/drop policy/node name)
    const std::filesystem::path& game_dir;  // GameApp::opts_.game_dir (EXTRA<n>.RES)

    // --- The CLI's own arguments (--host/--join, ADR-0010 §3.3 step 5) ---
    const int& net_role;                  // GameApp::opts_.net_role (1 = host, 2 = guest)
    const std::uint16_t& net_local_port;  // GameApp::opts_.net_local_port
    const std::string& net_peer_host;     // GameApp::opts_.net_peer_host
    const std::uint16_t& net_peer_port;   // GameApp::opts_.net_peer_port
    const std::uint32_t& net_seed;        // GameApp::opts_.net_seed (the SAME on both peers)

    // --- Matchmaker endpoint overrides (--matchmaker / --matchmaker-stun) ---
    // Only the FLAGS live here; the env-var and compile-time tiers of the
    // documented precedence order are resolved inside the runner.
    const std::string& matchmaker_url;          // GameApp::opts_.matchmaker_url
    const std::string& matchmaker_stun_host;    // GameApp::opts_.matchmaker_stun_host
    const std::uint16_t& matchmaker_stun_port;  // GameApp::opts_.matchmaker_stun_port (0 = unset)
};

// Borrow the team GATE for the duration of a scope and give it back.
//
// Both the online setup stage and the match itself drive `team_play` from
// SOMEBODY ELSE'S state — the host's live preview, then the agreed MatchConfig —
// and both have to hand the player's own Options setting back afterwards,
// because is_team_mode()/draw_player_row read the gate and a following LOCAL
// game would otherwise be played in a team mode nobody asked for.
//
// RAII rather than a saved local because the two scopes it guards have SEVEN
// exits between them. Restoring by hand was correct when it was audited, but
// only by inspection: this is the shape where the next exit added forgets.
class TeamPlayScope {
public:
    explicit TeamPlayScope(bool& team_play) : team_play_(&team_play), saved_(team_play) {}
    ~TeamPlayScope() { *team_play_ = saved_; }

    TeamPlayScope(const TeamPlayScope&) = delete;
    TeamPlayScope& operator=(const TeamPlayScope&) = delete;
    TeamPlayScope(TeamPlayScope&&) = delete;
    TeamPlayScope& operator=(TeamPlayScope&&) = delete;

private:
    bool* team_play_;
    bool saved_;
};

}  // namespace bomber::game
