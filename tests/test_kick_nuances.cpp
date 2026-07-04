// Kicked-bomb nuances — reverse-engineered from the bomb updater sub_42331C
// (kicked-slide block) cross-checked against sub_42708D (flame-at), sub_405654
// (stage-actor-at-tile) and sub_42464B (the kick handler). Audit findings (#8):
//   1. A bomb sliding onto a flaming tile EXPLODES (sub_42331C runs the
//      sub_42708D flame check each pixel-step) — FIXED here.
//   2. The mid-slide "re-steer" reads a DIRARROW/conveyor stage actor's godir
//      (sub_405654 scans the level-actor registry), NOT a resting player. We
//      do not model dirarrows yet, so there is nothing faithful to add; this
//      is deferred to ROADMAP #7 (conveyors/arrows).
//   3. Kicked-bomb speed is the fixed VALUELST id 300 (sub_42464B sets bomb
//      +112 = getvalue(300)); NOT a per-player base — already faithful.
// See docs/re/facts.md "Kick nuances".

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "helpers.hpp"

using namespace bomber::sim;
using namespace bomber::sim::test;

namespace {

// Drop a resting, sliding bomb at tile (tx,ty) heading dir. White-box: we set
// the motion directly to isolate the slide from the kick trigger (kick input
// is already covered in test_jelly).
void push_sliding_bomb(Simulation& s, int tx, int ty, Direction dir,
                       std::uint8_t owner = 0) {
    Bomb b;
    b.active = true;
    b.owner = owner;
    b.x = tx * kTileWF + kTileWF / 2;
    b.y = ty * kTileHF + kTileHF / 2;
    b.fuse = 10000;  // long fuse: we want to observe the slide, not a timeout
    b.flame = 2;
    b.moving = true;
    b.dir = dir;
    s.state().bombs.push_back(b);
}

}  // namespace

TEST_CASE("a bomb sliding into a flame tile explodes") {
    Simulation s(open_config());
    // Keep both spawned players out of the bomb's lane so they don't block it.
    s.state().players[0].alive = false;
    s.state().players[1].alive = false;

    push_sliding_bomb(s, 2, 0, Direction::Right);
    // A long-lived flame at (4,0) owned by player 1 (>0 so the slide's flame
    // check trips as the bomb enters it). Value 200 easily outlasts the couple
    // of ticks it takes the bomb to arrive (flames age 1/tick).
    s.state().flame[0][4] = 200;
    s.state().flame_owner[0][4] = 1;

    // Slide east; on entering (4,0) it must detonate (become inactive, then be
    // compacted out) rather than sail through the fire.
    bool gone = false;
    for (int i = 0; i < 30 && !gone; ++i) {
        s.tick(TickInputs{});
        gone = s.state().bombs.empty();
    }
    CHECK(gone);
    // The detonation left flame owned by the bomb's owner (0) on the field.
    bool flame_from_blast = false;
    for (int x = 0; x < kGridWidth && !flame_from_blast; ++x)
        if (s.state().flame_owner[0][x] == 0 && s.state().flame[0][x] > 0)
            flame_from_blast = true;
    CHECK(flame_from_blast);
}

TEST_CASE("a bomb sliding through open ground does NOT explode early") {
    // Control for the flame case: no flame in the lane -> the bomb just parks.
    Simulation s(open_config());
    s.state().players[0].alive = false;
    s.state().players[1].alive = false;
    s.state().cells[0][6] = Cell::Solid;  // a wall to stop it, no flame

    push_sliding_bomb(s, 2, 0, Direction::Right);
    run(s, 30);
    REQUIRE(s.state().bombs.size() == 1);
    CHECK(!s.state().bombs[0].moving);
    CHECK(s.state().bombs[0].tile_x() == 5);  // parked flush at the wall
}

TEST_CASE("a bomb kicked into a fresh explosion is caught by the flame") {
    // End-to-end: a real kick, then the bomb rolls into flame from another
    // bomb's blast and goes off, instead of sliding past it.
    Simulation s(open_config());
    s.state().players[1].alive = false;
    Player& p = s.state().players[0];
    p.kick = true;
    p.flame = 1;
    p.x = 2 * kTileWF + kTileWF / 2;  // stand at (2,0)
    p.y = kTileHF / 2;

    s.tick(press1(0));  // lay a bomb at (2,0)
    REQUIRE(s.state().bombs.size() == 1);

    TickInputs left;
    left.players[0].left = true;
    run(s, 12, left);   // step off to the west
    TickInputs right;
    right.players[0].right = true;
    run(s, 8, right);   // walk back east -> kick the bomb east
    REQUIRE(s.state().bombs[0].moving);

    // Put a long-lived flame directly in the sliding bomb's path; it must
    // detonate on entry instead of gliding through (value 200 outlasts travel).
    s.state().flame[0][6] = 200;
    s.state().flame_owner[0][6] = 1;
    bool gone = false;
    for (int i = 0; i < 40 && !gone; ++i) {
        s.tick(TickInputs{});
        gone = s.state().bombs.empty();
    }
    CHECK(gone);
}

TEST_CASE("kick nuances stay deterministic across replays") {
    MatchConfig cfg = open_config();
    Simulation a(cfg), b(cfg);
    a.state().players[0].kick = true;
    b.state().players[0].kick = true;
    TickInputs in;
    for (int t = 0; t < 200; ++t) {
        in.players[0].action1 = t % 29 == 0;
        in.players[0].right = (t / 4) % 2 == 0;
        in.players[0].down = (t / 9) % 2 == 0;
        a.tick(in);
        b.tick(in);
    }
    CHECK(a.hash() == b.hash());
}
