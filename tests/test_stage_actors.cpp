// Stage-actor mechanics that go beyond the standalone conveyor/trampoline
// suites: dirarrows (type 0, bomb-only re-steer), warpholes (type 1, teleport),
// and the bomb-on-conveyor slide. Faithful to sub_42331C (bomb mover) and
// sub_41EC84 (player warp step-on). See docs/re/stage-actors.md §5-6.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "helpers.hpp"

using namespace bomber::sim;
using namespace bomber::sim::test;

namespace {

// godir: 0=Up, 1=Right, 2=Down, 3=Left.
constexpr std::uint8_t kEast = 1, kDown = 2;

// Tile-centre field coordinates (grid.hpp is sim-internal, unavailable here).
Fixed centre_x(int tx) { return tx * kTileWF + kTileWF / 2; }
Fixed centre_y(int ty) { return ty * kTileHF + kTileHF / 2; }

// Drop a live, long-fused bomb directly onto the state (white-box).
Bomb& add_bomb(State& st, int tx, int ty, bool moving = false,
               Direction dir = Direction::Right) {
    Bomb b;
    b.active = true;
    b.owner = 0;
    b.x = centre_x(tx);
    b.y = centre_y(ty);
    b.fuse = 100000;  // never detonate during these tests
    b.moving = moving;
    b.dir = dir;
    st.bombs.push_back(b);
    return st.bombs.back();
}

bool saw_warp(const Simulation& s) {
    for (const auto& e : s.state().events)
        if (e.type == Event::Type::WarpUsed) return true;
    return false;
}

}  // namespace

// ---- Dirarrows (type 0): players are NOT steered, bombs ARE. ---------------

TEST_CASE("a dirarrow does NOT steer a walking player (bomb-only, per RE)") {
    Simulation s(open_config());
    State& st = s.state();
    // A South arrow at (2,0). A player walking east across it must keep going
    // east — the player mover has no dirarrow branch (docs §5).
    st.actor_type[0][2] = ActorType::DirArrow;
    st.actor_dir[0][2] = kDown;
    Player& p = st.players[0];
    p.x = kTileWF / 2;  // (0,0) centre
    p.y = kTileHF / 2;

    TickInputs east;
    east.players[0].right = true;
    run(s, 20, east);  // walk east across the arrow tile

    CHECK(p.tile_y() == 0);      // never turned south
    CHECK(p.tile_x() >= 2);      // walked past the arrow column
    CHECK(p.facing == Direction::Right);
}

TEST_CASE("a dirarrow re-steers a sliding bomb onto its godir") {
    Simulation s(open_config());
    State& st = s.state();
    // A South arrow at (2,0); pillars sit at (odd,odd), so column 2 is open
    // downward. A bomb sliding east reaches (2,0) centre, turns south, and rolls
    // down column 2. Both axes are centre-aligned there, so the re-steer fires.
    st.actor_type[0][2] = ActorType::DirArrow;
    st.actor_dir[0][2] = kDown;

    add_bomb(st, 0, 0, /*moving=*/true, Direction::Right);
    run(s, 40, TickInputs{});  // slide east into the arrow, then south

    REQUIRE_FALSE(st.bombs.empty());
    const Bomb& b = st.bombs[0];
    CHECK(b.tile_x() == 2);         // turned at the arrow column
    CHECK(b.tile_y() > 0);          // now travelling south, off row 0
    CHECK(b.dir == Direction::Down);
}

// ---- Warpholes (type 1): teleport, no RNG. --------------------------------

