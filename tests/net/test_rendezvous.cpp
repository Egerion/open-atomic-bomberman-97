// bomber::net Rendezvous test: two peers hole-punch a direct UDP path over
// localhost (ADR-0011 §3). Each is given only the OTHER's address as a
// candidate; both punch simultaneously and must converge on each other, set the
// transport peer, and then exchange an ordinary datagram over that path. Uses
// real sockets, so it soft-skips if the sandbox forbids them (bind fails), like
// the UdpTransport suite — never flakes the pre-push gate.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "bomber/net/rendezvous.hpp"
#include "bomber/net/udp_transport.hpp"

using namespace bomber;  // NOLINT(google-build-using-namespace) — test-local

TEST_CASE("rendezvous punches a direct path over localhost, then carries data") {
    net::UdpTransport a;
    net::UdpTransport b;
    if (!a.bind(0) || !b.bind(0)) {
        MESSAGE("UDP sockets unavailable in this environment; skipping");
        return;
    }

    net::Rendezvous ra(a, {{"127.0.0.1", b.local_port()}}, /*nonce=*/0xAAAAu);
    net::Rendezvous rb(b, {{"127.0.0.1", a.local_port()}}, /*nonce=*/0xBBBBu);

    std::int64_t now = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while ((!ra.done() || !rb.done()) && std::chrono::steady_clock::now() < deadline) {
        ra.step(now);
        rb.step(now);
        now += 5;  // synthetic clock; real delivery via the sleep below
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    REQUIRE(ra.connected());
    REQUIRE(rb.connected());
    REQUIRE(ra.winner() != nullptr);
    REQUIRE(rb.winner() != nullptr);
    // Each latched the other's actual address as the winning path.
    CHECK(ra.winner()->port == b.local_port());
    CHECK(rb.winner()->port == a.local_port());
    CHECK(ra.rtt_ms() >= 0);
    CHECK(rb.rtt_ms() >= 0);

    // set_peer() ran on both, so an ordinary send()/poll() now flows over the
    // punched path.
    const std::vector<std::uint8_t> payload = {9, 8, 7};
    a.send(payload.data(), payload.size());
    std::vector<std::uint8_t> got;
    bool received = false;
    const auto d2 = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < d2) {
        if (b.poll(&got)) {
            received = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(received);
    CHECK(got == payload);
}
