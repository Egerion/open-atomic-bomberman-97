// Disease behaviour tests — the nine skull diseases reverse-engineered from
// BM95.EXE. See docs/re/facts.md "Disease system".

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "helpers.hpp"

using namespace bomber::sim;
using namespace bomber::sim::test;

TEST_CASE("molasses thirds the speed, hyper multiplies it") {
    TickInputs right;
    right.players[0].right = true;
    auto dist = [&](const Disease* d) {
        Simulation s(open_config());
        if (d) infect(s.state().players[0], *d);
        int x0 = s.state().players[0].x;
        run(s, 10, right);
        return (s.state().players[0].x - x0) / 100;
    };
    int base = dist(nullptr);
    Disease slow = Disease::Slow, fast = Disease::Fast;
    int sl = dist(&slow), fa = dist(&fast);
    CHECK(sl < base / 2);  // molasses ~1/3
    CHECK(fa > base);      // hyper faster
}

TEST_CASE("constipation blocks laying bombs") {
    Simulation s(open_config());
    infect(s.state().players[0], Disease::Constipation);
    s.tick(press1(0));
    CHECK(s.state().bombs.empty());
}

TEST_CASE("diarrhea lays bombs automatically") {
    Simulation s(open_config());
    infect(s.state().players[0], Disease::Diarrhea);
    run(s, 2);                         // no button pressed
    CHECK(!s.state().bombs.empty());   // a bomb is laid anyway
}

TEST_CASE("reversed controls flip the pressed direction") {
    Simulation s(open_config());
    infect(s.state().players[0], Disease::Reversed);
    int y0 = s.state().players[0].tile_y();
    TickInputs up;
    up.players[0].up = true;
    run(s, 12, up);                             // pressing UP...
    CHECK(s.state().players[0].tile_y() > y0);  // ...actually moves DOWN
}

TEST_CASE("short flame and short fuse apply at drop time") {
    // Normal bomb, one tick after placement (fuse already decremented once).
    Simulation a(open_config());
    a.tick(press1(0));
    int normal_fuse = a.state().bombs[0].fuse;  // 40 -> 39

    Simulation s(open_config());
    s.state().players[0].flame = 5;
    infect(s.state().players[0], Disease::ShortFlame);
    infect(s.state().players[0], Disease::ShortFuse);
    s.tick(press1(0));
    REQUIRE(!s.state().bombs.empty());
    CHECK(s.state().bombs[0].flame == 1);  // short flame -> reach 1
    CHECK(s.state().bombs[0].fuse == s.state().tuning.fuse_frames / 3 - 1);
    CHECK(s.state().bombs[0].fuse < normal_fuse / 2);  // clearly a short fuse
}

TEST_CASE("diseases expire after their duration") {
    Simulation s(open_config());
    infect(s.state().players[0], Disease::Slow, 3);
    run(s, 3);
    CHECK(s.state().players[0].disease_timer == 0);
    CHECK(!s.state().players[0].sick(Disease::Slow));
}

TEST_CASE("contagion hands the whole set over on overlap") {
    Simulation s(open_config());
    infect(s.state().players[0], Disease::Fast, 300);
    s.state().players[1].x = s.state().players[0].x;  // stand on top of each other
    s.state().players[1].y = s.state().players[0].y;
    run(s, 1);
    CHECK(s.state().players[1].disease_timer > 0);   // caught it
    CHECK(s.state().players[1].sick(Disease::Fast));
    CHECK(s.state().players[0].sick(Disease::Fast));  // multiply=1: source keeps it
}

TEST_CASE("the skull can roll swap, exchanging positions") {
    // A skull token always emits an Infected event; find a seed whose roll is
    // Swap and confirm the two players exchange positions.
    bool swap_seen = false;
    for (std::uint32_t seed = 1; seed <= 300 && !swap_seen; ++seed) {
        MatchConfig cfg = open_config();
        cfg.seed = seed;
        Simulation s(cfg);
        int t0x = s.state().players[0].tile_x(), t0y = s.state().players[0].tile_y();
        int t1x = s.state().players[1].tile_x(), t1y = s.state().players[1].tile_y();
        s.state().floor[t0y][t0x] = PowerupType::Disease;  // p0 stands on a skull
        run(s, 1);
        for (auto& e : s.state().events)
            if (e.type == Event::Type::Infected && e.data == static_cast<int>(Disease::Swap))
                swap_seen = true;
        if (swap_seen) {
            CHECK(s.state().players[0].tile_x() == t1x);
            CHECK(s.state().players[0].tile_y() == t1y);
            CHECK(s.state().players[1].tile_x() == t0x);
            CHECK(s.state().players[1].tile_y() == t0y);
        }
    }
    CHECK(swap_seen);  // swap is reachable through the skull
}
