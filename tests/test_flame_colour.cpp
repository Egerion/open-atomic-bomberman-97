// Bomb/flame colour vs owner (docs/re/facts.md "Bomb/flame colour is not the
// owner"): the original packs a COLOUR byte (bomb +60, set once at creation
// from the placer) and an OWNER word (+62) into one dword. A flame-arm chain
// hit transfers ONLY the owner word (sub_42331C pseudo.c 25644), so kill
// credit moves to the chainer while the chained bomb — and every flame its
// explosion casts (sub_426FCC stores both fields in the cell record) — keeps
// the original placer's colour. Overlapping explosions from different
// players therefore keep their own colours on screen.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "helpers.hpp"

using namespace bomber::sim;
using namespace bomber::sim::test;

namespace {

void put_bomb(State& st, int tx, int ty, std::uint8_t owner, std::uint8_t colour, int flame,
              int fuse) {
    Bomb b;
    b.active = true;
    b.id = st.next_bomb_id++;
    b.owner = owner;
    b.colour = colour;
    b.x = tx * kTileWF + kTileWF / 2;
    b.y = ty * kTileHF + kTileHF / 2;
    b.flame = flame;
    b.fuse = fuse;
    b.fuse_init = fuse;
    st.players[owner].bombs_placed++;
    st.bombs.push_back(b);
}

}  // namespace

TEST_CASE("a placed bomb wears its placer's colour") {
    Simulation s(open_config());
    s.tick(press1(0));  // player 0 drops at its spawn
    REQUIRE(!s.state().bombs.empty());
    CHECK(s.state().bombs[0].owner == 0);
    CHECK(s.state().bombs[0].colour == 0);
}

TEST_CASE("a chain hit moves the owner word but NOT the colour byte") {
    Simulation s(open_config());
    State& st = s.state();
    // Clear row 0 so the arm travels: open_config leaves (odd,odd) pillars,
    // row 0 is already blank. P0's bomb west, P1's bomb two tiles east.
    put_bomb(st, 2, 0, /*owner=*/0, /*colour=*/0, /*flame=*/2, /*fuse=*/1);
    put_bomb(st, 4, 0, /*owner=*/1, /*colour=*/1, /*flame=*/2, /*fuse=*/10000);
    st.players[0].x = 8 * kTileWF;  // keep both players clear of the blasts
    st.players[0].y = 8 * kTileHF + kTileHF / 2;
    st.players[1].x = 10 * kTileWF;
    st.players[1].y = 8 * kTileHF + kTileHF / 2;

    s.tick(TickInputs{});  // bomb A explodes; its arm reaches bomb B
    // The arm's own cells wear A's colour; B is chain-queued with its owner
    // word rewritten to the chainer and its colour byte untouched.
    CHECK(st.flame[0][3] > 0);
    CHECK(st.flame_owner[0][3] == 0);
    CHECK(st.flame_colour[0][3] == 0);
    REQUIRE(!st.bombs.empty());
    const Bomb* b = nullptr;
    for (const auto& bb : st.bombs)
        if (bb.active && bb.tile_x() == 4 && bb.tile_y() == 0) b = &bb;
    REQUIRE(b != nullptr);
    CHECK(b->owner == 0);   // pseudo.c 25644: the +62 word transfers
    CHECK(b->colour == 1);  // the +60 colour byte does NOT

    s.tick(TickInputs{});  // the deferred chain fires: B explodes
    // B's flames: kill credit to the chainer (owner 0), drawn colour still
    // the original placer's (colour 1).
    CHECK(st.flame[0][4] > 0);
    CHECK(st.flame_owner[0][4] == 0);
    CHECK(st.flame_colour[0][4] == 1);
    // And an arm cell of B's explosion east of the epicentre agrees.
    CHECK(st.flame[0][5] > 0);
    CHECK(st.flame_owner[0][5] == 0);
    CHECK(st.flame_colour[0][5] == 1);
}

TEST_CASE("grab + throw preserves the carried bomb's colour") {
    Simulation s(open_config());
    State& st = s.state();
    Player& p = st.players[0];
    p.grab = true;
    s.tick(press1(0));  // drop own bomb underfoot
    REQUIRE(!st.bombs.empty());
    st.bombs[0].colour = 3;  // white-box: divergent colour to prove the plumbing
    s.tick(TickInputs{});    // release the key (edge reset)
    s.tick(press1(0));       // grab it
    REQUIRE(p.carrying);
    CHECK(p.carried_colour == 3);
    // Wait out the pickup pause, then release -> throw.
    run(s, st.tuning.pickup_pause + 1, press1(0));
    s.tick(TickInputs{});  // key released: the carried bomb is thrown
    REQUIRE(!p.carrying);
    REQUIRE(!st.bombs.empty());
    bool found = false;
    for (const auto& b : st.bombs)
        if (b.active && b.colour == 3) found = true;
    CHECK(found);
}

TEST_CASE("colour fields are hashed state") {
    Simulation a(open_config());
    Simulation b(open_config());
    put_bomb(a.state(), 2, 0, 0, 0, 2, 100);
    put_bomb(b.state(), 2, 0, 0, 0, 2, 100);
    b.state().bombs[0].colour = 1;
    CHECK(a.hash() != b.hash());

    Simulation c(open_config());
    Simulation d(open_config());
    c.state().flame[0][1] = 10;
    d.state().flame[0][1] = 10;
    d.state().flame_colour[0][1] = 2;
    CHECK(c.hash() != d.hash());
}
