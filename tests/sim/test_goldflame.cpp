// Goldflame — reverse-engineered from the pickup dispatcher (sub_41E21E case 8
// sets flag +94) and bomb placement (sub_41EB13). Goldflame is a FLAG, not a
// flame stat: at drop time the blast reach is computed as max(gridW, gridH).
// Ordering in sub_41EB13: short-flame forces reach 1 FIRST, then goldflame
// OVERRIDES to max(cols, rows) — so goldflame beats short-flame. See
// docs/re/facts.md "Goldflame".

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>

#include "helpers.hpp"

using namespace bomber::sim;
using namespace bomber::sim::test;

TEST_CASE("goldflame pickup sets a flag and leaves the flame stat untouched") {
    Simulation s(open_config());
    Player& p = s.state().players[0];
    const int flame_before = p.flame;

    s.state().floor[p.tile_y()][p.tile_x()] = PowerupType::Goldflame;
    s.tick(TickInputs{});
    CHECK(s.state().players[0].goldflame);
    CHECK(s.state().players[0].flame == flame_before);  // not clobbered to 99
}

TEST_CASE("a goldflame bomb reaches max(gridW, gridH) cells") {
    Simulation s(open_config());
    Player& p = s.state().players[0];
    p.goldflame = true;
    p.flame = 2;  // the stored stat is irrelevant while goldflame is set

    s.tick(press1(0));
    REQUIRE(s.state().bombs.size() == 1);
    CHECK(s.state().bombs[0].flame == std::max(kGridWidth, kGridHeight));
}

TEST_CASE("goldflame overrides short-flame (goldflame wins)") {
    Simulation s(open_config());
    Player& p = s.state().players[0];
    p.goldflame = true;
    p.flame = 5;
    infect(p, Disease::ShortFlame);

    s.tick(press1(0));
    REQUIRE(s.state().bombs.size() == 1);
    // sub_41EB13 sets v9=1 for short-flame, then goldflame overrides it.
    CHECK(s.state().bombs[0].flame == std::max(kGridWidth, kGridHeight));
}

TEST_CASE("short-flame without goldflame still clamps the blast to 1") {
    Simulation s(open_config());
    Player& p = s.state().players[0];
    p.flame = 6;
    infect(p, Disease::ShortFlame);

    s.tick(press1(0));
    REQUIRE(s.state().bombs.size() == 1);
    CHECK(s.state().bombs[0].flame == 1);
}

TEST_CASE("a plain flame stat still drives a non-goldflame bomb") {
    Simulation s(open_config());
    Player& p = s.state().players[0];
    p.flame = 4;
    s.tick(press1(0));
    REQUIRE(s.state().bombs.size() == 1);
    CHECK(s.state().bombs[0].flame == 4);
}
