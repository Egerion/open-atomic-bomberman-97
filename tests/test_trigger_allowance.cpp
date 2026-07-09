// Trigger-bomb allowance — reverse-engineered from bomb placement (sub_41EB13)
// and the Trigger pickup path (sub_41E21E case 9). A trigger player may only
// lay trigger bombs while a live-trigger counter (player byte +85) is below
// max_bombs (+86); each trigger placement increments it (++85), and it is
// refilled to 0 ONLY by a Trigger pickup — never decremented on detonation.
// Once exhausted, placement is NOT blocked: the bomb downgrades to a normal
// timed bomb. See docs/re/facts.md "Trigger allowance".

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "helpers.hpp"

using namespace bomber::sim;
using namespace bomber::sim::test;

namespace {

// Step the player one tile south (away from a bomb just dropped) so a
// detonation cannot catch them, then detonate their resting trigger bomb, then
// keep walking to reach a fresh empty tile for the next drop.
void clear_and_detonate(Simulation& s) {
    TickInputs down;
    down.players[0].down = true;
    run(s, 10, down);           // walk well clear of the bomb's blast (flame=1)
    s.tick(press2(0));          // action2 -> detonate_triggered (player clear)
    run(s, 8, down);            // walk on to the next empty tile
}

}  // namespace

TEST_CASE("a trigger pickup grants exactly max_bombs trigger placements") {
    Simulation s(open_config());
    Player& p = s.state().players[0];
    p.trigger = true;
    p.flame = 1;           // small blast so the player survives its own bomb
    p.max_bombs = 2;
    p.trigger_placed = 0;  // fresh allowance (as a Trigger pickup leaves it)

    s.tick(press1(0));  // placement #1 -> trigger bomb, allowance now 1
    REQUIRE(s.state().bombs.size() == 1);
    CHECK(s.state().bombs[0].trigger);
    CHECK(s.state().bombs[0].fuse < 0);          // waits for the trigger
    CHECK(s.state().players[0].trigger_placed == 1);

    clear_and_detonate(s);                        // free the slot, move on
    REQUIRE(s.state().players[0].alive);
    CHECK(s.state().bombs.empty());

    s.tick(press1(0));  // placement #2 -> still within allowance (1 < 2)
    REQUIRE(s.state().bombs.size() == 1);
    CHECK(s.state().bombs[0].trigger);
    CHECK(s.state().players[0].trigger_placed == 2);
}

TEST_CASE("exhausted trigger allowance downgrades to a normal timed bomb") {
    Simulation s(open_config());
    Player& p = s.state().players[0];
    p.trigger = true;
    p.flame = 1;
    p.max_bombs = 1;       // one trigger placement per pickup
    p.trigger_placed = 0;

    s.tick(press1(0));     // placement #1 -> trigger, allowance now 1 == max
    REQUIRE(s.state().bombs.size() == 1);
    CHECK(s.state().bombs[0].trigger);
    CHECK(s.state().players[0].trigger_placed == 1);

    clear_and_detonate(s);
    REQUIRE(s.state().players[0].alive);
    REQUIRE(s.state().bombs.empty());

    // Allowance spent (1 >= max_bombs 1): the flag is still set but this bomb
    // is a NORMAL timed bomb, and placement is NOT blocked.
    s.tick(press1(0));
    REQUIRE(s.state().bombs.size() == 1);
    CHECK(!s.state().bombs[0].trigger);          // downgraded
    // Placement and fuse ticking both run within the SAME tick (simulation.cpp
    // step 1 then step 3), so a bomb placed this tick has already had its fuse
    // decremented once by the time we observe it here — same convention as
    // test_sim.cpp's "bomb explodes at its fuse" (run(..., fuse_frames - 1)).
    CHECK(s.state().bombs[0].fuse == s.state().tuning.fuse_frames - 1);
    CHECK(s.state().players[0].trigger_placed == 1);  // not incremented further
}

TEST_CASE("picking up Trigger refills the allowance to a fresh max_bombs") {
    Simulation s(open_config());
    Player& p = s.state().players[0];
    p.trigger = true;
    p.flame = 1;
    p.max_bombs = 1;
    p.trigger_placed = 5;  // pretend the budget was spent long ago

    // Drop a Trigger token on the player's tile and let the pickup fire.
    s.state().floor[p.tile_y()][p.tile_x()] = PowerupType::Trigger;
    s.tick(TickInputs{});
    CHECK(s.state().players[0].trigger_placed == 0);  // reset by the pickup
    CHECK(s.state().players[0].trigger);

    s.tick(press1(0));  // now a trigger bomb again
    REQUIRE(s.state().bombs.size() == 1);
    CHECK(s.state().bombs[0].trigger);
    CHECK(s.state().players[0].trigger_placed == 1);
}

