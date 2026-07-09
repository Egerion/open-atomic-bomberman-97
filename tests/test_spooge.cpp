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

// ---- Core-feel audit 2026-07-10 (facts.md "Core-feel audit" §3) ------------

TEST_CASE("spooge fuses cascade: each tile of the run burns one tick longer") {
    // sub_41EB13 is called with the run index n (1-based); sub_422EDE inits
    // the fuse-elapsed to -50*n ms == +n ticks of countdown, so the line pops
    // one tile per tick, near to far. The underfoot bomb (a normal drop) keeps
    // the plain duration.
    Simulation s(open_config());
    Player& p = s.state().players[0];
    p.spooge = true;
    p.max_bombs = 4;
    p.facing = Direction::Right;
    spray(s);  // 3 ticks pass: drop, release, spray
    REQUIRE(s.state().bombs.size() == 4);
    const std::int32_t base = s.state().tuning.fuse_frames;
    // Underfoot bomb placed on tick 1 (fuse ticks down on its own placement
    // tick and the two after: base-3); the run placed on tick 3 with +1/+2/+3
    // staggers, each losing that tick's decrement: base, base+1, base+2.
    CHECK(s.state().bombs[0].fuse == base - 3);
    CHECK(s.state().bombs[1].fuse == base);
    CHECK(s.state().bombs[2].fuse == base + 1);
    CHECK(s.state().bombs[3].fuse == base + 2);
    // The stagger lives in the countdown only; the stored duration is flat.
    for (const auto& b : s.state().bombs) CHECK(b.fuse_init == base);
}

TEST_CASE("the spray stops short of a live player in the lane (sub_421CB5)") {
    Simulation s(open_config());
    Player& p = s.state().players[0];
    p.spooge = true;
    p.max_bombs = 5;
    p.facing = Direction::Right;
    // Park the second player at (2,0), in the spray path.
    Player& q = s.state().players[1];
    q.x = 2 * kTileWF + kTileWF / 2;
    q.y = kTileHF / 2;
    spray(s);
    CHECK(bomb_at_tile(s.state(), 0, 0));   // underfoot
    CHECK(bomb_at_tile(s.state(), 1, 0));   // one tile of run
    CHECK(!bomb_at_tile(s.state(), 2, 0));  // stopped at the player
    CHECK(s.state().bombs.size() == 2);
}

TEST_CASE("standing on someone ELSE'S bomb does not fire the spooger") {
    // The spooge branch requires the underfoot bomb's owner to be the player
    // (sub_41F29B: sub_422E48 result's owner word +62 == self).
    Simulation s(open_config());
    Player& p = s.state().players[0];
    p.spooge = true;
    p.max_bombs = 5;
    p.facing = Direction::Right;
    Bomb b;
    b.active = true;
    b.owner = 1;  // the OTHER player's bomb, right underfoot
    b.x = kTileWF / 2;
    b.y = kTileHF / 2;
    b.fuse = 10000;
    s.state().bombs.push_back(b);
    s.tick(press1(0));
    // Neither a spray nor a drop (the tile is occupied): nothing changed.
    CHECK(s.state().bombs.size() == 1);
}
