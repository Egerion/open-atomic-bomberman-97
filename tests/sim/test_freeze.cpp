// Round-start input freeze (docs/re/facts.md "Round-start input freeze"):
// round init sub_4214BC arms dword_4621E0 = 50ms × getvalue(30) ≈ 1000 ms,
// the player-pass entry sub_420F07 counts it down, and sub_41F29B's
// acquisition gate (`v113 && !dword_4621E0`, pseudo.c 23028) skips BOTH the
// AI brain and the human input read while it runs — nobody moves or acts
// through the opening colour-shuffle second of every round.
//
// tests/helpers.hpp's open_config() DISARMS the freeze for every other
// suite; this file re-arms it and pins the window itself.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "helpers.hpp"

using namespace bomber::sim;
using namespace bomber::sim::test;

namespace {

MatchConfig frozen_config() {
    MatchConfig cfg = open_config();
    cfg.tuning.input_freeze_ticks = Tuning{}.input_freeze_ticks;  // re-arm the default (id 30 = 20)
    return cfg;
}

}  // namespace

TEST_CASE("VALUELST default: the freeze arms to getvalue(30) = 20 ticks (1000 ms)") {
    CHECK(Tuning{}.input_freeze_ticks == 20);
    Simulation s(frozen_config());
    CHECK(s.state().input_freeze == 20);
    // Raw test states never arm it (build_state is the only writer).
    Simulation raw;
    CHECK(raw.state().input_freeze == 0);
}

TEST_CASE("a held key does nothing for the full freeze window, then moves on the next tick") {
    Simulation s(frozen_config());
    Player& p = s.state().players[0];
    const Fixed x0 = p.x;
    TickInputs right;
    right.players[0].right = true;

    // The original's gate opens on the frame at t >= 1000 ms — exactly
    // input_freeze_ticks (20) fully-dead ticks (run_tick decrements AFTER
    // the player pass so the window is not cut one tick short).
    const int window = s.state().tuning.input_freeze_ticks;
    for (int t = 0; t < window; ++t) {
        s.tick(right);
        CHECK(p.x == x0);
    }
    CHECK(s.state().input_freeze == 0);
    s.tick(right);  // gate open: the held key finally acquires
    CHECK(p.x > x0);
}

TEST_CASE("bomb keys are dead during the freeze too (LABEL_246 sees no acquired input)") {
    Simulation s(frozen_config());
    Player& p = s.state().players[0];
    const int window = s.state().tuning.input_freeze_ticks;
    // Alternate release/press so a fresh edge is offered every other tick.
    for (int t = 0; t < window; ++t) s.tick(t % 2 == 0 ? press1(0) : TickInputs{});
    CHECK(p.bombs_placed == 0);
    CHECK(s.state().bombs.empty());
    // After the window a press edge drops normally.
    s.tick(TickInputs{});
    s.tick(press1(0));
    CHECK(p.bombs_placed == 1);
}

TEST_CASE("the AI brain draws no RNG while the freeze runs, and wakes with it") {
    MatchConfig cfg = frozen_config();
    cfg.ai[0] = true;
    Simulation s(cfg);
    const std::uint32_t rng0 = s.state().rng;
    // Whole window: the acquisition gate skips the brain outright — zero
    // draws on this bomb/disease-free board (same baseline as test_ai.cpp's
    // "golden inert" case).
    const int window = s.state().tuning.input_freeze_ticks;
    for (int t = 0; t < window; ++t) s.tick(TickInputs{});
    CHECK(s.state().rng == rng0);
    s.tick(TickInputs{});  // gate open: draws A/B fire
    CHECK(s.state().rng != rng0);
}

TEST_CASE("input_freeze is hashed state") {
    Simulation a(frozen_config());
    Simulation b(frozen_config());
    b.state().input_freeze = 3;  // white-box divergence
    CHECK(a.hash() != b.hash());
}
