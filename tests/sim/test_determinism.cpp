// Determinism test (ADR-0003): two sims fed identical seeds and inputs must
// produce identical state hashes; a different seed must diverge.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "bomber/sim/rng.hpp"
#include "bomber/sim/simulation.hpp"

using namespace bomber::sim;

namespace {

TickInputs pattern(std::uint64_t t) {
    TickInputs in{};
    for (int p = 0; p < kMaxPlayers; ++p) {
        auto& pi = in.players[static_cast<std::size_t>(p)];
        pi.up = (t + static_cast<std::uint64_t>(p)) % 7 == 0;
        pi.down = (t + static_cast<std::uint64_t>(p)) % 11 == 1;
        pi.left = (t * 3 + static_cast<std::uint64_t>(p)) % 5 == 2;
        pi.right = (t * 5 + static_cast<std::uint64_t>(p)) % 9 == 3;
        pi.action1 = (t * 31 + static_cast<std::uint64_t>(p)) % 13 == 0;
        pi.action2 = (t * 17 + static_cast<std::uint64_t>(p)) % 23 == 0;
    }
    return in;
}

}  // namespace

TEST_CASE("identical seeds and inputs replay identically over 10k ticks") {
    constexpr std::uint64_t kTicks = 10000;

    Simulation a, b;
    a.state().rng = b.state().rng = 42u;
    for (std::uint64_t t = 0; t < kTicks; ++t) {
        TickInputs in = pattern(t);
        a.tick(in);
        b.tick(in);
        if (t % 3 == 0) {
            REQUIRE(next_random(a.state()) == next_random(b.state()));
        }
        if (t % 1000 == 0) REQUIRE(a.hash() == b.hash());
    }
    CHECK(a.hash() == b.hash());

    Simulation c;
    c.state().rng = 43u;
    for (std::uint64_t t = 0; t < kTicks; ++t) {
        c.tick(pattern(t));
        if (t % 3 == 0) (void)next_random(c.state());
    }
    CHECK(c.hash() != a.hash());  // different seeds must diverge
}
