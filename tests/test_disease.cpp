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

TEST_CASE("swap exchanges position only, not move_budget") {
    // sub_41DFB6's swap is a 2-field XOR trick on the integer-pixel position
    // ONLY (+0x1c/+0x20 — our x/y). An earlier port also swapped move_budget,
    // which has no counterpart in the original (facts.md "Disease system").
    bool swap_seen = false;
    for (std::uint32_t seed = 1; seed <= 300 && !swap_seen; ++seed) {
        MatchConfig cfg = open_config();
        cfg.seed = seed;
        Simulation s(cfg);
        s.state().players[0].move_budget = 111;
        s.state().players[1].move_budget = 222;
        int t0x = s.state().players[0].tile_x(), t0y = s.state().players[0].tile_y();
        s.state().floor[t0y][t0x] = PowerupType::Disease;
        run(s, 1);  // no input pressed: move_on_actor never touches move_budget
        for (auto& e : s.state().events)
            if (e.type == Event::Type::Infected && e.data == static_cast<int>(Disease::Swap))
                swap_seen = true;
        if (swap_seen) {
            CHECK(s.state().players[0].move_budget == 111);
            CHECK(s.state().players[1].move_budget == 222);
        }
    }
    CHECK(swap_seen);
}

TEST_CASE("a stunned player's disease does not age") {
    // sub_41F29B nests freshness--/age+=delta/cure entirely inside "not
    // stunned" (`if (!+8)` ~22904) — a stunned player's disease timer is
    // frozen, exactly like the rest of their per-tick update.
    Simulation s(open_config());
    infect(s.state().players[0], Disease::Slow, 10);
    s.state().players[0].stun = 5;
    run(s, 3);
    CHECK(s.state().players[0].stun == 2);            // stun itself still ticks down
    CHECK(s.state().players[0].disease_timer == 10);  // but the disease does not age
    CHECK(s.state().players[0].sick(Disease::Slow));
}

TEST_CASE("a stunned player can neither spread nor catch a disease") {
    // Both ends of sub_41F29B's contagion scan require "not stunned": the
    // source gate (same `if (!+8)` nesting as the age test above) and the
    // target validity check's own `!v103[2]`.
    Simulation source_stunned(open_config());
    infect(source_stunned.state().players[0], Disease::Fast, 300);
    source_stunned.state().players[0].stun = 5;
    source_stunned.state().players[1].x = source_stunned.state().players[0].x;
    source_stunned.state().players[1].y = source_stunned.state().players[0].y;
    run(source_stunned, 1);
    CHECK(!source_stunned.state().players[1].sick(Disease::Fast));

    Simulation target_stunned(open_config());
    infect(target_stunned.state().players[0], Disease::Fast, 300);
    target_stunned.state().players[1].stun = 5;
    target_stunned.state().players[1].x = target_stunned.state().players[0].x;
    target_stunned.state().players[1].y = target_stunned.state().players[0].y;
    run(target_stunned, 1);
    CHECK(!target_stunned.state().players[1].sick(Disease::Fast));
}

TEST_CASE("a freshly-contagious disease is not aged again the same tick it spreads") {
    // Regression for the age-before-spread ordering fix: sub_41F29B ages and
    // cure-checks a player BEFORE that player's own contagion scan runs, so
    // a target infected this tick inherits the source's POST-age freshness —
    // its own per-tick age/freshness turn already passed this tick and does
    // not apply a second time on top of the freshly-copied value.
    Simulation s(open_config());
    infect(s.state().players[0], Disease::Fast, 300);
    s.state().players[1].x = s.state().players[0].x;
    s.state().players[1].y = s.state().players[0].y;
    run(s, 1);
    REQUIRE(s.state().players[1].sick(Disease::Fast));
    CHECK(s.state().players[1].disease_fresh == s.state().tuning.disease_freshness);
}

TEST_CASE("multiply=off infects only the first target and clears the source") {
    MatchConfig cfg = open_config();
    cfg.spawns = {{0, 0}, {2, 0}, {4, 0}};
    cfg.player_count = 3;
    Simulation s(cfg);
    s.state().tuning.diseases_multiply = false;
    infect(s.state().players[0], Disease::Fast, 300);
    // Bring players 1 and 2 both into contagion range of player 0.
    s.state().players[1].x = s.state().players[0].x;
    s.state().players[1].y = s.state().players[0].y;
    s.state().players[2].x = s.state().players[0].x;
    s.state().players[2].y = s.state().players[0].y;
    run(s, 1);
    CHECK(!s.state().players[0].sick(Disease::Fast));  // source lost it (multiply=0)
    CHECK(s.state().players[1].sick(Disease::Fast));   // first in slot order caught it
    CHECK(!s.state().players[2].sick(Disease::Fast));  // scan stopped after the first match
}
