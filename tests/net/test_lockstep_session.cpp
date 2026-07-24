// bomber::net LockstepSession tests: input-delay lockstep over a latent
// in-memory link (docs/re/multiplayer.md §3.3 step 3, ADR-0010). Two sessions,
// each owning one seat, exchange inputs + Simulation::hash() only through the
// transport; the harness asserts they stay in perfect sync over a real match,
// and that the hash exchange actually CATCHES a divergence (so the sync check is
// not a false negative). No sockets — a LoopbackLink models UDP latency.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>

#include "bomber/net/lockstep_session.hpp"
#include "bomber/net/transport.hpp"
#include "helpers.hpp"  // bomber::sim::test::open_config

using namespace bomber;                  // NOLINT(google-build-using-namespace) — test-local
using bomber::sim::test::open_config;

namespace {

constexpr std::uint16_t kSeat0 = 0x1;
constexpr std::uint16_t kSeat1 = 0x2;
constexpr std::uint16_t kBoth = 0x3;

sim::TickInputs seat_input(int seat, const sim::PlayerInput& in) {
    sim::TickInputs t;
    t.players[static_cast<std::size_t>(seat)] = in;
    return t;
}

// Deterministic per-seat script: cycle moves, drop a bomb every 8th tick,
// phase-shifted per seat so the two seats differ (a real divergence test).
sim::PlayerInput scripted(int seat, std::uint32_t tick) {
    sim::PlayerInput in;
    switch ((tick + static_cast<std::uint32_t>(seat) * 7U) % 8U) {
        case 0: in.right = true; break;
        case 1: in.down = true; break;
        case 2: in.left = true; break;
        case 3: in.up = true; break;
        case 4: in.action1 = true; break;
        default: break;
    }
    return in;
}

// One round of the harness: each peer feeds its OWN seat's scripted input (keyed
// on that session's next input frame, so a stalled tick re-uses the same value)
// and tries to advance; then the link's delivery clock ticks once.
void pump(net::LockstepSession& a, net::LockstepSession& b, net::LoopbackLink& link) {
    a.advance(seat_input(0, scripted(0, a.input_tick())));
    b.advance(seat_input(1, scripted(1, b.input_tick())));
    link.step();
}

}  // namespace

TEST_CASE("input-delay lockstep: two peers over a latent link stay in perfect sync") {
    net::LoopbackLink link(/*latency=*/3);
    net::LoopbackTransport ta(link, 0);
    net::LoopbackTransport tb(link, 1);
    net::LockstepSession a(open_config(), kSeat0, kBoth, /*input_delay=*/4, ta);
    net::LockstepSession b(open_config(), kSeat1, kBoth, /*input_delay=*/4, tb);

    constexpr int kTarget = 600;         // 30 s at 20 Hz
    constexpr int kMaxRounds = kTarget * 4;  // headroom; a stall-free run needs ~kTarget
    int rounds = 0;
    while ((static_cast<int>(a.confirmed_tick()) < kTarget ||
            static_cast<int>(b.confirmed_tick()) < kTarget) &&
           rounds < kMaxRounds) {
        pump(a, b, link);
        ++rounds;
    }

    // delay 4 >= latency 3 hides the lag: both reach the target without a
    // permanent stall (and well inside the round budget).
    CHECK(static_cast<int>(a.confirmed_tick()) >= kTarget);
    CHECK(static_cast<int>(b.confirmed_tick()) >= kTarget);

    // Flush in-flight hash frames so the final ticks' hashes are compared too.
    for (int i = 0; i < 32; ++i) pump(a, b, link);

    // The sessions' OWN per-tick hash exchange verified every confirmed tick
    // matched — no divergence anywhere over the whole match.
    CHECK_FALSE(a.desynced());
    CHECK_FALSE(b.desynced());
}

TEST_CASE("the hash exchange catches a divergence (mismatched seeds)") {
    sim::MatchConfig cfg_a = open_config();
    sim::MatchConfig cfg_b = open_config();
    cfg_b.seed = cfg_a.seed + 1;  // a DIFFERENT arena => hashes differ from tick 0

    net::LoopbackLink link(/*latency=*/0);
    net::LoopbackTransport ta(link, 0);
    net::LoopbackTransport tb(link, 1);
    net::LockstepSession a(cfg_a, kSeat0, kBoth, /*input_delay=*/2, ta);
    net::LockstepSession b(cfg_b, kSeat1, kBoth, /*input_delay=*/2, tb);

    for (int i = 0; i < 100; ++i) pump(a, b, link);

    // The mismatched state trips the hash check on both peers — loudly, not
    // silently corrected (ADR-0010).
    CHECK(a.desynced());
    CHECK(b.desynced());
}
