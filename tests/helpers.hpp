#pragma once

// Shared scaffolding for the sim test suites.

#include "bomber/sim/simulation.hpp"

namespace bomber::sim::test {

// Small open arena with the classic (odd,odd) pillar pattern, two players in
// opposite corners, and no random powerups.
inline MatchConfig open_config() {
    MatchConfig cfg;
    for (int y = 0; y < kGridHeight; ++y)
        for (int x = 0; x < kGridWidth; ++x)
            cfg.cells[y][x] = (x % 2 == 1 && y % 2 == 1) ? Cell::Solid : Cell::Blank;
    cfg.spawns = {{0, 0}, {14, 10}};
    cfg.player_count = 2;
    cfg.seed = 7;
    for (auto& c : cfg.tuning.spawn_counts) c = 0;
    // Disarm the round-start input freeze (VALUELST id 30, ~1 s of dead
    // input at every round start — facts.md "Round-start input freeze") so
    // scenarios keep acting from tick 0; test_freeze.cpp covers the freeze
    // itself.
    cfg.tuning.input_freeze_ticks = 0;
    return cfg;
}

inline TickInputs press1(int player) {
    TickInputs in;
    in.players[player].action1 = true;
    return in;
}

inline TickInputs press2(int player) {
    TickInputs in;
    in.players[player].action2 = true;
    return in;
}

inline void run(Simulation& s, int ticks, const TickInputs& in = TickInputs{}) {
    for (int i = 0; i < ticks; ++i) s.tick(in);
}

// Keep >= 2 alive SIDES so a bomb-physics isolation test is not frozen by the
// bombs F1 round-decided freeze (sub_42331C @ pseudo.c 25603, docs/re/audit/
// bombs.md finding 1): once <= 1 side remains the original halts every fuse,
// chain and trigger detonation. Tests that used to KILL the spawned players to
// keep them "out of the lane" now instead park both at the far corners, still
// alive, clear of the row-0 / near-corner lanes these tests use. Both players
// stay present && alive; a bomb owned by either still resolves normally.
inline void park_players_clear(Simulation& s) {
    s.state().players[0].x = 0 * kTileWF + kTileWF / 2;   // (0,0) corner
    s.state().players[0].y = 0 * kTileHF + kTileHF / 2;
    s.state().players[1].x = 14 * kTileWF + kTileWF / 2;  // (14,10) corner
    s.state().players[1].y = 10 * kTileHF + kTileHF / 2;
}

// White-box: force a disease on a player (tests the effect, not the RNG roll).
inline void infect(Player& p, Disease d, int timer = 300) {
    p.disease[static_cast<int>(d)] = true;
    p.disease_timer = timer;
    p.disease_fresh = 0;
}

}  // namespace bomber::sim::test
