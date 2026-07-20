// Stage-actor mechanics that go beyond the standalone conveyor/trampoline
// suites: dirarrows (type 0, bomb-only re-steer), warpholes (type 1,
// PLAYER-ONLY teleport — a bomb of any kind is blocked at the doorstep and
// never warps, sub_4230A5), and the bomb-on-conveyor slide. Faithful to
// sub_42331C (bomb mover) and sub_41EC84 (player warp step-on). See
// docs/re/stage-actors.md §5-6 and facts.md "Bomb/warphole reconciliation
// 2026-07-10".

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

// Bomb/warphole reconciliation 2026-07-10 (facts.md "Bomb/warphole
// reconciliation 2026-07-10", correcting stage-actors.md §6 item 4): bombs
// NEVER warp in the original. sub_4230A5 (the sliding-bomb cell-entry probe)
// treats ANY tile occupied by a type-1 (warphole) actor as impassable,
// regardless of the underlying cell type, and the warp resolver sub_405A81 is
// called from exactly one site in the whole binary — the player stepper
// (sub_41EC84). A sliding/kicked/conveyor-carried bomb is blocked at a
// warphole's doorstep exactly like a wall; it stops one tile short and never
// enters, warps, or teleports.

TEST_CASE("a sliding bomb is blocked at a warphole (never warps, sub_4230A5)") {
    Simulation s(open_config());
    State& st = s.state();
    // Warphole pair on row 0: (5,0) -> (10,0).
    st.actor_type[0][5] = ActorType::Warphole;
    st.warp_dest_x[0][5] = 10;
    st.warp_dest_y[0][5] = 0;
    st.actor_type[0][10] = ActorType::Warphole;
    st.warp_dest_x[0][10] = 5;
    st.warp_dest_y[0][10] = 0;

    // Bomb at (3,0), sliding east; it must stop at (4,0) — the doorstep of the
    // warphole at (5,0) — and never cross onto it.
    add_bomb(st, 3, 0, /*moving=*/true, Direction::Right);

    const std::uint32_t rng_before = st.rng;
    // Events are per-tick outputs (rebuilt every tick, never accumulated), so
    // watch for BombStopped tick-by-tick rather than only after the last tick.
    bool stopped = false;
    bool warped = false;
    for (int t = 0; t < 20; ++t) {
        s.tick(TickInputs{});
        for (const auto& e : s.state().events) {
            if (e.type == Event::Type::BombStopped) stopped = true;
            if (e.type == Event::Type::WarpUsed) warped = true;
        }
    }

    REQUIRE_FALSE(st.bombs.empty());
    CHECK(st.bombs[0].tile_x() == 4);   // stopped one tile short, never entered
    CHECK_FALSE(st.bombs[0].moving);    // halted, exactly like hitting a wall
    CHECK(st.rng == rng_before);        // no RNG either way
    CHECK(stopped);
    CHECK_FALSE(warped);                // WarpUsed never fires for a bomb
}

TEST_CASE("a jelly bomb bounces off a warphole instead of entering it") {
    Simulation s(open_config());
    State& st = s.state();
    st.actor_type[0][5] = ActorType::Warphole;
    st.warp_dest_x[0][5] = 10;
    st.warp_dest_y[0][5] = 0;

    Bomb& b = add_bomb(st, 3, 0, /*moving=*/true, Direction::Right);
    b.jelly = true;

    // Roll east into the warphole's doorstep and ping-pong. (A jelly bomb never
    // stops, so with the faster kicked speed — bombs F2 — it may reverse more
    // than once within the window; assert the invariant, not the final dir.)
    bool bounced = false;
    for (int t = 0; t < 10; ++t) {
        s.tick(TickInputs{});
        for (const auto& e : s.state().events)
            if (e.type == Event::Type::JellyBounced) bounced = true;
        CHECK(st.bombs[0].tile_x() <= 4);  // never crosses onto the warphole (5,0)
    }

    REQUIRE_FALSE(st.bombs.empty());
    CHECK(bounced);                   // reversed off the warphole, like a wall
    CHECK(st.bombs[0].moving);        // still ping-ponging — never stopped
    CHECK_FALSE(saw_warp(s));
}

TEST_CASE("a bomb resting on a belt is blocked by a warphole ahead") {
    Simulation s(open_config());
    State& st = s.state();
    // East belt along row 0, up to the warphole at (5,0).
    for (int x = 0; x < 5; ++x) {
        st.actor_type[0][x] = ActorType::Conveyor;
        st.actor_dir[0][x] = kEast;
    }
    st.actor_type[0][5] = ActorType::Warphole;
    st.warp_dest_x[0][5] = 10;
    st.warp_dest_y[0][5] = 0;

    add_bomb(st, 3, 0);  // resting on the belt, not yet moving

    run(s, 40, TickInputs{});  // belt keeps trying to push it every tick

    REQUIRE_FALSE(st.bombs.empty());
    CHECK(st.bombs[0].tile_x() == 4);  // pushed up to the doorstep, no further
    CHECK_FALSE(saw_warp(s));
}

