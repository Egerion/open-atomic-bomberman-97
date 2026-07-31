// match_cadence.hpp — the F9 cadence lever resolved once, for all four of its
// consumers. Pure logic, so it is pinned headlessly rather than by eyeballing a
// window, which is the whole point: nothing in the game NOTICES when one consumer
// of the lever disagrees with the others, and for a while three of them did.
//
// The bug: the "netplay must be fixed-tick" rule was written only at the sim
// advance. The animation clock, the entity glide and the interpolation alpha all
// read the raw F9 flag, so an online match with F9 on ran the sim correctly at a
// fixed 20 Hz while the renderer was told native cadence was active and pinned
// interp_alpha to 1.0 — switching inter-tick interpolation OFF. F9 online was
// therefore pure loss: it could not speed the sim up (the one guard prevented
// that, correctly), it only removed the smoothing that makes 20 Hz look fluid.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "bomber/game_util/match_cadence.hpp"

using bomber::game::match_cadence;
using bomber::game::MatchCadence;

namespace {
constexpr int kMsPerTick = 50;
constexpr std::uint64_t kTickNs = 50'000'000ull;
}  // namespace

TEST_CASE("cadence: netplay is fixed-tick 20 Hz on ALL FOUR consumers, F9 or not") {
    // THE REGRESSION. With a session driving the sim, every derived value must be
    // the 20 Hz one whatever the lever says — not just the sim advance.
    for (const bool lever : {false, true}) {
        const MatchCadence c = match_cadence(lever, /*netplay=*/true, /*systems_accum_ms=*/37,
                                             kMsPerTick, /*acc_ns=*/20'000'000ull, kTickNs);
        CHECK_FALSE(c.native);              // 1. the sim advance (already guarded)
        CHECK(c.entity_interp == 1.0f);     // 2. the entity glide
        CHECK(c.interp_alpha == doctest::Approx(0.4f));  // 3. interpolation ALIVE, not pinned to 1
    }
    // And the lever genuinely makes no difference online: identical output.
    const MatchCadence off = match_cadence(false, true, 37, kMsPerTick, 20'000'000ull, kTickNs);
    const MatchCadence on = match_cadence(true, true, 37, kMsPerTick, 20'000'000ull, kTickNs);
    CHECK(off.native == on.native);
    CHECK(off.entity_interp == on.entity_interp);
    CHECK(off.interp_alpha == on.interp_alpha);
}

TEST_CASE("cadence: a LOCAL match with F9 on is untouched") {
    // The owner's reference for correct feel, validated 1:1 against the original
    // (ADR-0007). Native cadence renders the sim's live state directly and glides
    // the 50 ms-stepped entities off the sim's own systems accumulator.
    const MatchCadence c = match_cadence(/*lever=*/true, /*netplay=*/false,
                                         /*systems_accum_ms=*/25, kMsPerTick,
                                         /*acc_ns=*/0, kTickNs);
    CHECK(c.native);
    CHECK(c.entity_interp == doctest::Approx(0.5f));  // 25 of 50 ms into the tick
    CHECK(c.interp_alpha == 1.0f);                    // nothing to blend: the sim just ran
}

TEST_CASE("cadence: a LOCAL match with F9 off is the fixed-tick path") {
    const MatchCadence c = match_cadence(false, false, /*systems_accum_ms=*/25, kMsPerTick,
                                         /*acc_ns=*/12'500'000ull, kTickNs);
    CHECK_FALSE(c.native);
    CHECK(c.entity_interp == 1.0f);
    CHECK(c.interp_alpha == doctest::Approx(0.25f));
}

TEST_CASE("cadence: the accumulator left over by a netplay stall cannot extrapolate") {
    // A stall (or a re-phase hold) leaves the accumulator at or past a whole tick
    // with the sim frozen. Un-clamped that pushes alpha above 1 and runs entities
    // FORWARD through the pause, which reads as a rubber-band.
    const MatchCadence c = match_cadence(false, true, 0, kMsPerTick, /*acc_ns=*/3 * kTickNs,
                                         kTickNs);
    CHECK(c.interp_alpha == 1.0f);
    CHECK(c.interp_alpha <= 1.0f);
}

TEST_CASE("cadence: degenerate divisors do not produce a NaN alpha") {
    // Both denominators come from callers, so neither may be assumed non-zero.
    CHECK(match_cadence(false, false, 10, kMsPerTick, 0, 0).interp_alpha == 0.0f);
    CHECK(match_cadence(true, false, 10, /*ms_per_tick=*/0, 0, kTickNs).entity_interp == 1.0f);
}
