// Determinism test (ADR-0003): two sims fed identical seeds and inputs must
// produce identical state hashes; a different seed must diverge. The loopback
// suite (tests/net) proves the WIRE half of lockstep; this is the sim half —
// which is only a claim if the sims contain a match. This suite's first shape
// drove two EMPTY simulations (default ctor, no players): nothing behavioural
// could ever diverge, and the different-seed leg compared little beyond the
// hashed rng word with its own advanced value. The activity floors below are
// what refuse that shape now.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <utility>

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

// A REAL 4-player match: pillars, a deterministic brick scatter (so setup's
// powerup-hiding RNG runs and flames change the board), spawn corners kept
// clear so the canned inputs actually act. Bombs, explosions, brick burns,
// deaths — state a genuine divergence has room to live in.
Simulation make(std::uint32_t seed) {
    MatchConfig cfg;
    for (int y = 0; y < kGridHeight; ++y)
        for (int x = 0; x < kGridWidth; ++x) {
            if (x % 2 == 1 && y % 2 == 1)
                cfg.cells[y][x] = Cell::Solid;
            else
                cfg.cells[y][x] = ((x + y) % 3 == 0) ? Cell::Brick : Cell::Blank;
        }
    const int rx = kGridWidth - 1;
    const int by = kGridHeight - 1;
    for (auto [cx, cy] : {std::pair{0, 0}, std::pair{rx, 0}, std::pair{0, by}, std::pair{rx, by}})
        cfg.cells[cy][cx] = Cell::Blank;
    cfg.spawns = {{0, 0}, {rx, by}, {rx, 0}, {0, by}};
    cfg.player_count = 4;
    cfg.seed = seed;
    cfg.tuning.input_freeze_ticks = 0;  // act from tick 0
    // Baseline flame 3 — start_with REPLACES the inventory (facts.md, the -P
    // "born with" COUNT), and the stock 2 leaves a corner bomb one tile short
    // of the nearest (x+y)%3 brick. The activity floor below caught exactly
    // that: 10k ticks of explosions with not one BrickDestroyed.
    cfg.tuning.start_with[static_cast<int>(PowerupType::Flame)] = 3;
    return Simulation(cfg);
}

bool saw(const Simulation& s, Event::Type t) {
    for (const auto& e : s.state().events)
        if (e.type == t) return true;
    return false;
}

}  // namespace

TEST_CASE("identical seeds and inputs replay identically over 10k ticks") {
    constexpr std::uint64_t kTicks = 10000;

    Simulation a = make(42u);
    Simulation b = make(42u);
    // The floors that make this a sim-half claim: the roster is real and the
    // match is really played. Without them, the empty-sim version of this suite
    // was green while measuring nothing (the shape §12 exists to refuse).
    REQUIRE(a.state().players[0].present);
    REQUIRE(a.state().players[3].present);
    bool bombs = false, explosions = false, bricks = false;
    for (std::uint64_t t = 0; t < kTicks; ++t) {
        TickInputs in = pattern(t);
        a.tick(in);
        b.tick(in);
        bombs = bombs || saw(a, Event::Type::BombPlaced);
        explosions = explosions || saw(a, Event::Type::Explosion);
        bricks = bricks || saw(a, Event::Type::BrickDestroyed);
        if (t % 3 == 0) {
            REQUIRE(next_random(a.state()) == next_random(b.state()));
        }
        if (t % 1000 == 0) REQUIRE(a.hash() == b.hash());
    }
    CHECK(a.hash() == b.hash());
    CHECK(bombs);       // the pattern really dropped bombs...
    CHECK(explosions);  // ...which really went off...
    CHECK(bricks);      // ...and really changed the board

    // A different seed must diverge BEHAVIOURALLY: the hidden-powerup layout,
    // the burns and the deaths all fork, not merely the hashed rng word.
    Simulation c = make(43u);
    for (std::uint64_t t = 0; t < kTicks; ++t) {
        c.tick(pattern(t));
        if (t % 3 == 0) (void)next_random(c.state());
    }
    CHECK(c.hash() != a.hash());  // different seeds must diverge
}