// A flying bomb's landing check (sub_42331C ~25453: `!v62 || exp_ &&
// v62[1] != 1`) treats a warphole the same way it treats a wall/bomb/
// powerup: it cannot land there. `exp_` decompiles to a bare reference to
// the statically-linked, NEVER-CALLED CRT exp() routine — confirmed dead
// code by direct disassembly (the ONLY xref to it anywhere in the binary is
// a `dr_O` load of its address, immediately tested and always non-zero) —
// so the real condition is just "actor type != Warphole", the same rule
// already ported for the sliding-bomb probe above. facts.md "Chain-reaction
// timing" (exp_ resolution).
TEST_CASE("a flying bomb cannot land on a warphole; it hops onward instead") {
    Simulation s(open_config());
    State& st = s.state();
    st.actor_type[0][4] = ActorType::Warphole;
    st.warp_dest_x[0][4] = 10;
    st.warp_dest_y[0][4] = 0;

    // Airborne, about to land on the warphole tile (4,0) THIS tick.
    Bomb& b = add_bomb(st, 1, 0);
    b.flying = true;
    b.dir = Direction::Right;
    b.from_x = centre_x(1);
    b.from_y = centre_y(0);
    b.to_x = centre_x(4);
    b.to_y = centre_y(0);
    b.fly_total = 4;
    b.fly_ticks = 1;  // this tick's advance_bombs() call resolves the landing

    s.tick(TickInputs{});
    // Right after the bounce the bomb's logical position still reads tile 4
    // (fly()'s arrival code moves it to the landing tile's centre before
    // deciding whether to settle there — same as the original, pseudo.c
    // 25457-25458), but it must still be airborne: it did NOT settle.
    REQUIRE_FALSE(st.bombs.empty());
    CHECK(st.bombs[0].flying);
    bool warped = saw_warp(s);  // events are per-tick: accumulate across the run

    // Run to a full stop: it must settle somewhere other than the warphole.
    for (int i = 0; i < 40 && s.state().bombs[0].flying; ++i) {
        run(s, 1);
        if (saw_warp(s)) warped = true;
    }
    REQUIRE_FALSE(st.bombs.empty());
    CHECK_FALSE(st.bombs[0].flying);
    CHECK(st.bombs[0].tile_x() != 4);  // never actually settled on the warphole
    CHECK_FALSE(warped);               // WarpUsed never fires for a bomb

    // Contrast: the SAME setup over a plain open tile settles normally.
    Simulation s2(open_config());
    Bomb& b2 = add_bomb(s2.state(), 1, 0);
    b2.flying = true;
    b2.dir = Direction::Right;
    b2.from_x = centre_x(1);
    b2.from_y = centre_y(0);
    b2.to_x = centre_x(4);
    b2.to_y = centre_y(0);
    b2.fly_total = 4;
    b2.fly_ticks = 1;
    s2.tick(TickInputs{});
    REQUIRE_FALSE(s2.state().bombs.empty());
    CHECK_FALSE(s2.state().bombs[0].flying);  // settles fine with no actor in the way
    CHECK(s2.state().bombs[0].tile_x() == 4);
}

// ---- Core-feel audit 2026-07-10 (facts.md "Core-feel audit" §3) ------------

TEST_CASE("no bomb can be dropped while standing on a warphole (sub_41F29B ~23354)") {
    Simulation s(open_config());
    State& st = s.state();
    st.actor_type[0][2] = ActorType::Warphole;
    st.warp_dest_x[0][2] = 6;
    st.warp_dest_y[0][2] = 0;
    st.actor_type[0][6] = ActorType::Warphole;
    st.warp_dest_x[0][6] = 2;
    st.warp_dest_y[0][6] = 0;

    Player& p = st.players[0];
    p.x = centre_x(2);  // parked on the warp mouth...
    p.y = centre_y(0);
    p.warp_latch = true;  // ...latched (just arrived through it), so no re-warp

    s.tick(press1(0));
    CHECK(st.bombs.empty());  // the drop was refused
    bool refused = false;
    for (const auto& e : s.state().events)
        if (e.type == Event::Type::DropRefused) refused = true;
    CHECK(refused);  // ...audibly (SOUNDLST 40/41 via the SoundDirector)
}
