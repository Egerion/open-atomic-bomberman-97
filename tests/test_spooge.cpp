// Spooger powerup — a bomb underfoot plus a line sprayed in the facing
// direction, stopping at a wall, a bomb, a powerup, the field edge, or when
// the bomb supply runs out (the spooge branch of sub_41F29B). The drop block
// is edge-gated, so it takes two presses: one lays the underfoot bomb, a
// second (while standing on it) fires the whole run ahead. See docs/re/facts.md.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "helpers.hpp"

using namespace bomber::sim;
using namespace bomber::sim::test;

namespace {

int bombs_on_row(const State& s, int y) {
    int n = 0;
    for (auto& b : s.bombs)
        if (b.tile_y() == y) ++n;
    return n;
}

bool bomb_at_tile(const State& s, int x, int y) {
    for (auto& b : s.bombs)
        if (b.tile_x() == x && b.tile_y() == y) return true;
    return false;
}

// The two-press spray: lay the bomb underfoot, let go, press again to fire
// the run ahead (two distinct rising edges, as the original requires).
void spray(Simulation& s) {
    s.tick(press1(0));
    s.tick(TickInputs{});
    s.tick(press1(0));
}

}  // namespace

TEST_CASE("a single press only lays the underfoot bomb") {
    Simulation s(open_config());
    Player& p = s.state().players[0];
    p.spooge = true;
    p.max_bombs = 5;
    p.facing = Direction::Right;
    s.tick(press1(0));
    CHECK(s.state().bombs.size() == 1);
    CHECK(bomb_at_tile(s.state(), 0, 0));
    CHECK(!bomb_at_tile(s.state(), 1, 0));  // no line yet
}

TEST_CASE("the spray is limited by the bomb supply") {
    Simulation s(open_config());
    Player& p = s.state().players[0];
    p.spooge = true;
    p.max_bombs = 5;
    p.facing = Direction::Right;
    spray(s);
    CHECK(s.state().bombs.size() == 5);
    for (int x = 0; x < 5; ++x) CHECK(bomb_at_tile(s.state(), x, 0));  // (0,0)..(4,0)
    CHECK(!bomb_at_tile(s.state(), 5, 0));
}

TEST_CASE("the spray stops at a wall") {
    Simulation s(open_config());
    s.state().cells[0][3] = Cell::Solid;  // wall three tiles east
    Player& p = s.state().players[0];
    p.spooge = true;
    p.max_bombs = 10;
    p.facing = Direction::Right;
    spray(s);
    CHECK(bombs_on_row(s.state(), 0) == 3);  // (0,0),(1,0),(2,0)
    CHECK(!bomb_at_tile(s.state(), 3, 0));
}

TEST_CASE("without the spooger a spray leaves a single bomb") {
    Simulation s(open_config());
    Player& p = s.state().players[0];
    p.max_bombs = 5;
    p.facing = Direction::Right;
    spray(s);
    CHECK(s.state().bombs.size() == 1);
}

TEST_CASE("disease overrides carry to every sprayed bomb") {
    Simulation s(open_config());
    Player& p = s.state().players[0];
    p.spooge = true;
    p.max_bombs = 4;
    p.flame = 6;
    p.facing = Direction::Right;
    infect(p, Disease::ShortFlame);
    spray(s);
    CHECK(s.state().bombs.size() == 4);
    for (auto& b : s.state().bombs) CHECK(b.flame == 1);
}

TEST_CASE("the spray works down a column too") {
    Simulation s(open_config());
    Player& p = s.state().players[0];
    p.spooge = true;
    p.max_bombs = 3;
    p.facing = Direction::Down;  // column 0 is open (even x)
    spray(s);
    CHECK(s.state().bombs.size() == 3);
    for (int y = 0; y < 3; ++y) CHECK(bomb_at_tile(s.state(), 0, y));  // (0,0),(0,1),(0,2)
}
