// bomber::net RollbackSession tests: GGPO-style predict-and-rollback (ADR-0010
// §3.3 step 4). Under latency each peer PREDICTS the other's input and simulates
// ahead; when the real input arrives and disagrees, it restores a snapshot and
// re-simulates. The test asserts the CONFIRMED states stay in perfect agreement
// between the two peers (the session's own per-confirmed-tick hash exchange), and
// that speculation actually happened (so the rollback path was exercised, not a
// trivial zero-latency run). No sockets — a LoopbackLink models the latency.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>

#include "bomber/net/rollback_session.hpp"
#include "bomber/net/transport.hpp"
#include "helpers.hpp"        // bomber::sim::test::open_config
#include "input_scripts.hpp"  // the antiphase walk; see that header on the three scripts

using namespace bomber;                  // NOLINT(google-build-using-namespace) — test-local
using bomber::sim::test::open_config;
using bomber::test::scripted_cycle6;
using bomber::test::seat_input;

namespace {

constexpr std::uint16_t kSeat0 = 0x1;
constexpr std::uint16_t kSeat1 = 0x2;
constexpr std::uint16_t kBoth = 0x3;

}  // namespace

TEST_CASE("rollback: peers' confirmed states stay in perfect agreement under latency") {
    net::LoopbackLink link(/*latency=*/4);
    net::LoopbackTransport ta(link, 0);
    net::LoopbackTransport tb(link, 1);
    sim::Simulation sa(open_config());
    sim::Simulation sb(open_config());
    net::RollbackSession a(sa, kSeat0, kBoth, /*max_prediction=*/16, ta);
    net::RollbackSession b(sb, kSeat1, kBoth, /*max_prediction=*/16, tb);

    auto pump = [&] {
        a.advance(seat_input(0, scripted_cycle6(0, a.predicted_tick())));
        b.advance(seat_input(1, scripted_cycle6(1, b.predicted_tick())));
        link.step();
    };

    bool speculated = false;  // did the display ever run ahead of confirmed (a real prediction)?
    constexpr int kTarget = 500;
    int rounds = 0;
    while ((static_cast<int>(a.predicted_tick()) < kTarget ||
            static_cast<int>(b.predicted_tick()) < kTarget) &&
           rounds < kTarget * 4) {
        pump();
        if (a.predicted_tick() > a.confirmed_tick() || b.predicted_tick() > b.confirmed_tick())
            speculated = true;
        ++rounds;
    }

    CHECK(static_cast<int>(a.predicted_tick()) >= kTarget);
    CHECK(static_cast<int>(b.predicted_tick()) >= kTarget);
    CHECK(speculated);  // latency forced prediction -> the rollback machinery was actually used

    for (int i = 0; i < 64; ++i) pump();  // flush late inputs + confirmed-hash frames

    // Every confirmed tick's hash matched between the peers: rollback kept the
    // authoritative states bit-identical the whole match.
    CHECK_FALSE(a.desynced());
    CHECK_FALSE(b.desynced());
    // With no packet loss the confirmed frontier should have advanced well into
    // the match (it lags the predicted head by only ~latency).
    CHECK(a.confirmed_tick() > static_cast<std::uint32_t>(kTarget) - 32);
}

TEST_CASE("rollback: the hash exchange still catches a real divergence (mismatched seeds)") {
    sim::MatchConfig cfg_a = open_config();
    sim::MatchConfig cfg_b = open_config();
    cfg_b.seed = cfg_a.seed + 1;  // different arena => confirmed hashes must differ

    net::LoopbackLink link(/*latency=*/0);
    net::LoopbackTransport ta(link, 0);
    net::LoopbackTransport tb(link, 1);
    sim::Simulation sa(cfg_a);
    sim::Simulation sb(cfg_b);
    net::RollbackSession a(sa, kSeat0, kBoth, /*max_prediction=*/8, ta);
    net::RollbackSession b(sb, kSeat1, kBoth, /*max_prediction=*/8, tb);

    for (int i = 0; i < 100; ++i) {
        a.advance(seat_input(0, scripted_cycle6(0, a.predicted_tick())));
        b.advance(seat_input(1, scripted_cycle6(1, b.predicted_tick())));
        link.step();
    }
    CHECK(a.desynced());
    CHECK(b.desynced());
}