TEST_CASE("a player centred on a warphole warps (two-phase) to the linked exit") {
    Simulation s(open_config());
    State& st = s.state();
    // Two linked warpholes at (0,0) and (6,0). Pre-resolve their destinations
    // the way apply_actors would (the sim reads warp_dest_* directly, no RNG).
    st.actor_type[0][0] = ActorType::Warphole;
    st.warp_dest_x[0][0] = 6;
    st.warp_dest_y[0][0] = 0;
    st.actor_type[0][6] = ActorType::Warphole;
    st.warp_dest_x[0][6] = 0;
    st.warp_dest_y[0][6] = 0;

    Player& p = st.players[0];
    p.x = kTileWF / 2;  // centred on (0,0)
    p.y = kTileHF / 2;

    const std::uint32_t rng_before = st.rng;
    s.tick(TickInputs{});  // settle -> START the warp (warp=18; state-gated)

    CHECK(saw_warp(s));              // WarpUsed fired on step-on
    CHECK(p.warp == 18);            // warp in progress (warp-out phase)
    CHECK(p.tile_x() == 0);         // has NOT moved yet — the two-phase warp

    // warp-out runs until warp reaches the midpoint (9), where the relocation
    // fires. From warp=18 that is 9 more ticks.
    run(s, 8, TickInputs{});         // warp -> 10, still at the entry
    CHECK(p.tile_x() == 0);
    s.tick(TickInputs{});            // this tick decrements to 9 -> relocate
    CHECK(p.warp == 9);
    CHECK(p.tile_x() == 6);          // relocated to the exit at the midpoint
    CHECK(p.tile_y() == 0);

    run(s, 9, TickInputs{});         // finish warp-in (9 -> 0)
    CHECK(p.warp == 0);             // warp complete: the player can move again
    CHECK(st.rng == rng_before);     // the whole warp drew NO RNG (contract)
    CHECK(p.warp_latch);             // latched against immediate re-warp
}

TEST_CASE("a player WALKING onto a warphole warps mid-walk (Gap 1 regression)") {
    // The real "stuck" bug: a player walking THROUGH a warphole in an open lane
    // never lands the tick exactly on the centre pixel (9 px/tick stride skips
    // it), so a post-walk-only trigger never fired and the player just crossed
    // the warphole. The per-pixel stepper now fires the step-on the instant a
    // 1-px step settles on the centre (sub_41EC84 v35 == -1). Warp pair
    // (2,0) -> (8,0); player starts a tile away at (0,0) and walks east.
    Simulation s(open_config());
    State& st = s.state();
    st.actor_type[0][2] = ActorType::Warphole;
    st.warp_dest_x[0][2] = 8;
    st.warp_dest_y[0][2] = 0;
    st.actor_type[0][8] = ActorType::Warphole;
    st.warp_dest_x[0][8] = 2;
    st.warp_dest_y[0][8] = 0;
    Player& p = st.players[0];
    p.x = kTileWF / 2;  // centred on (0,0), one+ tiles WEST of the warphole
    p.y = kTileHF / 2;

    TickInputs east;
    east.players[0].right = true;

    // Walk east until the warp starts (it must, within a few tiles of walking).
    bool started = false;
    for (int t = 0; t < 40 && !started; ++t) {
        s.tick(east);
        if (p.warp > 0) started = true;
    }
    REQUIRE(started);                 // the walk DID trigger the warp
    CHECK(p.warp_to_x == 8);          // captured the correct exit at step-on
    CHECK(p.warp_to_y == 0);

    // Finish the warp (keep holding east — input is ignored while warping).
    for (int t = 0; t < 20 && p.warp > 0; ++t) s.tick(east);
    CHECK(p.warp == 0);
    CHECK(p.tile_x() == 8);           // relocated to the linked exit
    CHECK(p.tile_y() == 0);

    // And now it can move again: walk DOWN off the exit (column 8 open).
    TickInputs down;
    down.players[0].down = true;
    const Fixed y0 = p.y;
    run(s, 20, down);
    CHECK(p.y > y0);                  // not frozen — control returned
    CHECK_FALSE(p.warp_latch);        // latch cleared once off the warphole
}

TEST_CASE("a warped player can move again after the warp (not stuck)") {
    // Regression for Gap 2: the old instantaneous-teleport model left the player
    // unable to move. Now the warp completes in 18 ticks and control returns.
    Simulation s(open_config());
    State& st = s.state();
    st.actor_type[0][0] = ActorType::Warphole;
    st.warp_dest_x[0][0] = 6;
    st.warp_dest_y[0][0] = 0;
    st.actor_type[0][6] = ActorType::Warphole;
    st.warp_dest_x[0][6] = 0;
    st.warp_dest_y[0][6] = 0;
    Player& p = st.players[0];
    p.x = kTileWF / 2;
    p.y = kTileHF / 2;

    run(s, 19, TickInputs{});        // full warp: start (1) + 18 countdown ticks
    REQUIRE(p.tile_x() == 6);
    REQUIRE(p.warp == 0);

    // Walk DOWN off the exit warphole (column 6 is open on the pillars board).
    TickInputs down;
    down.players[0].down = true;
    const Fixed y0 = p.y;
    run(s, 20, down);
    CHECK(p.y > y0);                 // the player actually moved: not stuck
    CHECK_FALSE(p.warp_latch);       // latch cleared once off the warphole tile
}

