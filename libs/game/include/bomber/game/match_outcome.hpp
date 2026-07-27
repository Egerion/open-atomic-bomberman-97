#pragma once

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "bomber/assets/reslist.hpp"
#include "bomber/game/results.hpp"
#include "bomber/sim/constants.hpp"
#include "bomber/sim/simulation.hpp"  // sim::State + sim::winning_side

// The match-outcome predicates, promoted VERBATIM out of GameApp into free
// functions (ADR-0009 §10). They are shared by run_app, the RESULTS scoreboard,
// AND the match runtime — so a screen/runner that holds no GameApp&
// (ScoreboardScreen, MatchRunner) can still reach the SAME clinch/outcome logic
// run_app uses, as long as that logic is a free function taking the exact members
// it reads/writes as parameters (const& for reads; a non-const& for
// reset_match_scores's writes). ScoreboardScreen and MatchRunner call these
// directly; GameApp keeps thin 1-line forwarders (game_app.cpp) for its own last
// remaining caller, run_app, so it stays byte-identical (run_match /
// draw_player_row moved into MatchRunner). The full RE citations live here, on
// the code they describe; the forwarder declarations in game_app.hpp just point
// back to this header.

namespace bomber::game {

// The winner of the round just ended: the sole surviving player's index, or
// -1 for a draw (no survivor, or the clock ran out). Drives the Results
// screen's DRAW-vs-VICTORY choice and the "player N wins" naming.
inline int round_winner(const sim::State& s) {
    // A round win is exactly one SIDE of survivors with the clock still
    // running; a mutual wipe-out or a time-out is a draw. Mirrors sub_42A3F6,
    // which shows DRAW when the survivor query (sub_4219B0) returns none and
    // VICTORY<idx> for the lone survivor. Team-aware via sim::winning_side
    // (docs/re/ai.md TEAM follow-up, "our semantics"): teammates count as one
    // side, so a solo match (every team byte 0) is unchanged — the returned
    // slot is still the sole survivor, just resolved through the same-side
    // rule instead of a raw single-player check.
    // ORDER MATTERS, and it used to be wrong: the clock was tested FIRST, so a
    // round whose last opponent died with a second or two left was reported as
    // a DRAW. The round-decided window keeps ticking (nothing freezes
    // State::ticks_left), so the clock reliably reached 0 during it and stole
    // the win the player had just earned.
    //
    // The citation above describes one question, not two: sub_42A3F6 asks the
    // survivor query sub_4219B0 and shows DRAW only when it returns NOBODY.
    // Asking it first restores that. A genuine time-out still draws, because
    // with more than one side alive winning_side() has no single winner to
    // return — the draw falls out of the same question instead of pre-empting
    // it.
    //
    // NOTE: no separate "clock hit zero -> draw" path has ever been pinned in
    // the original; the old line was not backed by the routine it cited. If one
    // is ever found, this is where it goes.
    if (const int side = sim::winning_side(s); side >= 0) return side;
    return -1;
}

// Campaign round-pacing clauses 4-5 (docs/re/campaign.md "Round pacing",
// sub_4016DA): true when every PRESENT, ALIVE slot is COMPUTER
// (setup_type[i] == 1), i.e. no human/joystick player survives this round —
// regardless of whether an AI side is still alive and would otherwise be
// sim::winning_side()'s pick. The original force-ends (and, via
// `--dword_4648B0` undoing sub_40133F's next `++`, REPLAYS) the stage the
// instant this holds, so an AI "winning" a campaign round with no human left
// standing must NOT be credited as a win. Strictly wider than round_winner()'s
// plain draw (mutual total wipeout) — this also fires when a COMPUTER side is
// the sole sim-declared survivor.
inline bool campaign_no_human_survivor(bool campaign_active, const sim::State& s,
                                       const std::array<int, sim::kMaxPlayers>& setup_type) {
    // sub_4016DA clauses 4-5 (docs/re/campaign.md "Round pacing"), confirmed
    // against pseudo.c 4634-4648: `for (i=0;i<10;++i) { sub_421DD2(i,&type,0);
    // if (type!=1 && type && sub_4228C4(i)) return; }` — bail (no override)
    // the instant ANY present, non-COMPUTER, ALIVE slot is found; falling
    // through the loop means every human/joystick slot is dead. type==1 is
    // COMPUTER (setup_type's own convention, matching sub_421DD2's "type"
    // out-param) — a live COMPUTER slot does NOT stop the fall-through. The
    // actual predicate is the SDL-free campaign_round_needs_replay
    // (results.hpp, unit-tested in test_frontend.cpp) — this wrapper just
    // gathers the three per-slot arrays it needs from sim::State/setup_type.
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
// (docs/re/setup-screens.md dword_464964). Factored out so run_app's Results
// handler (the VICTORY-vs-scoreboard decision) and present_scoreboard (the
// scoreboard's own clinch/outcome-line render) agree on the SAME
// team_mode/win_by_kills gate — a divergence here would let the two disagree
// about whether the match is over.
inline bool is_team_mode(bool team_play, const sim::State& s,
                         const std::array<int, sim::kMaxPlayers>& setup_team) {
    // Team mode (docs/re/setup-screens.md dword_464964): any two ACTIVE
    // players sharing a MatchConfig team means team rows/strings apply.
    // setup_team[] is the frontend's per-slot +84 byte; team_play is the
    // game-type gate (start_match zeroes every slot's team when it is off,
    // so gating on team_play here keeps this in lockstep with the roster
    // actually built for the match in progress).
    if (!team_play) return false;
    std::array<bool, sim::kMaxPlayers> team_seen{};
    for (int i = 0; i < sim::kMaxPlayers; ++i) {
        if (!s.players[i].present) continue;
        int t = setup_team[i];
        if (t < 0 || t >= sim::kMaxPlayers) continue;
        if (team_seen[t]) return true;
        team_seen[t] = true;
    }
    return false;
}

// Credit the round win to `winner` — and, under Team Play, MIRROR the tally
// onto the winner's teammates.
//
// sub_421B56 @ 0x421B56 (one caller in the whole image, 0x42A919, the RESULTS
// tier of the round driver sub_42A3F6, passing a literal 1): it adds the award
// to the surviving slot's 16-bit win counter at record +0x6A — the very field
// the accessor sub_421AC8 reads back — and then, gated on the team-play flag
// dword_464964, walks all ten slots a second time and writes that counter into
// every OTHER slot that is PRESENT (+0x10) and carries the winner's team byte
// (+0x54).
//
// Two details of that second loop are load-bearing:
//   * it is a COPY, not another increment. Teammates END the round holding the
//     TEAM's total; they do not each accumulate a private share of it.
//   * the teammate filter tests PRESENT, not ALIVE, so a teammate who died this
//     round is credited too. That matters more for us than for the original:
//     round_winner() resolves to the lowest-indexed ALIVE member of the
//     surviving side, so without the mirror the member who happens to die every
//     round scores nothing at all.
//
// The mirror is what makes the SINGLE-SLOT reads downstream correct. The
// original's team clinch consults sub_421AC8 for the FIRST present member of
// each team only, and our RESULTS "Team N score" row (results_screens.cpp)
// faithfully does the same — fed an unmirrored counter it showed one member's
// share of the team's wins (typically 0) while the team was winning rounds. Our
// match_clinch() below scans every present slot instead, which is why the
// missing mirror only ever made the clinch LATE (bounded by pigeonhole), never
// unreachable; with the mirror in place the two reads agree by construction.
inline void award_round_win(std::array<int, sim::kMaxPlayers>& win_count, int winner,
                            bool team_play, const sim::State& s,
                            const std::array<int, sim::kMaxPlayers>& setup_team) {
    if (winner < 0 || winner >= sim::kMaxPlayers) return;
    ++win_count[winner];
    // dword_464964, the raw game-type gate — NOT is_team_mode()'s derived "two
    // active players share a team". They only differ when team play is on but
    // nobody actually shares a team, and then the loop below finds no teammate
    // anyway, so the faithful gate costs nothing.
    if (!team_play) return;
    for (int i = 0; i < sim::kMaxPlayers; ++i) {
        if (i == winner || !s.players[i].present) continue;
        if (setup_team[i] != setup_team[winner]) continue;
        win_count[i] = win_count[winner];
    }
}

// The §1 v73 match-clinch check, factored so run_app's Results handler and
// present_scoreboard call the identical predicate: the default win-count
// clinch, or (non-team + win_by_kills) the kill_count clinch via results.hpp's
// win_by_kills_clinch(). Returns the clinching player's index, or -1 if the
// match is not yet decided.
inline int match_clinch(const sim::State& s, bool team_play,
                        const std::array<int, sim::kMaxPlayers>& setup_team, bool win_by_kills,
                        const std::array<int, sim::kMaxPlayers>& kill_count,
                        const std::array<int, sim::kMaxPlayers>& win_count, int win_target) {
    // §1 v73 (sub_42A3F6, batch_0x4293E5.cpp:1189-1255): the clinch splits on
    // TEAM mode (dword_464964). The team branch is ALWAYS wins-based; the
    // kill-count clinch (win_by_kills_clinch: highest round-kill total >=
    // target, unique leader v78==1) lives ONLY in the NON-team branch's
    // dword_46497C sub-case (line 1243). win_by_kills is inherently a non-team
    // feature — team play forces it OFF (batch_0x405B3A.cpp:685-686
    // `if (dword_464964) dword_46497C = 0;`, mirrored at
    // options_screen.cpp's activate_row). The old `is_team_mode() &&
    // win_by_kills` gate was therefore DEAD (never true), silently falling the
    // "Win Matches By Kill Total" mode through to the round-win loop. Both call
    // sites (run_app's Results handler and present_scoreboard) share this gate.
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

// True when the DRAW/RESULTS screens may auto-advance after their dwell
// (all-AI roster or a demo/attract run, mirroring sub_42A3F6's
// `sub_42247A() || dword_4646B4` gate); false = a human match, which waits
// for Enter.
inline bool auto_advance_results(bool demo, int demo_ticks,
                                 const std::vector<std::pair<std::string, int>>& demo_shots,
                                 const std::array<int, sim::kMaxPlayers>& setup_type) {
    // sub_42A3F6's DRAW and RESULTS wait loops honour the 6 s auto-advance ONLY
    // when `sub_42247A() || dword_4646B4` (batch_0x4293E5.cpp:1109/1333):
    // sub_42247A returns 1 iff NO slot is human (every +16 type is OFF=0 or
    // CPU=1 — batch_0x421E80.cpp), and dword_4646B4 is the attract/demo flag. A
    // human match instead waits indefinitely for Enter. Our attract path is
    // all-AI and the --demo/--demo-shots path is scripted, so both collapse to:
    // auto-advance unless a real human (KEYBOARD=2 / JOYSTICK=3) is playing.
    if (demo || demo_ticks > 0 || !demo_shots.empty()) return true;
    for (int i = 0; i < sim::kMaxPlayers; ++i)
        if (setup_type[i] == 2 || setup_type[i] == 3) return false;  // a human slot
    return true;  // all-AI roster
}

// Reset the per-match win tally + read the win target getvalue(310) at the
// start of a fresh match (Menu -> StartMatch). Best-of-N, N = 2 by default.
// Writes win_count/kill_count/win_target (hence the non-const refs).
inline void reset_match_scores(std::array<int, sim::kMaxPlayers>& win_count,
                               std::array<int, sim::kMaxPlayers>& kill_count, int& win_target,
                               const assets::res::ValueList& values,
                               const std::optional<int>& num_to_win_match) {
    win_count.fill(0);
    kill_count.fill(0);
    // getvalue(310) "how many wins to win a match?" (first-column value, else
    // options.ini's num_to_win_match= if the VALUELST key is absent, else our
    // own fallback of 2 (task item 5 / §5: "num_to_win_match ... should seed
    // the frontend's win_target_ default"). The LEVEL & ROUNDS screen's WINS
    // row (present_map_select) still overrides on top of whichever default
    // wins here — this only affects the value shown before the player edits it.
    auto it = values.values.find(310);
    if (it != values.values.end())
        win_target = static_cast<int>(it->second);
    else
        win_target = num_to_win_match.value_or(2);
    if (win_target < 1) win_target = 1;
}

}  // namespace bomber::game
