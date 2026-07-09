// Ice / input-lag (VALUELST ids 450-460, Hockey Rink = level index 2):
// a fixed input-response delay applied to HUMAN players' desired movement
// direction, faithful to sub_41F29B's per-player history ring buffer
// (~23058-23078). AI players are exempt (gated on the player-type byte
// +16 != 1 in the original). See docs/re/facts.md "Ice / input-lag".

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "helpers.hpp"

using namespace bomber::sim;
using namespace bomber::sim::test;

namespace {

MatchConfig hockey_config() {
    MatchConfig cfg = open_config();
    for (auto& row : cfg.cells) row.fill(Cell::Blank);  // open runway, no pillars
    cfg.tuning.level_index = 2;                         // Hockey Rink
    return cfg;
}

}  // namespace

TEST_CASE(
    "VALUELST defaults: only Hockey Rink (2) has ice delay, only Haunted House (7) has regen") {
    Tuning t;
    for (int i = 0; i < 11; ++i) {
        CHECK(t.ice_delay_ms[i] == (i == 2 ? 250 : 0));
        CHECK(t.regen_seconds[i] == (i == 7 ? 4 : 0));
    }
    CHECK(t.regen_clear_radius == 4);
    CHECK(t.level_index == 0);
}

TEST_CASE("ice delay is inert off Hockey Rink: movement matches the plain speed-budget rule") {
    Simulation s(open_config());  // level_index defaults to 0 ("new traditionalist")
    Player& p = s.state().players[0];
    TickInputs right;
    right.players[0].right = true;
    const int x0 = p.x;
    run(s, 10, right);
    int moved = (p.x - x0) / 100;
    long budget = 0, expected = 0;
    for (int t = 0; t < 10; ++t) {
        budget += s.state().tuning.start_speed;
        while (budget > 0) {
            budget -= 100;
            ++expected;
        }
    }
    CHECK(moved == static_cast<int>(expected));   // identical to test_move.cpp's baseline
    for (auto v : p.ice_history) CHECK(v == -1);  // buffer never written off-level
}

TEST_CASE("Hockey Rink delays a human player's first step by exactly 5 ticks (250ms/50ms)") {
    Simulation s(hockey_config());
    Player& p = s.state().players[0];
    TickInputs right;
    right.players[0].right = true;
    const int x0 = p.x;

    // want_godir is pushed into the history every tick starting tick 1; the
    // buffer starts all "-1" (no direction) at setup, so index 5 (the
    // 250ms-old sample) only starts holding a REAL sample from tick 6 on —
    // movement stays frozen for the first 5 ticks.
    for (int t = 1; t <= 5; ++t) {
        s.tick(right);
        CHECK(p.x == x0);
    }
    s.tick(right);  // tick 6: the delayed sample finally reads "Right"
    CHECK(p.x > x0);
}

TEST_CASE("Hockey Rink still lets a player through, just later — same total distance, shifted") {
    // Compare a Hockey Rink run to a plain (level_index 0) run of the SAME
    // length: the ice-delayed run should be roughly a right-shift of the
    // undelayed one once movement starts (same speed budget, later onset).
    Simulation plain(open_config());
    Simulation ice(hockey_config());
    TickInputs right;
    right.players[0].right = true;
    const int x0 = ice.state().players[0].x;
    run(plain, 60, right);
    run(ice, 60, right);
    CHECK(ice.state().players[0].x < plain.state().players[0].x);  // strictly behind
    CHECK(ice.state().players[0].x > x0);                          // but still made progress
}

TEST_CASE("AI players are exempt from ice delay: their history buffer is never written") {
    MatchConfig cfg = hockey_config();
    cfg.player_count = 2;
    cfg.spawns = {{0, 0}, {14, 10}};
    cfg.ai[1] = true;  // player 1 is computer-controlled
    Simulation s(cfg);
    TickInputs in;
    in.players[0].right = true;  // human: should start buffering
    for (int t = 0; t < 40; ++t) s.tick(in);

    bool human_wrote_history = false;
    for (auto v : s.state().players[0].ice_history)
        if (v != -1) human_wrote_history = true;
    CHECK(human_wrote_history);

    for (auto v : s.state().players[1].ice_history)
        CHECK(v == -1);  // AI: MovementSystem::ice_delay returns before touching it
}

TEST_CASE("the ice-history ring buffer is part of the hashed state") {
    Simulation a(open_config());
    Simulation b(open_config());
    b.state().players[0].ice_history[0] = 1;  // white-box: differ by one buffered sample
    CHECK(a.hash() != b.hash());
}
