#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

#include "bomber/assets/campaign.hpp"       // assets::res::CampaignStage
#include "bomber/assets/sch.hpp"            // assets::sch::Scheme
#include "bomber/game/options_screen.hpp"  // OptionsSnapshot
#include "bomber/game/renderer.hpp"        // Renderer
#include "bomber/sim/constants.hpp"        // sim::kMaxPlayers
#include "bomber/sim/simulation.hpp"       // sim::Simulation
#include "bomber/sim/tuning.hpp"           // sim::Tuning

// The netplay lockstep session (bomber::net, libs/net) is only ever reached
// through a POINTER in this seam — forward-declared, never included, so the
// heavy net/sim-coupled header stays out of every screen TU that pulls the
// seam in. match_runner.cpp (the sole caller of ->advance()) includes the real
// header. Default-null below, so every non-netplay caller is byte-identical.
namespace bomber::net {
class RollbackSession;
}  // namespace bomber::net

// Seam 2 (ADR-0009 §"shared front-end state" / §10 MatchRunner): the non-service
// state the match runtime (run_match + its start_match / collect_inputs /
// draw_player_row / draw_fps_overlay members) reads/writes, bundled by reference
// so MatchRunner can be its own class without threading a GameApp& — GameApp owns
// the members and hands a fresh MatchRunnerState to the runner ctor alongside the
// ScreenContext services bundle. Cheap value type (references only), copied by
// value into the runner; the referenced members are GameApp members that outlive
// every run. This is a LARGE seam because run_match is the god-object's biggest
// method — it drives the deterministic sim, so the whole MatchConfig-build /
// tick-loop / seed path lives here.
//
// GOLDEN-SENSITIVE (ADR-0009 §10): these are the exact members whose read/write
// order feeds the sim. The mutable four (sim/renderer/next_seed/kill_count) are
// non-const; the three F7/F8/F9 video levers are non-const references too because
// the global SDL_EventFilter (GameApp::handle_global_event) flips them MID-MATCH,
// so run_match must read the live member each frame (a by-value snapshot would
// freeze them — a behaviour change). Everything else run_match/start_match only
// READ, so they are const references (they stay fixed for the duration of a run,
// but are kept as references — not value snapshots — to read the member verbatim).

namespace bomber::game {

struct MatchRunnerState {
    // --- Mutated by the run (non-const) ---
    sim::Simulation& sim;                                // GameApp::sim_ (ticked: tick/frame/reset)
    Renderer& renderer;                                  // GameApp::*renderer_ (draw/on_events/...)
    std::uint32_t& next_seed;                            // GameApp::next_seed_ (start_match(next_seed++))
    std::array<int, sim::kMaxPlayers>& kill_count;       // GameApp::kill_count_ (tally_kills)

    // --- F7/F8/F9 live levers: read here, flipped mid-match by the global event
    //     filter, so they MUST be references (not value snapshots) ---
    bool& uncap_fps;        // GameApp::uncap_fps_ (F8 pacing lever)
    bool& native_cadence;   // GameApp::native_cadence_ (F9 cadence lever)
    bool& show_fps;         // GameApp::show_fps_ (F7 overlay lever)
    // F3: the in-match NETPLAY diagnostic panel (screens/net_overlay.hpp). A
    // reference for the same reason the three above are — the global event
    // filter flips it mid-match. Session-only: unlike show_fps it is never read
    // from or written to options.ini, so no saved value can reach a capture.
    bool& show_netstats;

    // --- Read-only match inputs (const references) ---
    const assets::sch::Scheme& scheme;                   // GameApp::scheme_ (build_match_config)
    const sim::Tuning& base_tuning;                       // GameApp::base_tuning_ (pick_stage)
    const OptionsSnapshot& options;                       // GameApp::options_ (per-match Tuning overrides)
    const std::optional<int>& conveyor_speed_index;       // GameApp::conveyor_speed_index_
    const std::array<int, sim::kMaxPlayers>& setup_type;  // GameApp::setup_type_ (roster / collect_inputs)
    const std::array<int, sim::kMaxPlayers>& setup_sub;   // GameApp::setup_sub_ (collect_inputs)
    const std::array<int, sim::kMaxPlayers>& setup_team;  // GameApp::setup_team_ (roster / is_team_mode)
    const std::array<int, sim::kMaxPlayers>& win_count;   // GameApp::win_count_ (player-row HUD)
    const bool& team_play;                                // GameApp::team_play_ (team gate / is_team_mode)
    const bool& campaign_active;                          // GameApp::campaign_active_ (rovers / round pacing)
    const bool& attract;                                  // GameApp::attract_ (any-input abort)
    const int& gold_player;                               // GameApp::gold_player_ (born-with award / twinkle)
    const int& gold_prize;                                // GameApp::gold_prize_ (born-with award)
    const int& selected_level;                            // GameApp::selected_level_ (stage pick)
    const std::vector<assets::res::CampaignStage>& campaign_stages;  // GameApp::campaign_stages_
    const int& campaign_stage_index;                      // GameApp::campaign_stage_index_
    const std::filesystem::path& game_dir;                // GameApp::opts_.game_dir (EXTRA<n>.RES)
    const bool& demo;                                     // GameApp::opts_.demo (round-start freeze disarm)

    // --- Netplay hook (increment 5b, ADR-0010 §3.3 step 5) ---
    // When non-null, MatchRunner::run() drives the (borrowed) sim through this
    // lockstep session instead of ticking it directly, forcing the deterministic
    // fixed-tick path (never the F9 frame() cadence). GameApp::run_netplay owns
    // the session + the canonical config; every other caller leaves this null,
    // so the sim tick/seed path — and the golden hashes — are untouched.
    net::RollbackSession* net_session = nullptr;
    std::uint16_t net_local_seats = 0;  // this peer's human-seat bitmask (bit s == seat s)
};

}  // namespace bomber::game
