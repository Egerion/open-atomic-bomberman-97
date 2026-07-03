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

// White-box: force a disease on a player (tests the effect, not the RNG roll).
inline void infect(Player& p, Disease d, int timer = 300) {
    p.disease[static_cast<int>(d)] = true;
    p.disease_timer = timer;
    p.disease_fresh = 0;
}

}  // namespace bomber::sim::test
