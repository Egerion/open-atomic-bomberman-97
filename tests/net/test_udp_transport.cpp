// bomber::net UdpTransport tests: a localhost datagram round-trip, and the FULL
// lockstep stack running over two real UDP sockets (docs/re/multiplayer.md §3.3
// step 2, ADR-0010). This is the "two peers actually play over the network"
// proof — over 127.0.0.1 so it needs no external network. Both cases soft-skip
// if the sandbox forbids sockets (bind fails), like the visual golden skips
// without an install, so they never flake the pre-push gate.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "bomber/net/lockstep_session.hpp"
#include "bomber/net/udp_transport.hpp"
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

// Cross-connect two localhost UDP sockets on OS-chosen ports. Returns false if
// the sandbox forbids sockets (bind failure) so the caller can soft-skip.
bool connect_pair(net::UdpTransport& a, net::UdpTransport& b) {
    if (!a.bind(0) || !b.bind(0)) return false;
    return a.set_peer("127.0.0.1", b.local_port()) && b.set_peer("127.0.0.1", a.local_port());
}

}  // namespace

TEST_CASE("UdpTransport round-trips a datagram over localhost") {
    net::UdpTransport a;
    net::UdpTransport b;
    if (!connect_pair(a, b)) {
        MESSAGE("UDP sockets unavailable in this environment; skipping");
        return;
    }

    const std::vector<std::uint8_t> payload = {1, 2, 3, 4, 5};
    a.send(payload.data(), payload.size());

    std::vector<std::uint8_t> got;
    bool received = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (b.poll(&got)) {
            received = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(received);
    CHECK(got == payload);
}

TEST_CASE("lockstep over real UDP (localhost): two peers stay in perfect sync") {
    net::UdpTransport ta;
    net::UdpTransport tb;
    if (!connect_pair(ta, tb)) {
        MESSAGE("UDP sockets unavailable in this environment; skipping");
        return;
    }

    net::LockstepSession a(open_config(), kSeat0, kBoth, /*input_delay=*/3, ta);
    net::LockstepSession b(open_config(), kSeat1, kBoth, /*input_delay=*/3, tb);

    constexpr int kTarget = 120;  // 6 s of gameplay over the loopback network
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    auto pump = [&] {
        a.advance(seat_input(0, scripted(0, a.input_tick())));
        b.advance(seat_input(1, scripted(1, b.input_tick())));
        std::this_thread::sleep_for(std::chrono::microseconds(100));  // let the OS deliver
    };

    while ((static_cast<int>(a.confirmed_tick()) < kTarget ||
            static_cast<int>(b.confirmed_tick()) < kTarget) &&
           std::chrono::steady_clock::now() < deadline) {
        pump();
    }
    CHECK(static_cast<int>(a.confirmed_tick()) >= kTarget);
    CHECK(static_cast<int>(b.confirmed_tick()) >= kTarget);

    for (int i = 0; i < 200; ++i) pump();  // flush in-flight hash frames
    CHECK_FALSE(a.desynced());
    CHECK_FALSE(b.desynced());
}