TEST_CASE("a non-trigger player never consumes trigger allowance") {
    Simulation s(open_config());
    Player& p = s.state().players[0];
    p.trigger = false;
    p.trigger_placed = 0;
    s.tick(press1(0));
    REQUIRE(s.state().bombs.size() == 1);
    CHECK(!s.state().bombs[0].trigger);
    CHECK(s.state().players[0].trigger_placed == 0);
}

// ---- Core-feel audit 2026-07-10 (facts.md "Core-feel audit" §2) ------------

TEST_CASE("a mutually-exclusive pickup SCATTERS the evicted token (sub_41E16A)") {
    Simulation s(open_config());
    Player& p = s.state().players[0];
    p.punch = true;
    s.state().floor[0][1] = PowerupType::Trigger;  // one tile east

    TickInputs right;
    right.players[0].right = true;
    run(s, 8, right);  // walk onto the token

    REQUIRE(p.trigger);
    CHECK(!p.punch);  // evicted...
    int punch_tokens = 0;
    for (int y = 0; y < kGridHeight; ++y)
        for (int x = 0; x < kGridWidth; ++x)
            if (s.state().floor[y][x] == PowerupType::Punch) ++punch_tokens;
    CHECK(punch_tokens == 1);  // ...and returned to the board, not destroyed
}

TEST_CASE("evicting Trigger downgrades the live trigger bombs to fresh timed bombs (sub_424C47)") {
    Simulation s(open_config());
    Player& p = s.state().players[0];
    p.trigger = true;
    p.flame = 1;
    s.state().floor[0][2] = PowerupType::Jelly;  // two tiles east

    s.tick(press1(0));  // trigger bomb at (0,0)
    REQUIRE(s.state().bombs.size() == 1);
    REQUIRE(s.state().bombs[0].trigger);
    REQUIRE(s.state().bombs[0].fuse < 0);

    TickInputs right;
    right.players[0].right = true;
    int t = 0;
    for (; t < 20 && !p.jelly; ++t) s.tick(right);  // walk onto the Jelly token
    REQUIRE(p.jelly);
    CHECK(!p.trigger);
    const Bomb& b = s.state().bombs[0];
    CHECK(!b.trigger);                                   // downgraded in place
    CHECK(b.fuse > 0);                                   // ...and relit
    CHECK(b.fuse >= s.state().tuning.fuse_frames - 2);   // from SCRATCH (elapsed = 0)
    int trigger_tokens = 0;
    for (int y = 0; y < kGridHeight; ++y)
        for (int x = 0; x < kGridWidth; ++x)
            if (s.state().floor[y][x] == PowerupType::Trigger) ++trigger_tokens;
    CHECK(trigger_tokens == 1);  // the evicted Trigger scattered back
}

TEST_CASE("a SLIDING trigger bomb can be detonated remotely (sub_424B41)") {
    // The detonate scan only exempts carried (3) and flying (2) bombs.
    Simulation s(open_config());
    Player& p = s.state().players[0];
    p.trigger = true;
    p.flame = 1;
    Bomb b;
    b.active = true;
    b.owner = 0;
    b.trigger = true;
    b.fuse = -1;
    b.flame = 1;
    b.x = 4 * kTileWF + kTileWF / 2;
    b.y = kTileHF / 2;
    b.moving = true;  // mid-slide
    b.dir = Direction::Right;
    s.state().bombs.push_back(b);

    s.tick(press2(0));
    CHECK(s.state().bombs.empty());  // went off despite sliding
    bool flame_seen = false;
    for (int x = 0; x < kGridWidth && !flame_seen; ++x)
        if (s.state().flame[0][x] > 0) flame_seen = true;
    CHECK(flame_seen);
}

TEST_CASE("trigger allowance stays deterministic across replays") {
    MatchConfig cfg = open_config();
    Simulation a(cfg), b(cfg);
    a.state().players[0].trigger = true;
    a.state().players[0].max_bombs = 3;
    b.state().players[0].trigger = true;
    b.state().players[0].max_bombs = 3;
    TickInputs in;
    for (int t = 0; t < 200; ++t) {
        in.players[0].action1 = t % 23 == 0;
        in.players[0].action2 = t % 31 == 0;
        in.players[0].down = (t / 5) % 2 == 0;
        a.tick(in);
        b.tick(in);
    }
    CHECK(a.hash() == b.hash());
}
