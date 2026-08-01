#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "bomber/frontend/options_model.hpp"       // OptionsSnapshot
#include "bomber/game_util/net_round_gate.hpp"     // NetRoundGate
#include "bomber/sim/constants.hpp"                // sim::kMaxPlayers
#include "bomber/sim/state.hpp"                    // sim::State

// The non-service state the two RESULTS-tier screens (sub_42A3F6) read and write,
// bundled by reference so neither needs a GameApp&.
//
// Two DISJOINT seams — the scoreboard only reads the tally/roster, the wheel only
// writes the pending-gold trio — rather than one fat struct half-used by each.

namespace bomber::game {

// The scoreboard WRITES none of these: it is a pure display of one round's
// cumulative tally.
struct ScoreboardState {
    // Null for local play. Non-null between the rounds of an ONLINE match: the
    // HOST's accept commits the next round and the GUEST's screen ends when that
    // commitment arrives. Borrowed; owned by the caller's match loop.
    NetRoundGate* net_gate = nullptr;
    // The just-ended round's state. The scoreboard NEVER ticks the sim, so a
    // reference captured at construction stays valid for its whole loop.
    const sim::State& state;
    // The two per-player match tally rows (sub_421B0F).
    const std::array<int, sim::kMaxPlayers>& win_count;
    const std::array<int, sim::kMaxPlayers>& kill_count;
    int win_target;                                       // spliced into the pre-clinch line
    const std::array<int, sim::kMaxPlayers>& setup_team;  // what the team rows aggregate by
    bool team_play;                                       // the TEAM gate (dword_464964)
    const std::array<int, sim::kMaxPlayers>& setup_type;  // the auto-advance roster test
    const OptionsSnapshot& options;                       // read-only: win_by_kills
    bool demo;                                            // the --demo / --demo-shots flags
    int demo_ticks;
    const std::vector<std::pair<std::string, int>>& demo_shots;
};

// The wheel WRITES all three, hence non-const.
struct GoldmanState {
    // The wheel's dedicated presentation LCG (never sim::State::rng), advanced one
    // step per spin. The 5 setup draws it seeds are observable
    // (goldman-roulette.md §3), so the draw order and count are pinned.
    std::uint32_t& goldman_lcg;
    int& gold_player;  // cleared to -1 when Esc aborts the wheel (doc §2)
    int& gold_prize;   // the completed spin's prize (doc §4)
};

}  // namespace bomber::game
