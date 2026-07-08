// Focused checks for the reverse-engineered player stepper (sub_41EC84).
// See docs/re/facts.md "Player movement / collision stepper — CONFIRMED".

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "helpers.hpp"

using namespace bomber::sim;
using namespace bomber::sim::test;

TEST_CASE("speed follows the carried-budget rule") {
    // start_speed added per tick, 100 spent per pixel, remainder carried
    // across ticks (~ start_speed/100 px per tick).
    Simulation s(open_config());
    Player& p = s.state().players[0];
    TickInputs right;
    right.players[0].right = true;
    int x0 = p.x;
    run(s, 10, right);
    int moved = (p.x - x0) / 100;
    long budget = 0, expected = 0;
    for (int t = 0; t < 10; ++t) {
        budget += s.state().tuning.start_speed;
        while (budget > 0) {
            budget -= 100;
            ++expected;
        }
    }
    CHECK(moved == static_cast<int>(expected));
    CHECK(p.tile_y() == 0);  // stayed on the lane
}

TEST_CASE("an off-centre player glides back onto the lane centreline") {
    Simulation s(open_config());
    Player& p = s.state().players[0];
    p.y = kTileHF / 2 + 500;  // 5px below the row-0 centre
    TickInputs right;
    right.players[0].right = true;
    run(s, 8, right);
    CHECK(p.y == kTileHF / 2);  // snapped exactly to centre
}

TEST_CASE("corner assist has no distance threshold") {
    // Even a 4px lean rounds the corner (the old guessed 9px gate would have
    // ignored this).
    Simulation s(open_config());
    Player& p = s.state().players[0];
    p.x = kTileWF / 2;                  // column 0 centre
    p.y = kTileHF + kTileHF / 2 - 400;  // row 1 centre, 4px up
    TickInputs right;
    right.players[0].right = true;
    run(s, 20, right);
    CHECK(p.tile_y() == 0);  // rounded up into the open row 0
    CHECK(p.tile_x() >= 1);  // and carried on east
}

TEST_CASE("a centred player walking into a wall stops dead on the centre") {
    Simulation s(open_config());
    Player& p = s.state().players[0];
    p.x = kTileWF / 2;
    p.y = kTileHF + kTileHF / 2;  // (0,1) dead centre; pillar at (1,1)
    TickInputs right;
    right.players[0].right = true;
    run(s, 5, right);
    CHECK(p.tile_x() == 0);
    CHECK(p.x == kTileWF / 2);  // clamped exactly at the tile centre
    CHECK(p.tile_y() == 1);     // did not drift off the row
}

// Goldman wheel clogs (docs/re/goldman-roulette.md §9.1): a speed PENALTY,
// the skate bonus's mirror image, folded into Player::speed at setup — so it
// reduces per-tick displacement exactly like a lower start_speed would, with
// no separate movement-code path (movement.cpp's `eff = p.speed` already
// covers whatever speed carries in).
TEST_CASE("clogs reduces per-tick walking distance, mirroring the skate speed bonus") {
    MatchConfig cfg = open_config();
    cfg.born_with_clogs[0] = 1;
    Simulation s(cfg);
    Player& p = s.state().players[0];
    REQUIRE(p.speed == cfg.tuning.start_speed - cfg.tuning.clogs_speed_penalty);
    TickInputs right;
    right.players[0].right = true;
    int x0 = p.x;
    run(s, 10, right);
    int moved = (p.x - x0) / 100;

    long budget = 0, expected = 0;
    for (int t = 0; t < 10; ++t) {
        budget += p.speed;  // the clogs-reduced speed
        while (budget > 0) {
            budget -= 100;
            ++expected;
        }
    }
    CHECK(moved == static_cast<int>(expected));
    // Strictly slower than a clean run at start_speed (same tick count).
    CHECK(moved < 10 * cfg.tuning.start_speed / 100 + 1);
}

TEST_CASE("clogs and molasses disease compose in the pinned order: clogs folds into speed, then disease scales") {
    // sub_41F29B (§9.1): the clogs subtraction happens in the SAME base term
    // skates/base-speed do, THEN disease scaling (molasses /3) applies to the
    // WHOLE total — so (start_speed - clogs*penalty) gets divided by 3, not
    // just start_speed.
    MatchConfig cfg = open_config();
    cfg.born_with_clogs[0] = 1;
    Simulation s(cfg);
    Player& p = s.state().players[0];
    int clogged_speed = p.speed;
    REQUIRE(clogged_speed == cfg.tuning.start_speed - cfg.tuning.clogs_speed_penalty);
    infect(p, Disease::Slow);
    TickInputs right;
    right.players[0].right = true;
    int x0 = p.x;
    run(s, 20, right);
    int moved = (p.x - x0) / 100;

    long budget = 0, expected = 0;
    for (int t = 0; t < 20; ++t) {
        budget += clogged_speed / 3;  // clogs folded in BEFORE the /3, matching sub_41F29B
        while (budget > 0) {
            budget -= 100;
            ++expected;
        }
    }
    CHECK(moved == static_cast<int>(expected));
}
