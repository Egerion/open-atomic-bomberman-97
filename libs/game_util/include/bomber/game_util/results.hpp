#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "bomber/sim/constants.hpp"
#include "bomber/sim/event.hpp"

// Pure helpers for the RESULTS scoreboard (docs/re/results-and-options.md §1,
// sub_42A3F6's middle tier). Deliberately DEPENDENCY-LIGHT — several SDL-free
// suites include this with only bomber::sim linked — so nothing here may reach
// for libs/net; net_tally.hpp is the online counterpart that does.

namespace bomber::game {

// Tallies one sim tick's PlayerDied events into a per-round kill counter (§1's
// sub_421B0F field). PlayerDied.data carries the killer's index per event.hpp:
// -1 = no attributable killer (enclosure/warphole crush), data == the victim's
// own index = a self-kill.
//
// Self-kill semantics are OUR OWN choice: §1 documents the counter but does not
// pin whether a player who blows themselves up increments it. We EXCLUDE
// self-kills. Revisit if the binary is found to credit them.
inline void tally_kills(const std::vector<sim::Event>& events,
                        std::array<int, sim::kMaxPlayers>& kill_count) {
    for (const sim::Event& e : events) {
        if (e.type != sim::Event::Type::PlayerDied) continue;
        // bugprone-signed-char-misuse: e.data (std::int8_t) is a genuine signed
        // small int here — casting through unsigned char would turn the -1
        // sentinel into 255 and break the check below.
        const int killer = e.data;  // NOLINT(bugprone-signed-char-misuse)
        if (killer < 0 || killer >= sim::kMaxPlayers) continue;  // no killer
        if (killer == e.player) continue;                        // self-kill: excluded
        ++kill_count[static_cast<std::size_t>(killer)];
    }
}

// The §1 v73 clinch's win_by_kills branch: the highest round-kill total
// (sub_421B0F) against dword_464A7C, with the leader count required to be
// exactly 1. Returns the clinching index, or -1.
//
// `present` gates which slots count — an absent slot's kill_count entry, always
// 0, must not tie for the lead against a real player who also has 0.
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
            continue;
        }
        if (kill_count[i] == best_count) ++leaders;
    }
    if (best < 0 || best_count < target) return -1;  // nobody reached the target
    if (leaders != 1) return -1;                     // a unique leader is required
    return best;
}

// The Goldman Roulette gold-player assignment (docs/re/goldman-roulette.md §2,
// pseudo.c 30004-30022): sub_42A3F6's RESULTS tier writes dword_46492C from v73,
// the MATCH-CLINCH winner — NOT the per-round winner — whenever goldman is on,
// else -1. In team mode the stored value is the clinching player's raw team id.
//
// Call ONLY when the RESULTS tier itself runs, i.e. round_winner() >= 0: a DRAW
// never reaches that tier in the original, so a pending gold player must be left
// untouched on a draw round.
inline int assign_gold_player(bool goldman_on, bool team_mode, int clinched_player,
                              const std::array<int, sim::kMaxPlayers>& team_of) {
    if (!goldman_on) return -1;
    if (team_mode && clinched_player >= 0)
        return team_of[static_cast<std::size_t>(clinched_player)];
    return clinched_player;
}

// Which slots the Gold Bomberman twinkle seeds on — sub_420F07's per-slot gate
// (pseudo.c 23658-23671, docs/re/goldman-roulette.md §6). Solo play compares
// dword_46492C against the slot INDEX; team play first re-encodes THE SLOT'S OWN
// +84 team byte into dword_46492C's doubled representation
// (`v11 = byte_461C18[152*i] ? 2 : 0`) and compares that — the SAME encoding on
// both sides of the ==, so every alive member of the gold team twinkles.
//
// The port's two encodings differ, and this is where they meet: gold_player
// stores the RAW 0/1 team id (assign_gold_player above) while the hashed sim
// Player::team carries that byte SHIFTED +1 (match_factory.hpp reserves 0 for
// "no team / solo side"), so the original's re-encoding step lands here as
// `raw + 1`. Comparing them UNSHIFTED put the twinkle on the OPPOSING — freshly
// defeated — team whenever team 1 clinched, and on nobody when team 0 did.
// The alive gate stays at the call site (the original tests the +0 alive dword
// in the same && as this comparison).
inline bool gold_twinkle_matches(bool team_mode, int gold_player, int slot, int sim_team) {
    if (gold_player < 0) return false;
    if (team_mode) return sim_team == gold_player + 1;
    return slot == gold_player;
}

// The clinch screen's background (docs/re/frontend-flow.md "VICTORY" §3,
// CONFIRMED): team game -> aTeamU ("team%u"), else aVictoryU ("victory%u").
// `clinched_team` is the RAW 0/1 setup-screen team id, NOT the sim's shifted 1/2
// Player::team. Before this helper existed the port always resolved
// VICTORY<player> even under Team Play — a genuine gap, not a simplification.
inline std::string victory_background_name(bool team_mode, int clinched_player, int clinched_team) {
    return team_mode ? "TEAM" + std::to_string(clinched_team)
                     : "VICTORY" + std::to_string(clinched_player);
}

// Campaign AI-roster seeding (docs/re/campaign.md "Rover/ghost/AI roster —
// CORRECTED"): sub_40151B's per-stage starter loops `ai_count` times calling
// sub_422928, which each call picks a RANDOM slot (`rand()%10`) and claims it
// only if it currently reads OFF, otherwise re-rolling. NOT a sequential fill
// from slot 0. `lcg` is a presentation RNG, never sim::State::rng.
//
// `occupied` marks slots already taken — confirmed 2026-07-30 from the raw
// disassembly: sub_422928 @0x422977 tests the candidate's type byte (+0x10) and
// writes 1 only when it reads 0, and its caller sub_40151B never clears a slot
// (the per-slot sub_42288C at 0x40156C only resets a "dialog already shown"
// latch). So a campaign STAGE ADVANCE adds `ai_count` AI ON TOP of the standing
// roster. The default "all free" keeps fresh-roster callers reading unchanged.
inline std::vector<int> seed_campaign_ai_slots(std::uint32_t& lcg, int ai_count,
                                               std::array<bool, sim::kMaxPlayers> occupied = {}) {
    std::array<bool, sim::kMaxPlayers> claimed{};
    int free_slots = 0;
    for (int i = 0; i < sim::kMaxPlayers; ++i)
        if (!occupied[static_cast<std::size_t>(i)]) ++free_slots;
    const int to_seed = ai_count < 0 ? 0 : (ai_count > free_slots ? free_slots : ai_count);
    int seeded = 0;
    int guard = 0;
    // sub_422928 retries up to 100 rand() draws PER call before giving up
    // silently; a shared budget stands in for that per-call cap.
    while (seeded < to_seed && guard < 1000) {
        ++guard;
        lcg = lcg * 1664525u + 1013904223u;
        const int slot = static_cast<int>((lcg >> 16) % sim::kMaxPlayers);
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

// Campaign round-pacing clauses 4-5 (sub_4016DA, confirmed against pseudo.c
// 4634-4648): walk slots 0..9, and bail the instant ANY present, non-COMPUTER,
// ALIVE slot is found. Falling through means every human/joystick slot is dead.
// `slot_type[i]==1` is COMPUTER (setup_type_'s convention: 0=OFF, 1=COMPUTER,
// 2/3=human), and a live COMPUTER does NOT stop the fall-through — which is what
// makes this strictly wider than a mutual total wipeout.
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
