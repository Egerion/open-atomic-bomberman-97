// bomber::net multi-round rotation: the two numbers both peers must agree on for
// round N of an online best-of-N match WITHOUT exchanging an extra byte
// (round_rotation.hpp), plus the RollbackSession tick base those numbers exist to
// feed (rollback_session.hpp's `start_tick`).
//
// The bug this pins: an online match used to run exactly ONE round and drop back
// to the menu, whatever the LEVEL & ROUNDS screen's WINS row said. Rotating
// rounds over one long-lived socket needs (a) a per-round seed both peers derive
// identically, so the host's confirmed config is self-identifying, and (b) a
// per-round tick space that a straggler from the previous round cannot land in.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "bomber/net/protocol.hpp"
#include "bomber/net/rollback_session.hpp"
#include "bomber/net/round_rotation.hpp"
#include "bomber/net/transport.hpp"
#include "helpers.hpp"        // bomber::sim::test::open_config
#include "input_scripts.hpp"  // the antiphase walk; see that header on the three scripts

using namespace bomber;  // NOLINT(google-build-using-namespace) — test-local
using bomber::sim::test::open_config;
using bomber::test::scripted_cycle6;
using bomber::test::seat_input;

namespace {

constexpr std::uint16_t kSeat0 = 0x1;
constexpr std::uint16_t kSeat1 = 0x2;
constexpr std::uint16_t kBoth = 0x3;

}  // namespace

TEST_CASE("round rotation: the per-round seed is derived, not exchanged") {
    constexpr std::uint32_t kMatchSeed = 0xC0FFEEu;

    // Round 0 IS the match seed — the config the host confirmed before the match
    // carries it, so both peers start from the same number with no extra message.
    CHECK(net::round_seed(kMatchSeed, 0) == kMatchSeed);

    // Each later round is one step on, matching the LOCAL path's own per-round
    // walk (MatchRunner::run's start_match(next_seed++)) so an online match's
    // board sequence is the one a local match would have had.
    CHECK(net::round_seed(kMatchSeed, 1) == kMatchSeed + 1);
    CHECK(net::round_seed(kMatchSeed, 7) == kMatchSeed + 7);

    // Distinct per round — this is what makes the host's confirmed config
    // self-identifying, so a blob replayed out of an earlier round can never be
    // mistaken for the next one.
    for (int i = 0; i < 100; ++i)
        for (int j = i + 1; j < 100; ++j)
            REQUIRE(net::round_seed(kMatchSeed, i) != net::round_seed(kMatchSeed, j));
}

TEST_CASE("round rotation: tick bases are strictly increasing and clear a whole round") {
    CHECK(net::round_tick_base(0) == 0u);

    // Every round's base is above the previous one by more than the longest round
    // the game can produce. MatchRunner::build_config caps even the 1001
    // "Infinite" play-time sentinel at 99999 seconds; at 20 Hz that is under
    // 2.0 M ticks, and the stride is 4.19 M.
    constexpr std::uint32_t kLongestRoundTicks = 99999u * 20u;
    static_assert(net::kRoundTickStride > kLongestRoundTicks);
    for (int r = 1; r < 100; ++r) {
        REQUIRE(net::round_tick_base(r) > net::round_tick_base(r - 1));
        REQUIRE(net::round_tick_base(r) - net::round_tick_base(r - 1) > kLongestRoundTicks);
    }

    // The level screen clamps the win target to 100, so a match cannot walk the
    // base anywhere near a uint32 wrap.
    CHECK(net::round_tick_base(100) < 0x7FFFFFFFu);
}

TEST_CASE("round rotation: a session based mid-uint32 stays in sync exactly as one based at 0") {
    // Round 3 of a match: both peers build a fresh session over the same socket,
    // based at round_tick_base(3) rather than restarting the count at 0.
    const std::uint32_t base = net::round_tick_base(3);
    net::LoopbackLink link(/*latency=*/4);
    net::LoopbackTransport ta(link, 0);
    net::LoopbackTransport tb(link, 1);
    sim::Simulation sa(open_config());
    sim::Simulation sb(open_config());
    net::RollbackSession a(sa, kSeat0, kBoth, /*max_prediction=*/16, ta, /*drop=*/{}, base);
    net::RollbackSession b(sb, kSeat1, kBoth, /*max_prediction=*/16, tb, /*drop=*/{}, base);

    CHECK(a.predicted_tick() == base);
    CHECK(a.confirmed_tick() == base);

    const std::uint32_t target = base + 300;
    bool speculated = false;
    int rounds = 0;
    while ((a.predicted_tick() < target || b.predicted_tick() < target) && rounds < 4000) {
        a.advance(seat_input(0, scripted_cycle6(0, a.predicted_tick())));
        b.advance(seat_input(1, scripted_cycle6(1, b.predicted_tick())));
        link.step();
        if (a.predicted_tick() > a.confirmed_tick() || b.predicted_tick() > b.confirmed_tick())
            speculated = true;
        ++rounds;
    }

    CHECK(a.predicted_tick() >= target);
    CHECK(b.predicted_tick() >= target);
    CHECK(speculated);          // latency forced prediction: the rollback path really ran
    CHECK_FALSE(a.desynced());  // the per-confirmed-tick hash exchange agreed throughout
    CHECK_FALSE(b.desynced());
    CHECK(a.confirmed_tick() > base);
}

TEST_CASE("round rotation: a straggler from the previous round cannot poison the next one") {
    // THE HAZARD. Rounds share one socket and the wire carries no round id, so a
    // datagram still in flight (or sitting in the kernel buffer) when round N+1
    // starts is decoded by the new session. If both rounds started at tick 0 it
    // would be filed as a FUTURE input — apply_remote only rejects ticks BELOW
    // confirmed_ — and its HASH would later be compared against a completely
    // different simulation, i.e. a phantom desync. The tick base is what makes
    // that structurally impossible.
    const std::uint32_t prev_base = net::round_tick_base(0);
    const std::uint32_t base = net::round_tick_base(1);

    net::LoopbackLink link(/*latency=*/0);
    net::LoopbackTransport ta(link, 0);
    net::LoopbackTransport tb(link, 1);
    sim::Simulation sa(open_config());
    sim::Simulation sb(open_config());
    net::RollbackSession a(sa, kSeat0, kBoth, /*max_prediction=*/16, ta, /*drop=*/{}, base);
    net::RollbackSession b(sb, kSeat1, kBoth, /*max_prediction=*/16, tb, /*drop=*/{}, base);

    // Peer B's socket receives leftovers from ROUND 0: a late input and the hash
    // that went with it. Both carry ticks from the previous round's space.
    sim::PlayerInput stale;
    stale.action1 = true;
    const std::vector<std::uint8_t> old_input =
        net::encode_input(prev_base + 250, kSeat0, seat_input(0, stale));
    const std::vector<std::uint8_t> old_hash = net::encode_hash(prev_base + 250, 0xDEADBEEFull);
    ta.send(old_input.data(), old_input.size());
    ta.send(old_hash.data(), old_hash.size());
    link.step();

    for (int i = 0; i < 300; ++i) {
        a.advance(seat_input(0, scripted_cycle6(0, a.predicted_tick())));
        b.advance(seat_input(1, scripted_cycle6(1, b.predicted_tick())));
        link.step();
    }

    // The stale pair was swallowed by the `tick < confirmed_` guard instead of
    // being adopted, so round 1 ran clean.
    CHECK_FALSE(b.desynced());
    CHECK_FALSE(a.desynced());
    CHECK(b.confirmed_tick() > base);
}
