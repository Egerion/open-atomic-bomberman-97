// Ice / input-lag (VALUELST ids 450-460, Hockey Rink = level index 2):
// a fixed input-response delay applied to HUMAN players' desired movement
// direction, faithful to sub_41F29B's per-player history ring buffer
// (~23058-23078). AI players are exempt (gated on the player-type byte
// +16 != 1 in the original). See docs/re/facts.md "Ice / input-lag".

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "helpers.hpp"

using namespace bomber::sim;
using namespace bomber::sim::test;

namespace {

MatchConfig hockey_config() {
    MatchConfig cfg = open_config();
    for (auto& row : cfg.cells) row.fill(Cell::Blank);  // open runway, no pillars
    cfg.tuning.level_index = 2;                         // Hockey Rink
    return cfg;
}

}  // namespace

TEST_CASE(
    "VALUELST defaults: only Hockey Rink (2) has ice delay, only Haunted House (7) has regen") {
    Tuning t;
    for (int i = 0; i < 11; ++i) {
        CHECK(t.ice_delay_ms[i] == (i == 2 ? 250 : 0));
        CHECK(t.regen_seconds[i] == (i == 7 ? 4 : 0));
    }
    CHECK(t.regen_clear_radius == 4);
    CHECK(t.level_index == 0);
}

TEST_CASE("ice delay is inert off Hockey Rink: movement matches the plain speed-budget rule") {
    Simulation s(open_config());  // level_index defaults to 0 ("new traditionalist")
    Player& p = s.state().players[0];
    TickInputs right;
    right.players[0].right = true;
    const int x0 = p.x;
    run(s, 10, right);
    int moved = (p.x - x0) / 100;
    // Canonical sub-frame accrual (constants.hpp kSubFrames/frame_budget) —
    // identical to test_move.cpp's baseline.
    long budget = 0, expected = 0;
    for (int t = 0; t < 10; ++t) {
        for (int f = 0; f < kSubFrames; ++f) {
            budget += frame_budget(s.state().tuning.start_speed, kSubFrameMs[f]);
            while (budget > 0) {
                budget -= 100;
                ++expected;
            }
        }
    }
    CHECK(moved == static_cast<int>(expected));
    for (auto v : p.ice_history) CHECK(v == -1);  // buffer never written off-level
}

TEST_CASE("Hockey Rink delays a human's first step by the 250ms lag, capped by the history") {
    Simulation s(hockey_config());
    Player& p = s.state().players[0];
    TickInputs right;
    right.players[0].right = true;
    const int x0 = p.x;

    // The buffer is pushed once per canonical SUB-FRAME (the original pushes
    // once per displayed frame); it starts all "-1" at setup, so the delayed
    // slot only reads a REAL sample once `lag + 1` pushes have happened. The
    // lag slot is ceil(250 ms in canonical frames) CLAMPED to the 30-slot
    // buffer — the original's own behaviour when the frame rate outruns its
    // fixed history (at ~180 fps its 30 slots span only ~166 ms), see
    // MovementSystem::ice_delay.
    int lag = (250 * kSubFrames + kMsPerTick - 1) / kMsPerTick;
    if (lag >= Player::kIceHistoryLen) lag = Player::kIceHistoryLen - 1;
    const int frozen_ticks = lag / kSubFrames;  // full ticks the slot still reads -1
    // Without this the "the delay really happened" loop can empty (lag < one
    // tick's worth of sub-frames), leaving only `p.x > x0` below — which an
    // UNDELAYED walker satisfies just as well.
    REQUIRE(frozen_ticks >= 1);
    for (int t = 1; t <= frozen_ticks; ++t) {
        s.tick(right);
        CHECK(p.x == x0);
    }
    s.tick(right);  // the delayed sample turns "Right" partway through this tick
    CHECK(p.x > x0);
}

TEST_CASE("Hockey Rink still lets a player through, just later — same total distance, shifted") {
    // Compare a Hockey Rink run to a plain (level_index 0) run of the SAME
    // length: the ice-delayed run should be roughly a right-shift of the
    // undelayed one once movement starts (same speed budget, later onset).
    Simulation plain(open_config());
    Simulation ice(hockey_config());
    TickInputs right;
    right.players[0].right = true;
    const int x0 = ice.state().players[0].x;
    run(plain, 60, right);
    run(ice, 60, right);
    CHECK(ice.state().players[0].x < plain.state().players[0].x);  // strictly behind
    CHECK(ice.state().players[0].x > x0);                          // but still made progress
}

TEST_CASE("AI players are exempt from ice delay: their history buffer is never written") {
    MatchConfig cfg = hockey_config();
    cfg.player_count = 2;
    cfg.spawns = {{0, 0}, {14, 10}};
    cfg.ai[1] = true;  // player 1 is computer-controlled
    Simulation s(cfg);
    TickInputs in;
    in.players[0].right = true;  // human: should start buffering
    for (int t = 0; t < 40; ++t) s.tick(in);

    bool human_wrote_history = false;
    for (auto v : s.state().players[0].ice_history)
        if (v != -1) human_wrote_history = true;
    CHECK(human_wrote_history);

    for (auto v : s.state().players[1].ice_history)
        CHECK(v == -1);  // AI: MovementSystem::ice_delay returns before touching it
}

TEST_CASE("the ice-history ring buffer is part of the hashed state") {
    Simulation a(open_config());
    Simulation b(open_config());
    b.state().players[0].ice_history[0] = 1;  // white-box: differ by one buffered sample
    CHECK(a.hash() != b.hash());
}
