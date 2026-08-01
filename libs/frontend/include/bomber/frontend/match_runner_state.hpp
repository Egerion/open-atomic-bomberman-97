#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

#include "bomber/assets/campaign.hpp"               // assets::res::CampaignStage
#include "bomber/assets/sch.hpp"                    // assets::sch::Scheme
#include "bomber/frontend/options_model.hpp"        // OptionsSnapshot
#include "bomber/game_util/campaign_round_end.hpp"  // CampaignVerdict (dword_464894)
#include "bomber/render/renderer.hpp"               // Renderer
#include "bomber/sim/constants.hpp"                 // sim::kMaxPlayers
#include "bomber/sim/simulation.hpp"                // sim::Simulation
#include "bomber/sim/tuning.hpp"                    // sim::Tuning

// The netplay session is only ever reached through a POINTER here —
// forward-declared, never included, so the heavy net/sim-coupled header stays out
// of every screen TU that pulls this seam in.
namespace bomber::net {
class RollbackSession;
}  // namespace bomber::net

namespace bomber::game {

// HOW THE LOCAL PLAYER WALKED OUT of an online round. Both values are UNILATERAL
// and IMMEDIATE by definition: they tear the transport down locally and send
// nothing, because a bail-out that needs the network is not a bail-out. Distinct
// from STOPPING the round (request_end_round), which is host-only and DOES need
// both machines to agree, because it changes what both of them simulate.
enum class NetLeave : std::uint8_t {
    None = 0,
    // Ctrl+Q — CONFIRMED as the original's only mid-round abort
    // (docs/re/in-match-shell.md "Esc negative finding").
    Forfeit,
    // Double-Esc — the PORT's bail-out from a match that stopped responding. Same
    // teardown, different reason in netdiag.log, because the two say very
    // different things about whether the netcode is working.
    Stalled,
};

}  // namespace bomber::game

// The non-service state the match runtime reads and writes, bundled by reference
// so MatchRunner needs no GameApp&.
//
// GOLDEN-SENSITIVE: these are the exact members whose read/write order feeds the
// sim, so even the read-only ones are const REFERENCES and are read verbatim. The
// video levers MUST be references because the global SDL_EventFilter flips them
// MID-MATCH; a by-value snapshot would freeze them, a behaviour change.

namespace bomber::game {

struct MatchRunnerState {
    // --- Mutated by the run ---
    sim::Simulation& sim;
    Renderer& renderer;
    std::uint32_t& next_seed;
    std::array<int, sim::kMaxPlayers>& kill_count;
    // What sub_4016DA left behind. MatchRunner LATCHES it the instant the
    // round-end condition first holds rather than letting the shell re-derive it
    // from the frozen state: this runner lingers for the death animations, and a
    // human dying to a leftover flame during that linger would silently turn a
    // stage ADVANCE into a stage REPLAY.
    CampaignPacing& campaign_pacing;

    // --- Live video levers, flipped mid-match by the global event filter ---
    bool& uncap_fps;       // F8 pacing
    bool& native_cadence;  // F9 cadence
    bool& show_fps;        // F7 overlay
    // F3. Session-only: unlike show_fps it is never read from or written to
    // options.ini, so no saved value can reach a capture.
    bool& show_netstats;

    // --- Read-only match inputs ---
    const assets::sch::Scheme& scheme;
    const sim::Tuning& base_tuning;
    const OptionsSnapshot& options;
    const std::optional<int>& conveyor_speed_index;
    const std::array<int, sim::kMaxPlayers>& setup_type;
    const std::array<int, sim::kMaxPlayers>& setup_sub;
    const std::array<int, sim::kMaxPlayers>& setup_team;
    const std::array<int, sim::kMaxPlayers>& win_count;
    const bool& team_play;
    const bool& campaign_active;
    const bool& attract;
    const int& gold_player;
    const int& gold_prize;
    const int& selected_level;
    const std::vector<assets::res::CampaignStage>& campaign_stages;
    const int& campaign_stage_index;
    const std::filesystem::path& game_dir;  // EXTRA<n>.RES
    const bool& demo;                       // disarms the round-start input freeze

    // --- Netplay hook ---
    // Non-null forces the deterministic fixed-tick path (never F9's frame()
    // cadence) through the session. Every non-netplay caller leaves it null, so
    // the sim tick/seed path — and the golden hashes — are untouched.
    net::RollbackSession* net_session = nullptr;
    std::uint16_t net_local_seats = 0;  // this peer's human-seat bitmask (bit s == seat s)
    bool net_is_host = false;  // stopping the match is host-only; leaving is not

    // An explicit out-parameter because the shell used to INFER the act from the
    // frozen sim state, and that inference is wrong at both edges: a forfeit
    // pressed as the last opponent died reads as a natural round end, and a
    // bail-out pressed after an abandon was agreed reads as a draw and rotates
    // into another round.
    NetLeave* net_leave = nullptr;
};

}  // namespace bomber::game
