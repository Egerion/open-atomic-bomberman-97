#pragma once

#include <array>
#include <cstdint>
#include <string>
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
        // bugprone-signed-char-misuse (NOLINT below) — e.data (std::int8_t)
        // is a genuine signed small int here (-1 == "no killer" sentinel,
        // checked right below); casting through unsigned char first would
        // turn -1 into 255 and break that sentinel check.
        int killer = e.data;  // NOLINT(bugprone-signed-char-misuse)
        if (killer < 0 || killer >= sim::kMaxPlayers) continue;  // no killer
        if (killer == e.player) continue;                        // self-kill: excluded
        ++kill_count[static_cast<std::size_t>(killer)];
    }
}

// The §1 v73 match-clinch check's win_by_kills branch: "in team mode with
// win_by_kills set, the clinch compares the highest round-kill total
// (sub_421B0F) against dword_464A7C (the wins-needed target), breaking ties
// by requiring the leader count to be exactly 1". Returns the clinching
// player's index, or -1 if no single player has both reached the target AND
// uniquely holds the highest kill total.
//
// `present` gates which slots are considered (an absent/OFF slot's kill_count
// entry, always 0, must not accidentally tie for the lead against a real
// player who also has 0 kills).
inline int win_by_kills_clinch(const std::array<int, sim::kMaxPlayers>& kill_count,
                               const std::array<bool, sim::kMaxPlayers>& present, int target) {
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
    if (leaders != 1) return -1;                     // leader count 1: unique leader required
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
    if (team_mode && clinched_player >= 0)
        return team_of[static_cast<std::size_t>(clinched_player)];
    return clinched_player;
}

// The match-clinch outcome screen's background name (docs/re/frontend-flow.md
// "VICTORY" §3, CONFIRMED): "Team game -> aTeamU (\"team%u\" -> TEAM0/
// TEAM1.PCX); else -> aVictoryU (\"victory%u\" -> VICTORY0..VICTORY9.PCX)".
// `clinched_team` is the clinching player's RAW 0/1 setup-screen team id
// (team_of[clinched_player], same id present_scoreboard's "TEAM %u WINS THE
// MATCH!" line and the setup-screen team-marker glyph already use) — NOT the
// sim's shifted 1/2 Player::team. Both TEAM0.PCX/TEAM1.PCX are confirmed
// shipped in the install. Before this helper existed the port always
// resolved VICTORY<player> even under Team Play (a genuine end-to-end gap,
// not a deliberate simplification — TEAM%u used to be a documented "future
// hook" note from before Player::team landed).
inline std::string victory_background_name(bool team_mode, int clinched_player, int clinched_team) {
    return team_mode ? "TEAM" + std::to_string(clinched_team)
                     : "VICTORY" + std::to_string(clinched_player);
}

// Campaign AI-roster seeding (docs/re/campaign.md "Rover/ghost/AI roster —
// CORRECTED"): sub_40151B's per-stage starter loops `ai_count` times calling
// sub_422928, which each call picks a RANDOM slot (`rand()%10`) and, if that
// slot is currently OFF, claims it (flips to COMPUTER) — otherwise it keeps
// re-rolling. This is NOT a sequential fill from slot 0. `lcg` is advanced
// in place (an LCG step per draw, the same shape as GameApp's setup_lcg_/
// goldman_lcg_ presentation RNGs — never sim::State::rng). Returns the set of
// newly-COMPUTER slot indices (ascending, for test convenience); the caller
// applies them to setup_type_.
//
// `occupied` marks the slots that are ALREADY taken and must not be claimed —
// confirmed 2026-07-30 from the raw disassembly of the two functions involved.
// sub_422928 @0x422977 tests the candidate slot's own type byte (+0x10) and
// only writes 1 when it currently reads 0, re-rolling otherwise; and its caller
// sub_40151B @0x40151B never clears a slot at all — the per-slot call it makes
// first (sub_42288C, 0x40156C) only resets a "dialog already shown" latch. So a
// campaign STAGE ADVANCE adds `ai_count` AI on top of the roster already
// standing; it does not start from a blank one. This parameter defaults to "all
// free" so the pure unit tests and any fresh-roster caller read unchanged.
inline std::vector<int> seed_campaign_ai_slots(std::uint32_t& lcg, int ai_count,
                                               std::array<bool, sim::kMaxPlayers> occupied = {}) {
    std::array<bool, sim::kMaxPlayers> claimed{};
    int free_slots = 0;
    for (int i = 0; i < sim::kMaxPlayers; ++i)
        if (!occupied[static_cast<std::size_t>(i)]) ++free_slots;
    int to_seed = ai_count < 0 ? 0 : (ai_count > free_slots ? free_slots : ai_count);
    int seeded = 0;
    int guard = 0;
    // sub_422928 itself retries up to 100 rand() draws PER call before giving
    // up silently; since we only ever seed up to kMaxPlayers distinct slots
    // and always have a free one available while seeded < to_seed, a
    // generous shared retry budget stands in for that per-call cap.
    while (seeded < to_seed && guard < 1000) {
        ++guard;
        lcg = lcg * 1664525u + 1013904223u;
        int slot = static_cast<int>((lcg >> 16) % sim::kMaxPlayers);
        if (occupied[static_cast<std::size_t>(slot)]) continue;
        if (claimed[static_cast<std::size_t>(slot)]) continue;
        claimed[static_cast<std::size_t>(slot)] = true;
        ++seeded;
    }
    std::vector<int> slots;
    for (int i = 0; i < sim::kMaxPlayers; ++i)
        if (claimed[static_cast<std::size_t>(i)]) slots.push_back(i);
    return slots;
}

// Campaign round-pacing clauses 4-5 (docs/re/campaign.md "Round pacing",
// sub_4016DA), confirmed against pseudo.c 4634-4648, which walks slots 0..9,
// asks sub_421DD2 for each slot's type through an out-param, and returns as
// soon as one slot has a type that is neither 0 nor 1 AND sub_4228C4 says it is
// alive — bail (false, "a human/joystick survivor exists") the instant ANY present,
// non-COMPUTER, ALIVE slot is found; falling through the loop means every
// human/joystick slot is dead (true). `slot_type[i]==1` is COMPUTER, matching
// setup_type_'s own convention (0=OFF, 1=COMPUTER, 2/3=human). Strictly wider
// than a plain draw (mutual TOTAL wipeout) — this also fires when a COMPUTER
// side is the sole survivor, which the original force-replays rather than
// crediting as a campaign win.
inline bool campaign_round_needs_replay(const std::array<bool, sim::kMaxPlayers>& present,
                                        const std::array<bool, sim::kMaxPlayers>& alive,
                                        const std::array<int, sim::kMaxPlayers>& slot_type) {
    for (int i = 0; i < sim::kMaxPlayers; ++i) {
        if (!present[i] || !alive[i]) continue;
        if (slot_type[i] == 1) continue;  // COMPUTER: doesn't save the round
        return false;                     // a human/joystick slot is alive
    }
    return true;
}

}  // namespace bomber::game
