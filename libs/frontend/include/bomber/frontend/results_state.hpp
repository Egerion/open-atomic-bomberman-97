#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "bomber/frontend/options_screen.hpp"      // OptionsSnapshot
#include "bomber/game_util/net_round_gate.hpp"     // NetRoundGate
#include "bomber/sim/constants.hpp"                // sim::kMaxPlayers
#include "bomber/sim/state.hpp"                    // sim::State

// Seam 2 (ADR-0009 §"shared front-end state"): the non-service state the two
// RESULTS-tier screens (present_scoreboard + present_goldman_wheel, sub_42A3F6)
// read/write, bundled so each can be its own class without threading a GameApp&
// — GameApp owns the members and hands a fresh seam to the runner ctor alongside
// the ScreenContext services bundle. Cheap value types (references / small
// scalars), copied by value into the runner; the referenced members are GameApp
// members that outlive every screen. ScreenContext stays front-end-service-only,
// so the results-specific mutable state lives here instead.
//
// Two DISJOINT seams (the scoreboard only reads the tally/roster; the wheel only
// writes the pending-gold trio), matching the pattern's one-seam-per-screen shape
// rather than one fat MatchScores struct half-used by each.

namespace bomber::game {

// present_scoreboard's non-service reads (it writes none of these — a pure
// display of one round's cumulative tally):
//  - state — the just-ended round's sim::State (sim_.state()): the per-slot
//    `present` gate for every row, and the argument every promoted predicate
//    (round_winner/is_team_mode/match_clinch) needs. The scoreboard NEVER ticks
//    the sim, so a reference captured at construction stays valid for its loop
//    (same frozen-frame invariant MatchBackdrop relies on). Kept in the seam
//    rather than as a MatchBackdrop because the scoreboard draws RESULTS.PCX,
//    NOT the live match frame (renderer_->draw_frame), so it needs the State but
//    not the Renderer (ADR-0009 §8's asset-screen-only case).
//  - win_count/kill_count — the per-player match tally rows (§1 sub_421B0F).
//  - win_target — the win goal spliced into the pre-clinch "(Match winner must
//    score %u ...)" line, and passed to match_clinch.
//  - setup_team — the per-slot team byte the team rows aggregate by, and
//    is_team_mode/match_clinch's gate.
//  - team_play — the game-type TEAM gate (dword_464964), is_team_mode's gate.
//  - setup_type — read ONLY by the per-frame auto_advance_results() re-check
//    (the all-AI/human roster test).
//  - options — READ-ONLY, for options.win_by_kills (the pre-clinch/clinch line
//    id pick 120/121 & 35/36) and match_clinch's win_by_kills branch.
//  - demo/demo_ticks/demo_shots — the --demo / --demo-shots flags
//    auto_advance_results() consults every frame; taken from GameApp::opts_.
// Read-only aggregates are const&; the read-only scalars are snapshotted by
// value (the screen never mutates them and they don't change across its loop).
//  - net_gate — NULL for local play (the screen behaves exactly as it always
//    has). Non-null between the rounds of an ONLINE match: the HOST's accept
//    commits the next round and the GUEST's screen ends when that commitment
//    arrives, mirroring the original's host-driven RESULTS wait loop (the
//    network-only 903 accept code + the guest's SFX-40 buzz,
//    docs/re/in-match-shell.md). Borrowed; owned by the caller's match loop.
struct ScoreboardState {
    NetRoundGate* net_gate = nullptr;                       // null = local play
    const sim::State& state;                                // sim_.state()
    const std::array<int, sim::kMaxPlayers>& win_count;     // GameApp::win_count_
    const std::array<int, sim::kMaxPlayers>& kill_count;    // GameApp::kill_count_
    int win_target;                                         // GameApp::win_target_
    const std::array<int, sim::kMaxPlayers>& setup_team;    // GameApp::setup_team_
    bool team_play;                                         // GameApp::team_play_
    const std::array<int, sim::kMaxPlayers>& setup_type;    // GameApp::setup_type_ (auto-advance)
    const OptionsSnapshot& options;                         // GameApp::options_ (win_by_kills)
    bool demo;                                              // GameApp::opts_.demo
    int demo_ticks;                                         // GameApp::opts_.demo_ticks
    const std::vector<std::pair<std::string, int>>& demo_shots;  // GameApp::opts_.demo_shots
};

// present_goldman_wheel's non-service WRITES (all three, hence non-const refs):
//  - goldman_lcg — the wheel's dedicated presentation LCG (never sim::State::rng):
//    advanced one step per spin before wheel.enter(); the 5 setup draws it seeds
//    are observable (goldman-roulette.md §3), so the draw order/count is pinned.
//  - gold_player — cleared to -1 when Esc aborts the wheel (doc §2 "Cleared to -1
//    by: Esc on the wheel"), forfeiting the pending winner.
//  - gold_prize — set to wheel.prize() on a completed spin (doc §4); start_match
//    re-applies it every round of the following match via born_with_extra.
struct GoldmanState {
    std::uint32_t& goldman_lcg;  // GameApp::goldman_lcg_
    int& gold_player;            // GameApp::gold_player_
    int& gold_prize;             // GameApp::gold_prize_
};

}  // namespace bomber::game
