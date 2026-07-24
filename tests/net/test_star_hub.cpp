// bomber::net StarHubTransport tests (ADR-0011 decisions 2+4): the host-relay
// star that carries matches with more than two seats. The hub fans its own
// datagrams out to every guest and REFLECTS each guest's datagram to the others,
// so guests never talk to each other — linear packets and punches instead of a
// quadratic mesh. Real localhost sockets; soft-skips if the sandbox forbids them.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "bomber/net/star_hub_transport.hpp"
#include "bomber/net/udp_transport.hpp"

using namespace bomber::net;  // NOLINT(google-build-using-namespace) — test-local

namespace {

// Wait briefly for a datagram on `tp` (localhost delivery is fast but not
// instant). Returns false if nothing arrived.
bool recv_within(UdpTransport& tp, std::vector<std::uint8_t>* out, int ms = 1000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (tp.poll(out)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

bool hub_poll_within(StarHubTransport& hub, std::vector<std::uint8_t>* out, int ms = 1000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (hub.poll(out)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

}  // namespace

TEST_CASE("the hub fans its own datagram out to every guest") {
    UdpTransport hub_sock;
    UdpTransport g1;
    UdpTransport g2;
    if (!hub_sock.bind(0) || !g1.bind(0) || !g2.bind(0)) {
        MESSAGE("UDP sockets unavailable in this environment; skipping");
        return;
    }
    StarHubTransport hub(hub_sock, {{"127.0.0.1", g1.local_port()}, {"127.0.0.1", g2.local_port()}});
    CHECK(hub.guest_count() == 2);

    const std::vector<std::uint8_t> frame = {1, 2, 3, 4};
    hub.send(frame.data(), frame.size());

    std::vector<std::uint8_t> a;
    std::vector<std::uint8_t> b;
    REQUIRE(recv_within(g1, &a));
    REQUIRE(recv_within(g2, &b));
    CHECK(a == frame);
    CHECK(b == frame);
}

TEST_CASE("a guest's datagram reaches the hub AND is reflected to the other guest") {
    UdpTransport hub_sock;
    UdpTransport g1;
    UdpTransport g2;
    if (!hub_sock.bind(0) || !g1.bind(0) || !g2.bind(0)) {
        MESSAGE("UDP sockets unavailable in this environment; skipping");
        return;
    }
    StarHubTransport hub(hub_sock, {{"127.0.0.1", g1.local_port()}, {"127.0.0.1", g2.local_port()}});

    // Guest 1 sends its input frame to the hub (a guest needs no special
    // transport — a plain UdpTransport aimed at the hub).
    REQUIRE(g1.set_peer("127.0.0.1", hub_sock.local_port()));
    const std::vector<std::uint8_t> frame = {9, 8, 7, 6, 5};
    g1.send(frame.data(), frame.size());

    // The hub's own session receives it...
    std::vector<std::uint8_t> at_hub;
    REQUIRE(hub_poll_within(hub, &at_hub));
    CHECK(at_hub == frame);

    // ...and guest 2 gets it reflected, verbatim, without ever talking to g1.
    std::vector<std::uint8_t> at_g2;
    REQUIRE(recv_within(g2, &at_g2));
    CHECK(at_g2 == frame);

    // The sender must NOT get its own frame echoed back.
    std::vector<std::uint8_t> echoed;
    CHECK_FALSE(recv_within(g1, &echoed, 200));
}

TEST_CASE("datagrams from a stranger are dropped, not reflected") {
    UdpTransport hub_sock;
    UdpTransport g1;
    UdpTransport outsider;
    if (!hub_sock.bind(0) || !g1.bind(0) || !outsider.bind(0)) {
        MESSAGE("UDP sockets unavailable in this environment; skipping");
        return;
    }
    // Only g1 is a seat in this match; `outsider` is not.
    StarHubTransport hub(hub_sock, {{"127.0.0.1", g1.local_port()}});

    const std::vector<std::uint8_t> junk = {0xFF, 0xFF};
    outsider.send_to("127.0.0.1", hub_sock.local_port(), junk.data(), junk.size());

    // The hub's session never sees it...
    std::vector<std::uint8_t> at_hub;
    CHECK_FALSE(hub_poll_within(hub, &at_hub, 300));
    // ...and it is never reflected into the match (which would let an outsider
    // inject input frames at every peer).
    std::vector<std::uint8_t> at_g1;
    CHECK_FALSE(recv_within(g1, &at_g1, 200));
}