TEST_CASE("a warped player does not ping-pong at the exit") {
    Simulation s(open_config());
    State& st = s.state();
    st.actor_type[0][0] = ActorType::Warphole;
    st.warp_dest_x[0][0] = 6;
    st.warp_dest_y[0][0] = 0;
    st.actor_type[0][6] = ActorType::Warphole;
    st.warp_dest_x[0][6] = 0;
    st.warp_dest_y[0][6] = 0;
    Player& p = st.players[0];
    p.x = kTileWF / 2;
    p.y = kTileHF / 2;

    run(s, 19, TickInputs{});        // full warp to (6,0), latch set
    REQUIRE(p.tile_x() == 6);
    run(s, 5, TickInputs{});         // sit on the exit warphole

    CHECK(p.tile_x() == 6);          // stayed put: no bounce back to (0,0)
    CHECK(p.warp_latch);
}

TEST_CASE("a warphole with no partner leaves the player in place") {
    Simulation s(open_config());
    State& st = s.state();
    // Lone warphole: apply_actors would set its dest to itself; emulate that.
    st.actor_type[0][0] = ActorType::Warphole;
    st.warp_dest_x[0][0] = 0;
    st.warp_dest_y[0][0] = 0;
    Player& p = st.players[0];
    p.x = kTileWF / 2;
    p.y = kTileHF / 2;

    run(s, 19, TickInputs{});  // a self-linked warp is a harmless in-place hop
    CHECK(p.tile_x() == 0);
    CHECK(p.tile_y() == 0);
    CHECK(p.warp == 0);
}

TEST_CASE("the warphole exit tile is part of the hashed state") {
    // Two matches with the same warphole positions but different link targets
    // must hash differently (warp_dest_* is a hashed setup input).
    Simulation a(open_config());
    Simulation b(open_config());
    a.state().actor_type[0][0] = ActorType::Warphole;
    a.state().warp_dest_x[0][0] = 6;
    b.state().actor_type[0][0] = ActorType::Warphole;
    b.state().warp_dest_x[0][0] = 8;  // different exit
    CHECK(a.hash() != b.hash());
}

// ---- Bomb on a conveyor: it slides along the belt. ------------------------

TEST_CASE("a bomb resting on a conveyor slides along the belt") {
    Simulation s(open_config());
    State& st = s.state();
    // East belt along row 0.
    for (int x = 0; x < kGridWidth; ++x) {
        st.actor_type[0][x] = ActorType::Conveyor;
        st.actor_dir[0][x] = kEast;
    }
    add_bomb(st, 2, 0);  // resting bomb at (2,0)

    const Fixed x0 = st.bombs[0].x;
    run(s, 20, TickInputs{});  // no input: only the belt moves the bomb

    REQUIRE_FALSE(st.bombs.empty());
    CHECK(st.bombs[0].x > x0);          // carried east by the belt
    CHECK(st.bombs[0].tile_y() == 0);   // stayed on the belt row
    CHECK(st.bombs[0].dir == Direction::Right);
}

TEST_CASE("a bomb warps through a warphole while sliding") {
    Simulation s(open_config());
    State& st = s.state();
    // Warphole pair on row 0: (5,0) -> (10,0).
    st.actor_type[0][5] = ActorType::Warphole;
    st.warp_dest_x[0][5] = 10;
    st.warp_dest_y[0][5] = 0;
    st.actor_type[0][10] = ActorType::Warphole;
    st.warp_dest_x[0][10] = 5;
    st.warp_dest_y[0][10] = 0;

    // Bomb at (3,0), sliding east; it should reach (5,0) and warp to (10,0).
    add_bomb(st, 3, 0, /*moving=*/true, Direction::Right);

    const std::uint32_t rng_before = st.rng;
    run(s, 12, TickInputs{});  // let it roll east into the warphole

    REQUIRE_FALSE(st.bombs.empty());
    CHECK(st.bombs[0].tile_x() >= 10);  // crossed (5,0) and jumped east
    CHECK(st.rng == rng_before);        // bomb warp draws no RNG
}
