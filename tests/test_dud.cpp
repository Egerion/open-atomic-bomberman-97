// Dud bombs — reverse-engineered from bomb creation (sub_422EDE) and the
// bomb updater (sub_42331C): only REGULAR bombs can fizzle; a global gate
// (armed at match init, re-armed base + rand(spread) SECONDS ahead by
// sub_422C13 — VALUELST 320/321's own legend; ~3-6 minutes per opportunity)
// rate-limits the 1-in-getvalue(322) roll; a dud freezes its fuse for
// getvalue(323) ticks, then relights and resumes. See docs/re/facts.md
// "Dud bombs" (units corrected 2026-07-10).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "helpers.hpp"

using namespace bomber::sim;
using namespace bomber::sim::test;

namespace {

// Every regular bomb duds: gate always open, chance 1-in-1.
MatchConfig always_dud_config() {
    MatchConfig cfg = open_config();
    cfg.tuning.dud_gate_base = 0;
    cfg.tuning.dud_gate_rand = 1;
    cfg.tuning.dud_chance = 1;
    return cfg;
}

}  // namespace

TEST_CASE("a dud freezes its fuse, relights, and finally explodes") {
    Simulation s(always_dud_config());
    s.tick(press1(0));
    REQUIRE(s.state().bombs.size() == 1);
    const int dud0 = s.state().bombs[0].dud_left;
    CHECK(dud0 == s.state().tuning.dud_frames - 1);  // fizzling (one tick passed)
    const int frozen_fuse = s.state().bombs[0].fuse;
    CHECK(frozen_fuse == s.state().tuning.fuse_frames);  // fuse untouched so far

    // Well past the normal fuse: still there, still fizzling, fuse frozen.
    run(s, s.state().tuning.fuse_frames + 10);
    REQUIRE(s.state().bombs.size() == 1);
    CHECK(s.state().bombs[0].fuse == frozen_fuse);
    CHECK(s.state().bombs[0].dud_left > 0);

    // Run the fizzle down to its tail, then let it relight: the fuse resumes
    // where it froze (total lifetime = dud_frames + fuse_frames).
    run(s, s.state().bombs[0].dud_left - 5);  // 5 fizzle ticks left
    REQUIRE(s.state().bombs.size() == 1);
    run(s, 10);  // fizzle ends mid-way; fuse burns the last 5 of these
    REQUIRE(s.state().bombs.size() == 1);
    CHECK(s.state().bombs[0].dud_left == 0);
    CHECK(s.state().bombs[0].fuse < frozen_fuse);
    run(s, s.state().bombs[0].fuse + 1);
    CHECK(s.state().bombs.empty());  // finally went off
}

TEST_CASE("trigger and jelly bombs never fizzle") {
    MatchConfig cfg = always_dud_config();
    Simulation s(cfg);
    Player& p = s.state().players[0];
    p.jelly = true;
    s.tick(press1(0));
    REQUIRE(!s.state().bombs.empty());
    CHECK(s.state().bombs[0].dud_left == 0);  // jelly kind: no dud

    Simulation s2(cfg);
    s2.state().players[0].trigger = true;
    s2.tick(press1(0));
    REQUIRE(!s2.state().bombs.empty());
    CHECK(s2.state().bombs[0].dud_left == 0);  // trigger kind: no dud
}

TEST_CASE("the gate rate-limits dud rolls") {
    // Setup arms the gate open (base 0); pushing the base out AFTER setup
    // makes the first placement's re-arm shut it for the second bomb.
    Simulation s(always_dud_config());
    s.state().tuning.dud_gate_base = 1000;
    Player& p = s.state().players[0];
    p.max_bombs = 2;
    s.tick(press1(0));  // bomb #1: gate open (armed at 0+rand%1=0) -> dud
    TickInputs down;
    down.players[0].down = true;
    run(s, 10, down);   // step to a fresh tile
    s.tick(press1(0));  // bomb #2: gate now shut for ~1000 ticks -> no dud
    REQUIRE(s.state().bombs.size() == 2);
    CHECK(s.state().bombs[0].dud_left > 0);
    CHECK(s.state().bombs[1].dud_left == 0);
}

TEST_CASE("a chain explosion sets off a fizzling dud") {
    Simulation s(always_dud_config());
    s.state().tuning.dud_gate_base = 1000;  // only the first bomb duds
    Player& p = s.state().players[0];
    p.max_bombs = 2;
    p.flame = 2;
    s.tick(press1(0));  // dud at (0,0)
    TickInputs down;
    down.players[0].down = true;
    run(s, 8, down);    // to (0,2), within flame reach of (0,0)
    s.tick(press1(0));  // live bomb at (0,2)
    REQUIRE(s.state().bombs.size() == 2);
    CHECK(s.state().bombs[0].dud_left > 0);
    // Let the live bomb explode; its flame QUEUES the dud, which forcibly
    // detonates the NEXT tick (docs/re/facts.md "Chain-reaction timing",
    // sub_423209's deferred queue — not the same tick). The dud state is
    // irrelevant to the queue: it does not gate the forced detonation.
    run(s, s.state().tuning.fuse_frames + 2);
    CHECK(s.state().bombs.empty());
}

// ---- Core-feel audit 2026-07-10 (facts.md "Dud bombs" units correction) ----

TEST_CASE("the dud gate is armed in SECONDS, 3-6 minutes out (VALUELST 320/321 legend)") {
    // Default tuning: base 180 s + rand % 180 s, converted to ticks at arm
    // time. The old port misread these as ticks (9-18 s), making duds ~20x
    // too frequent.
    Simulation s(open_config());
    CHECK(s.state().dud_gate >= 180ull * kTicksPerSecond);
    CHECK(s.state().dud_gate < 360ull * kTicksPerSecond);
}

TEST_CASE("the re-arm ACCUMULATES on the previous deadline (sub_422C13 `+=`)") {
    // Gate armed at 0 (base 0, spread 1): the first placement's re-arm adds
    // (base + rand % 1) * 20 ticks to the OLD gate value, not to "now".
    Simulation s(always_dud_config());
    s.state().tuning.dud_gate_base = 100;             // re-arm adds exactly 100 s
    const std::uint64_t before = s.state().dud_gate;  // 0 (armed with base 0)
    s.tick(press1(0));                                // placement -> re-arm
    CHECK(s.state().dud_gate == before + 100ull * kTicksPerSecond);
}

TEST_CASE("dud rolls are deterministic") {
    MatchConfig cfg = open_config();  // default gate/chance
    Simulation a(cfg), b(cfg);
    TickInputs in;
    for (int t = 0; t < 400; ++t) {
        in.players[0].action1 = t % 37 == 0;
        in.players[0].right = (t / 7) % 2 == 0;
        a.tick(in);
        b.tick(in);
    }
    CHECK(a.hash() == b.hash());
}
