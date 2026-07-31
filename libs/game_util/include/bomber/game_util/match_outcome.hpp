#pragma once

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "bomber/assets/reslist.hpp"
#include "bomber/game_util/results.hpp"
#include "bomber/sim/constants.hpp"
#include "bomber/sim/simulation.hpp"  // sim::State + sim::winning_side

// The match-outcome predicates, as free functions taking the exact members they
// read and write (ADR-0009 §10), so run_app, the RESULTS scoreboard and the
// match runtime reach the SAME clinch logic — a screen holding no GameApp&
// could not otherwise. GameApp keeps thin forwarders.

namespace bomber::game {

// The winner of the round just ended: the sole surviving SIDE's slot, or -1 for
// a draw. Mirrors sub_42A3F6, which shows DRAW when the survivor query
// (sub_4219B0) returns none and VICTORY<idx> for the lone survivor.
//
// ORDER MATTERS, and it used to be wrong: the clock was tested FIRST, so a round
// whose last opponent died with a second or two left was reported as a DRAW (the
// round-decided window keeps ticking, so the clock reliably reached 0 during
// it). The citation describes ONE question, not two — a genuine time-out still
// draws, because with more than one side alive winning_side() has no winner to
// return. No separate "clock hit zero -> draw" path has ever been pinned in the
// original; if one is found, this is where it goes.
inline int round_winner(const sim::State& s) {
    if (const int side = sim::winning_side(s); side >= 0) return side;
    return -1;
}

// Campaign round-pacing clauses 4-5 (sub_4016DA): the original force-ends and
// REPLAYS the stage the instant no human/joystick player survives (a decrement
// of dword_4648B0 undoes sub_40133F's next increment), so an AI "winning" with
// no human left standing must NOT be credited. The predicate is
// campaign_round_needs_replay (results.hpp); this only gathers its arrays.
inline bool campaign_no_human_survivor(bool campaign_active, const sim::State& s,
                                       const std::array<int, sim::kMaxPlayers>& setup_type) {
    if (!campaign_active) return false;
    std::array<bool, sim::kMaxPlayers> present{};
    std::array<bool, sim::kMaxPlayers> alive{};
    for (int i = 0; i < sim::kMaxPlayers; ++i) {
        present[i] = s.players[i].present;
        alive[i] = s.players[i].alive;
    }
    return campaign_round_needs_replay(present, alive, setup_type);
}

// True when at least two ACTIVE players share a MatchConfig team
// (docs/re/setup-screens.md dword_464964). Shared by run_app's Results handler
// and present_scoreboard so the two cannot disagree about whether the match is
// over. `team_play` is the game-type gate: start_match zeroes every slot's team
// when it is off, so gating on it keeps this in lockstep with the roster
// actually built for the match in progress.
inline bool is_team_mode(bool team_play, const sim::State& s,
                         const std::array<int, sim::kMaxPlayers>& setup_team) {
    if (!team_play) return false;
    std::array<bool, sim::kMaxPlayers> team_seen{};
    for (int i = 0; i < sim::kMaxPlayers; ++i) {
        if (!s.players[i].present) continue;
        const int t = setup_team[i];
        if (t < 0 || t >= sim::kMaxPlayers) continue;
        if (team_seen[t]) return true;
        team_seen[t] = true;
    }
    return false;
}

// Credit the round win to `winner` — and, under Team Play, MIRROR the tally onto
// the winner's teammates.
//
// sub_421B56 @0x421B56 (one caller in the image, 0x42A919, passing a literal 1)
// adds the award to the surviving slot's 16-bit counter at +0x6A — the field
// sub_421AC8 reads back — then, gated on dword_464964, walks all ten slots and
// writes that counter into every OTHER slot that is PRESENT (+0x10) and carries
// the winner's team byte (+0x54). Two details of that second loop are
// load-bearing: it is a COPY rather than another increment, so teammates end the
// round holding the TEAM's total; and it tests PRESENT, NOT ALIVE, so a teammate
// who died is credited too.
//
// Both matter more to us than to the original, because round_winner() resolves
// to the lowest-indexed ALIVE member and the "Team N score" row — like the
// original's clinch — reads the FIRST present member only. Without the mirror,
// the member who happens to die every round scores nothing, and that row shows
// one member's share of a total the team has actually won.
inline void award_round_win(std::array<int, sim::kMaxPlayers>& win_count, int winner,
                            bool team_play, const sim::State& s,
                            const std::array<int, sim::kMaxPlayers>& setup_team) {
    if (winner < 0 || winner >= sim::kMaxPlayers) return;
    ++win_count[winner];
    // dword_464964, the raw game-type gate — NOT is_team_mode()'s derived "two
    // active players share a team". They differ only when team play is on but
    // nobody shares a team, and then the loop finds no teammate anyway.
    if (!team_play) return;
    for (int i = 0; i < sim::kMaxPlayers; ++i) {
        if (i == winner || !s.players[i].present) continue;
        if (setup_team[i] != setup_team[winner]) continue;
        win_count[i] = win_count[winner];
    }
}

// The §1 v73 match clinch: the clinching player's index, or -1.
//
// sub_42A3F6 (batch_0x4293E5.cpp:1189-1255) splits on TEAM mode (dword_464964).
// The team branch is ALWAYS wins-based; the kill-count clinch lives ONLY in the
// NON-team branch's dword_46497C sub-case (line 1243). win_by_kills is
// inherently non-team — team play forces it OFF (batch_0x405B3A.cpp:685-686
// clears dword_46497C whenever dword_464964 is set). The old
// `is_team_mode() && win_by_kills` gate was therefore DEAD, silently falling
// "Win Matches By Kill Total" through to the round-win loop.
inline int match_clinch(const sim::State& s, bool team_play,
                        const std::array<int, sim::kMaxPlayers>& setup_team, bool win_by_kills,
                        const std::array<int, sim::kMaxPlayers>& kill_count,
                        const std::array<int, sim::kMaxPlayers>& win_count, int win_target) {
    if (!is_team_mode(team_play, s, setup_team) && win_by_kills) {
        std::array<bool, sim::kMaxPlayers> present{};
        for (int i = 0; i < sim::kMaxPlayers; ++i) present[i] = s.players[i].present;
        return win_by_kills_clinch(kill_count, present, win_target);
    }
    for (int i = 0; i < sim::kMaxPlayers; ++i) {
        if (!s.players[i].present) continue;
        if (win_count[i] >= win_target) return i;
    }
    return -1;
}

// Whether the DRAW/RESULTS screens may auto-advance after their dwell.
//
// sub_42A3F6's wait loops honour the 6 s auto-advance ONLY when
// `sub_42247A() || dword_4646B4` (batch_0x4293E5.cpp:1109/1333): sub_42247A
// returns 1 iff NO slot is human (every +16 type is OFF=0 or CPU=1) and
// dword_4646B4 is the attract/demo flag. A human match waits indefinitely for
// Enter. Our attract path is all-AI and --demo/--demo-shots is scripted, so both
// collapse to "auto-advance unless a real human (KEYBOARD=2 / JOYSTICK=3) plays".
inline bool auto_advance_results(bool demo, int demo_ticks,
                                 const std::vector<std::pair<std::string, int>>& demo_shots,
                                 const std::array<int, sim::kMaxPlayers>& setup_type) {
    if (demo || demo_ticks > 0 || !demo_shots.empty()) return true;
    for (int i = 0; i < sim::kMaxPlayers; ++i)
        if (setup_type[i] == 2 || setup_type[i] == 3) return false;  // a human slot
    return true;                                                     // all-AI roster
}

// Reset the per-match tally and read the win target at the start of a fresh
// match. getvalue(310) "how many wins to win a match?" wins; else options.ini's
// num_to_win_match=; else 2. The LEVEL & ROUNDS screen's WINS row still
// overrides on top, so this only affects the value shown before an edit.
inline void reset_match_scores(std::array<int, sim::kMaxPlayers>& win_count,
                               std::array<int, sim::kMaxPlayers>& kill_count, int& win_target,
                               const assets::res::ValueList& values,
                               const std::optional<int>& num_to_win_match) {
    win_count.fill(0);
    kill_count.fill(0);
    const auto it = values.values.find(310);
    win_target =
        it != values.values.end() ? static_cast<int>(it->second) : num_to_win_match.value_or(2);
    if (win_target < 1) win_target = 1;
}

}  // namespace bomber::game
