// Locks the Goldman Roulette wheel's pure math (goldman_wheel.hpp), mirrored
// from sub_4034BC @0x4034BC (docs/re/goldman-roulette.md §3): the 5-draw
// setup order, the boundary-crossing deceleration that always halts a mover
// exactly on a segment boundary, and the result-slot resolution. SDL-free —
// see that file's own doc comment for why it has no bearing on the
// determinism contract (ADR-0003): everything here is presentation
// randomness (WheelRng), never bomber::sim::State::rng.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "bomber/game/goldman_wheel.hpp"
#include "bomber/sim/types.hpp"

using namespace bomber::game;
using bomber::sim::PowerupType;

TEST_CASE("spin_setup draws exactly 5 values in the doc's fixed order") {
    // A hand-rolled deterministic sequence lets us hand-verify each field
    // against the doc's formulas (doc §3 "Setup — exactly 5 rand() draws").
    WheelRng rng{1};
    std::uint32_t r1 = rng.state * 1664525u + 1013904223u;  // == the 1st draw's raw state
    (void)r1;

    WheelRng rng2{1};
    WheelState w = spin_setup(rng2, wheel_circle_steps());

    // Independently replay the same 5 draws to cross-check every field.
    WheelRng check{1};
    int direction = 2 * static_cast<int>(check.next() % 2) - 1;
    int wheel_pos = static_cast<int>(check.next() % static_cast<std::uint32_t>(wheel_circle_steps()));
    int ring_pos = static_cast<int>(check.next() % static_cast<std::uint32_t>(wheel_circle_steps()));
    int wheel_budget = static_cast<int>(check.next() % 20) + 20;
    int ring_budget = wheel_budget + static_cast<int>(check.next() % 20);

    CHECK(w.direction == direction);
    CHECK(w.wheel.pos == wheel_pos);
    CHECK(w.ring.pos == ring_pos);
    CHECK(w.wheel.budget == wheel_budget);
    CHECK(w.ring.budget == ring_budget);
    CHECK(w.phase == WheelPhase::FreeSpin);
    CHECK(w.result == -1);
}

TEST_CASE("wheel budget is 20..39 and ring budget is always >= wheel budget") {
    // The doc pins both ranges: wheel budget rand%20+20 (20..39, doc §3 item
    // 4), ring budget = wheel's + rand%20 (doc item 5, so ring >= wheel by
    // construction, never the other way around).
    for (std::uint32_t seed = 1; seed < 200; ++seed) {
        WheelRng rng{seed};
        WheelState w = spin_setup(rng);
        CHECK(w.wheel.budget >= 20);
        CHECK(w.wheel.budget <= 39);
        CHECK(w.ring.budget >= w.wheel.budget);
    }
}

TEST_CASE("free-spin phase never consumes budget, only WindingDown does") {
    WheelRng rng{42};
    WheelState w = spin_setup(rng);
    int wheel_budget_before = w.wheel.budget;
    int ring_budget_before = w.ring.budget;
    for (int i = 0; i < 50; ++i) {
        step_mover(w.ring, w.phase, w.direction, wheel_circle_steps());
        step_mover(w.wheel, w.phase, -w.direction, wheel_circle_steps());
    }
    CHECK(w.wheel.budget == wheel_budget_before);
    CHECK(w.ring.budget == ring_budget_before);

    w.phase = WheelPhase::WindingDown;
    step_mover(w.ring, w.phase, w.direction, wheel_circle_steps());
    step_mover(w.wheel, w.phase, -w.direction, wheel_circle_steps());
    // At least one budget should now have moved (a boundary was crossed within
    // max(budget,6) steps almost always for a 70-step segment and a >=20
    // budget) OR stayed the same if no boundary happened to fall in this
    // particular frame — but it can never have INCREASED.
    CHECK(w.wheel.budget <= wheel_budget_before);
    CHECK(w.ring.budget <= ring_budget_before);
}

TEST_CASE("winding-down deceleration always halts a mover exactly on a segment boundary") {
    for (std::uint32_t seed = 1; seed < 100; ++seed) {
        WheelRng rng{seed};
        WheelState w = spin_setup(rng);
        w.phase = WheelPhase::WindingDown;
        // Run well past both budgets' worst case (39 + 19 boundary hits at
        // >=6 steps/frame each) so both movers are guaranteed to settle.
        for (int frame = 0; frame < 500 && (w.wheel.budget > 0 || w.ring.budget > 0); ++frame) {
            step_mover(w.ring, w.phase, w.direction, wheel_circle_steps());
            step_mover(w.wheel, w.phase, -w.direction, wheel_circle_steps());
        }
        CAPTURE(seed);
        CHECK(w.wheel.budget == 0);
        CHECK(w.ring.budget == 0);
        CHECK(w.wheel.pos % kWheelSegmentSteps == 0);
        CHECK(w.ring.pos % kWheelSegmentSteps == 0);
    }
}

TEST_CASE("resolve_prize maps the settled offset onto the doc's 6-slot table") {
    // offset = ring - wheel (+T if negative); prize = table[offset/70].
    WheelState w;
    w.wheel.pos = 0;
    for (int slot = 0; slot < kWheelSegments; ++slot) {
        w.ring.pos = slot * kWheelSegmentSteps;
        CHECK(resolve_prize(w) == kWheelPrizeIds[static_cast<std::size_t>(slot)]);
    }
    // Wrap case: ring < wheel.
    w.wheel.pos = 350;
    w.ring.pos = 0;  // offset = 0 - 350 + 420 = 70 -> slot 1
    CHECK(resolve_prize(w) == kWheelPrizeIds[1]);
}

TEST_CASE("wheel_prize_to_powerup maps 0/1/3/4/8 onto our PowerupType and 13 (clogs) onto None") {
    CHECK(wheel_prize_to_powerup(0) == PowerupType::ExtraBomb);
    CHECK(wheel_prize_to_powerup(1) == PowerupType::Flame);
    CHECK(wheel_prize_to_powerup(3) == PowerupType::Kick);
    CHECK(wheel_prize_to_powerup(4) == PowerupType::Skate);
    CHECK(wheel_prize_to_powerup(8) == PowerupType::Goldflame);
    // Clogs is PERMANENTLY not a sim::PowerupType (doc §8/§9.2) — its effect
    // is ported via MatchConfig::born_with_clogs instead (test_goldman_award.cpp).
    CHECK(wheel_prize_to_powerup(kClogsPrizeId) == PowerupType::None);
}

TEST_CASE("the full spin pipeline (setup -> wind-down -> resolve) always yields one of the 6 prizes") {
    for (std::uint32_t seed = 1; seed < 50; ++seed) {
        WheelRng rng{seed};
        WheelState w = spin_setup(rng);
        w.phase = WheelPhase::WindingDown;
        for (int frame = 0; frame < 500 && (w.wheel.budget > 0 || w.ring.budget > 0); ++frame) {
            step_mover(w.ring, w.phase, w.direction, wheel_circle_steps());
            step_mover(w.wheel, w.phase, -w.direction, wheel_circle_steps());
        }
        int prize = resolve_prize(w);
        bool found = false;
        for (int id : kWheelPrizeIds)
            if (id == prize) found = true;
        CAPTURE(seed);
        CHECK(found);
    }
}
