// bomber::net SeedHandshake tests (ADR-0010 §3.3 step 5): the pre-match seed
// exchange that lets a menu-driven host/join flow agree on the match seed with
// no --seed on either command line. Two cases:
//   (a) headless LoopbackLink — two handshakes pumped together converge and
//       agree on the host's seed;
//   (b) real UDP over localhost — the host only bind()s a known port (it must
//       LEARN the guest from its first datagram, exactly the menu flow), the
//       guest set_peer()s the host; both reach done() with the same seed and the
//       host ends up with a peer.
// The UDP case soft-skips if the sandbox forbids sockets (bind fails), mirroring
// test_udp_transport.cpp, so it never flakes the pre-push gate.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>

#include "bomber/net/handshake.hpp"
#include "bomber/net/transport.hpp"
#include "bomber/net/udp_transport.hpp"

using namespace bomber;  // NOLINT(google-build-using-namespace) — test-local

namespace {
constexpr std::uint32_t kHostSeed = 0xC0FFEEu;
}  // namespace

TEST_CASE("SeedHandshake converges over a loopback link and agrees on the host seed") {
    net::LoopbackLink link;
    net::LoopbackTransport ta(link, 0);
    net::LoopbackTransport tb(link, 1);
    net::SeedHandshake host(ta, /*is_host=*/true, kHostSeed);
    net::SeedHandshake guest(tb, /*is_host=*/false, 0);

    // Pump both in lockstep, advancing the shared delivery clock each round,
    // until both finish (a generous bound — convergence takes only a couple of
    // rounds even with the send-then-poll ordering).
    int rounds = 0;
    for (; rounds < 100 && !(host.done() && guest.done()); ++rounds) {
        host.step();
        guest.step();
        link.step();
    }
    CHECK(host.done());
    CHECK(guest.done());
    CHECK(host.seed() == kHostSeed);
    CHECK(guest.seed() == kHostSeed);  // the guest adopted the host's seed
}

TEST_CASE("SeedHandshake converges over real UDP on localhost; the host learns the guest") {
    net::UdpTransport host_t;
    net::UdpTransport guest_t;
    // The host binds a KNOWN port and does NOT set_peer — it must learn the guest
    // from the first ACK (the menu flow: only the joiner types an address). The
    // guest binds an ephemeral port and aims at the host.
    if (!host_t.bind(0) || !guest_t.bind(0)) {
        MESSAGE("UDP sockets unavailable in this environment; skipping");
        return;
    }
    if (!guest_t.set_peer("127.0.0.1", host_t.local_port())) {
        MESSAGE("UDP set_peer unavailable in this environment; skipping");
        return;
    }
    CHECK_FALSE(host_t.has_peer());  // the host knows no peer up front

    net::SeedHandshake host(host_t, /*is_host=*/true, kHostSeed);
    net::SeedHandshake guest(guest_t, /*is_host=*/false, 0);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!(host.done() && guest.done()) && std::chrono::steady_clock::now() < deadline) {
        host.step();
        guest.step();
    }
    CHECK(host.done());
    CHECK(guest.done());
    CHECK(host.seed() == kHostSeed);
    CHECK(guest.seed() == kHostSeed);
    CHECK(host_t.has_peer());  // learned from the guest's first datagram
}
