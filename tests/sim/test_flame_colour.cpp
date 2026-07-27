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

#include "bomber/sim/rng.hpp"
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

// Brick-reveal cure roll (empty hook, RNG-count only). sub_425107's first
// statement on every brick ignite draws rand() modulo 30 and calls sub_42BE0B
// on a zero result;
// sub_42BE0B is empty (pseudo.c 30942), so the roll only CONSUMES one RNG
// draw with no gameplay effect. The port must reproduce that draw or its
// whole downstream RNG stream drifts one step per brick reveal. Verified by
// two identical sims where one bomb's blast hits a plain brick (no hidden
// token, no overpowered relocate, no floor powerup -> the ONLY RNG that tick
// is the cure roll) and the other's hits blank floor: the brick run's rng
// must be exactly one xorshift step ahead of the blank run's.
TEST_CASE("a brick ignite consumes exactly one RNG draw (empty cure hook)") {
    auto build = [](bool brick) {
        MatchConfig cfg = open_config();  // (odd,odd) pillars, no scattered powerups
        Simulation s(cfg);
        State& st = s.state();
        // Keep both players ALIVE (2 sides, else bombs F1's round-end freeze
        // holds the fuse and it never explodes) but well clear of the blast so
        // no death scatter draws. Player 1 stays at its (14,10) corner spawn.
        st.players[0].x = 8 * kTileWF + kTileWF / 2;
        st.players[0].y = 8 * kTileHF + kTileHF / 2;
        // (3,0) is blank in open_config (y even); make it a brick in one run.
        if (brick) st.cells[0][3] = Cell::Brick;
        // A bomb at (2,0), flame 1, fuse 1: its right arm hits (3,0) and nothing
        // else it touches ((1,0),(2,1),(2,0)) draws RNG. hidden[0][3] is None
        // (spawn_counts 0), so the reveal path draws nothing but the cure roll.
        put_bomb(st, 2, 0, /*owner=*/0, /*colour=*/0, /*flame=*/1, /*fuse=*/1);
        return s;
    };
    Simulation with_brick = build(true), no_brick = build(false);
    const std::uint32_t rng0 = with_brick.state().rng;
    REQUIRE(no_brick.state().rng == rng0);  // identical setup RNG position

    with_brick.tick(TickInputs{});  // fuse 1 -> 0, explodes, right arm hits the brick
    no_brick.tick(TickInputs{});    // same, but the arm runs into blank floor

    REQUIRE(with_brick.state().burning[0][3] > 0);  // the brick really ignited
    // Blank run: no RNG drawn this tick. Brick run: exactly the one cure roll.
    CHECK(no_brick.state().rng == rng0);
    // One xorshift32 step (the rng.hpp stream) applied to the pre-explosion rng.
    std::uint32_t expected = rng0;
    expected ^= expected << 13;
    expected ^= expected >> 17;
    expected ^= expected << 5;
    CHECK(with_brick.state().rng == expected);
}
