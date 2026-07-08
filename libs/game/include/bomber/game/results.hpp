#pragma once

#include <array>
#include <vector>

#include "bomber/sim/constants.hpp"
#include "bomber/sim/event.hpp"

// Pure, SDL-free helpers for the RESULTS scoreboard (docs/re/
// results-and-options.md §1, sub_42A3F6's middle tier). Kept out of
// game_app.cpp so the per-round kill tally and the win_by_kills clinch
// predicate are unit-testable without linking SDL3 (mirrors input.hpp's
// cycle_slot_input_type / app_flow.hpp's next() — see tests/test_frontend.cpp,
// bomber_frontend_tests links no SDL3).

namespace bomber::game {

// Tallies one sim tick's PlayerDied events into a per-round kill counter
// (§1's sub_421B0F field). PlayerDied.data carries the killer's index per
// event.hpp's convention: -1 = no attributable killer (enclosure/warphole
// crush, not counted), data == the victim's own index = a self-kill.
//
// Self-kill semantics are OUR OWN choice: §1 documents the counter's
// existence and its position in the RESULTS row but does not pin whether a
// player who blows themselves up increments their own kill count. We EXCLUDE
// self-kills — a player should not be rewarded for dying — matching the
// intuitive reading of "kills" as "opponents you eliminated". Revisit if the
// binary is found to credit them.
inline void tally_kills(const std::vector<sim::Event>& events,
                        std::array<int, sim::kMaxPlayers>& kill_count) {
    for (const sim::Event& e : events) {
        if (e.type != sim::Event::Type::PlayerDied) continue;
        int killer = e.data;
        if (killer < 0 || killer >= sim::kMaxPlayers) continue;  // no killer
        if (killer == e.player) continue;                        // self-kill: excluded
        ++kill_count[static_cast<std::size_t>(killer)];
    }
}

// The §1 v73 match-clinch check's win_by_kills branch: "in team mode with
// win_by_kills set, the clinch compares the highest round-kill total
// (sub_421B0F) against dword_464A7C (the wins-needed target), breaking ties
// by requiring a single unique leader (v78 == 1)". Returns the clinching
// player's index, or -1 if no single player has both reached the target AND
// uniquely holds the highest kill total.
//
// `present` gates which slots are considered (an absent/OFF slot's kill_count
// entry, always 0, must not accidentally tie for the lead against a real
// player who also has 0 kills).
inline int win_by_kills_clinch(const std::array<int, sim::kMaxPlayers>& kill_count,
                               const std::array<bool, sim::kMaxPlayers>& present,
                               int target) {
    int best = -1;
    int best_count = -1;
    int leaders = 0;
    for (int i = 0; i < sim::kMaxPlayers; ++i) {
        if (!present[i]) continue;
        if (kill_count[i] > best_count) {
            best_count = kill_count[i];
            best = i;
            leaders = 1;
        } else if (kill_count[i] == best_count) {
            ++leaders;
        }
    }
    if (best < 0 || best_count < target) return -1;  // nobody reached the target
    if (leaders != 1) return -1;                      // v78 == 1: unique leader required
    return best;
}

// The Goldman Roulette gold-player assignment (docs/re/goldman-roulette.md
// §2, pseudo.c 30004-30022): sub_42A3F6's RESULTS tier writes dword_46492C
// from v73 (the MATCH-CLINCH winner, i.e. match_clinch()'s return — NOT the
// per-round winner) whenever goldman is on, else -1. In team mode the stored
// value is the clinching player's raw team id (our port's 0/1 space; see the
// doc's note on the original's internal 0/2 encoding), matching both the
// wheel-award consumer in GameApp::build_match_config and
// present_scoreboard's own clinched_player -> team_of[] lookup.
//
// Call ONLY when the RESULTS tier itself runs, i.e. the round had a survivor
// (round_winner() >= 0) — a DRAW never reaches sub_42A3F6's RESULTS tier in
// the original, so a pending gold player must be left untouched on a draw
// round (the caller simply skips calling this, not passing a sentinel).
inline int assign_gold_player(bool goldman_on, bool team_mode, int clinched_player,
                               const std::array<int, sim::kMaxPlayers>& team_of) {
    if (!goldman_on) return -1;
    if (team_mode && clinched_player >= 0) return team_of[static_cast<std::size_t>(clinched_player)];
    return clinched_player;
}

}  // namespace bomber::game
