// Jelly bombs — reverse-engineered from the bomb updater sub_42331C.
// Kicked jelly reverses off obstacles and keeps rolling (sound 135); flying
// jelly rolls a 1-in-N ±90° veer at each landing boundary (VALUELST 667).
// Bomb kind is exclusive at creation: trigger overrides jelly (sub_41EB13).
// See docs/re/facts.md "Bomb machine".

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "helpers.hpp"

using namespace bomber::sim;
using namespace bomber::sim::test;

namespace {

int count_events(const State& s, Event::Type t) {
    int n = 0;
    for (const auto& e : s.events)
        if (e.type == t) ++n;
    return n;
}

}  // namespace

TEST_CASE("a kicked jelly bomb reverses at a wall and keeps sliding") {
    Simulation s(open_config());
    Player& p = s.state().players[0];
    p.kick = true;
    p.jelly = true;
    p.x = 2 * kTileWF + kTileWF / 2;  // stand at (2,0)
    p.y = kTileHF / 2;
    s.state().cells[0][5] = Cell::Solid;  // wall at (5,0)
    s.tick(press1(0));                    // lay a jelly bomb at (2,0)
    REQUIRE(s.state().bombs.size() == 1);
    CHECK(s.state().bombs[0].jelly);

    TickInputs left;
    left.players[0].left = true;
    run(s, 12, left);  // step off the bomb
    TickInputs right;
    right.players[0].right = true;
    run(s, 8, right);  // walk back against it -> kick east
    REQUIRE(s.state().bombs[0].moving);

    // Slides to the wall at (5,0), reverses there instead of stopping.
    bool bounced = false;
    for (int i = 0; i < 20 && !bounced; ++i) {
        s.tick(TickInputs{});
        bounced = count_events(s.state(), Event::Type::JellyBounced) > 0;
    }
    CHECK(bounced);
    CHECK(s.state().bombs[0].moving);                    // still rolling...
    CHECK(s.state().bombs[0].dir == Direction::Left);    // ...the other way
    int x_after_bounce = s.state().bombs[0].tile_x();
    run(s, 3);
    CHECK(s.state().bombs[0].tile_x() <= x_after_bounce);  // moving west now
}

TEST_CASE("a kicked regular bomb stops dead at the wall") {
    Simulation s(open_config());
    Player& p = s.state().players[0];
    p.kick = true;
    p.x = 2 * kTileWF + kTileWF / 2;
    p.y = kTileHF / 2;
    s.state().cells[0][5] = Cell::Solid;
    s.tick(press1(0));
    TickInputs left;
    left.players[0].left = true;
    run(s, 12, left);
    TickInputs right;
    right.players[0].right = true;
    run(s, 8, right);
    REQUIRE(s.state().bombs[0].moving);

    bool stopped = false;
    for (int i = 0; i < 20 && !stopped; ++i) {
        s.tick(TickInputs{});
        stopped = count_events(s.state(), Event::Type::BombStopped) > 0;
    }
    CHECK(stopped);
    CHECK(!s.state().bombs[0].moving);
    CHECK(s.state().bombs[0].tile_x() == 4);  // parked flush against the wall
    CHECK(s.state().bombs[0].tile_y() == 0);
}

TEST_CASE("trigger overrides jelly at placement — trigger bombs never bounce") {
    Simulation s(open_config());
    Player& p = s.state().players[0];
    p.trigger = true;
    p.jelly = true;
    s.tick(press1(0));
    REQUIRE(s.state().bombs.size() == 1);
    CHECK(s.state().bombs[0].trigger);
    CHECK(!s.state().bombs[0].jelly);   // kind is exclusive (sub_41EB13)
    CHECK(s.state().bombs[0].fuse < 0); // waits for the trigger
}

TEST_CASE("a flying jelly bomb veers sideways when it cannot land") {
    // jelly_turn_chance = 1 makes the veer roll always hit, so the hop after
    // a blocked landing goes perpendicular instead of straight on.
    MatchConfig cfg = open_config();
    cfg.tuning.jelly_turn_chance = 1;
    Simulation s(cfg);
    Player& p = s.state().players[0];
    p.punch = true;
    p.x = 0 * kTileWF + kTileWF / 2;  // (0,2), facing east
    p.y = 2 * kTileHF + kTileHF / 2;
    p.facing = Direction::Right;

    Bomb jelly;                        // jelly bomb resting ahead at (1,2)
    jelly.active = true;
    jelly.owner = 1;
    jelly.jelly = true;
    jelly.fuse = 10000;
    jelly.flame = 1;
    jelly.x = 1 * kTileWF + kTileWF / 2;
    jelly.y = 2 * kTileHF + kTileHF / 2;
    s.state().bombs.push_back(jelly);

    Bomb blocker = jelly;              // occupies the 3-tile landing at (4,2)
    blocker.jelly = false;
    blocker.x = 4 * kTileWF + kTileWF / 2;
    s.state().bombs.push_back(blocker);

    s.tick(press2(0));  // punch: jelly flies (1,2) -> (4,2), which is taken
    run(s, 40);
    const Bomb& b = s.state().bombs[0];
    CHECK(!b.flying);
    // Veered off the blocked landing: one tile up or down from (4,2), never
    // the straight-on (5,2).
    CHECK(b.tile_x() == 4);
    CHECK((b.tile_y() == 1 || b.tile_y() == 3));
}

TEST_CASE("a flying regular bomb hops straight over a blocked landing") {
    MatchConfig cfg = open_config();
    cfg.tuning.jelly_turn_chance = 1;  // must not affect non-jelly bombs
    Simulation s(cfg);
    Player& p = s.state().players[0];
    p.punch = true;
    p.x = 0 * kTileWF + kTileWF / 2;
    p.y = 2 * kTileHF + kTileHF / 2;
    p.facing = Direction::Right;

    Bomb reg;
    reg.active = true;
    reg.owner = 1;
    reg.fuse = 10000;
    reg.flame = 1;
    reg.x = 1 * kTileWF + kTileWF / 2;
    reg.y = 2 * kTileHF + kTileHF / 2;
    s.state().bombs.push_back(reg);

    Bomb blocker = reg;
    blocker.x = 4 * kTileWF + kTileWF / 2;
    s.state().bombs.push_back(blocker);

    s.tick(press2(0));
    run(s, 40);
    const Bomb& b = s.state().bombs[0];
    CHECK(!b.flying);
    CHECK(b.tile_x() == 5);  // straight-on hop east
    CHECK(b.tile_y() == 2);
}
