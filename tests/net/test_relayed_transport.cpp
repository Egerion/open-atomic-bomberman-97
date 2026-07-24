// bomber::net RelayedTransport tests (ADR-0011 decision 3 / PROTOCOL.md §6): the
// TURN-like fallback used when the hole punch fails. A third UdpTransport plays
// the relay — it strips the 17-byte routing header, swaps in the destination's
// alloc_id + the sender's seat, and forwards the opaque payload — so this proves
// the client half of the frozen format without the Go binary.
// Soft-skips if the sandbox forbids sockets.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "bomber/net/relayed_transport.hpp"
#include "bomber/net/udp_transport.hpp"

using namespace bomber::net;  // NOLINT(google-build-using-namespace) — test-local

namespace {

std::array<std::uint8_t, kRelayAllocIdBytes> alloc_bytes(std::uint8_t fill) {
    std::array<std::uint8_t, kRelayAllocIdBytes> a{};
    a.fill(fill);
    return a;
}

}  // namespace

TEST_CASE("parse_alloc_id decodes the server's 32-hex handle") {
    std::array<std::uint8_t, kRelayAllocIdBytes> out{};
    REQUIRE(parse_alloc_id("000102030405060708090a0b0c0d0e0f", &out));
    for (std::size_t i = 0; i < kRelayAllocIdBytes; ++i) CHECK(out[i] == i);
    // Upper case is equally valid hex.
    REQUIRE(parse_alloc_id("000102030405060708090A0B0C0D0E0F", &out));
    CHECK(out[10] == 0x0a);
    // Untrusted input: wrong length or non-hex is rejected, never decoded.
    CHECK_FALSE(parse_alloc_id("", &out));
    CHECK_FALSE(parse_alloc_id("00010203", &out));
    CHECK_FALSE(parse_alloc_id("000102030405060708090a0b0c0d0e0", &out));   // 31
    CHECK_FALSE(parse_alloc_id("000102030405060708090a0b0c0d0e0ff", &out));  // 33
    CHECK_FALSE(parse_alloc_id("zz0102030405060708090a0b0c0d0e0f", &out));
}

TEST_CASE("RelayedTransport frames datagrams to the frozen §6 layout") {
    UdpTransport client;
    UdpTransport relay;
    if (!client.bind(0) || !relay.bind(0)) {
        MESSAGE("UDP sockets unavailable in this environment; skipping");
        return;
    }
    const auto mine = alloc_bytes(0xAB);
    RelayedTransport rt(client, "127.0.0.1", relay.local_port(), mine, /*dst_seat=*/3);

    const std::vector<std::uint8_t> payload = {7, 7, 7, 1};
    rt.send(payload.data(), payload.size());

    std::vector<std::uint8_t> got;
    bool received = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (relay.poll_from(&got, nullptr, nullptr)) {
            received = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(received);
    REQUIRE(got.size() == kRelayHeaderBytes + payload.size());
    for (std::size_t i = 0; i < kRelayAllocIdBytes; ++i) CHECK(got[i] == 0xAB);  // our alloc_id
    CHECK(got[kRelayAllocIdBytes] == 3);  // destination seat
    CHECK(std::vector<std::uint8_t>(got.begin() + kRelayHeaderBytes, got.end()) == payload);
}

TEST_CASE("RelayedTransport round-trips a payload through a fake forwarder") {
    UdpTransport a;
    UdpTransport b;
    UdpTransport relay;
    if (!a.bind(0) || !b.bind(0) || !relay.bind(0)) {
        MESSAGE("UDP sockets unavailable in this environment; skipping");
        return;
    }
    const auto alloc_a = alloc_bytes(0x11);
    const auto alloc_b = alloc_bytes(0x22);
    RelayedTransport ra(a, "127.0.0.1", relay.local_port(), alloc_a, /*dst_seat=*/1);
    RelayedTransport rb(b, "127.0.0.1", relay.local_port(), alloc_b, /*dst_seat=*/0);

    const std::vector<std::uint8_t> payload = {0xDE, 0xAD, 0xBE, 0xEF};
    ra.send(payload.data(), payload.size());

    // The fake relay: read A's datagram, re-address it to B with B's alloc_id
    // and A's seat, and forward the payload untouched (it never decodes it).
    std::vector<std::uint8_t> in;
    std::string src_ip;
    std::uint16_t src_port = 0;
    bool forwarded = false;
    const auto d1 = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < d1) {
        if (relay.poll_from(&in, &src_ip, &src_port)) {
            REQUIRE(in.size() > kRelayHeaderBytes);
            std::vector<std::uint8_t> out(alloc_b.begin(), alloc_b.end());
            out.push_back(0);  // sender seat (A owns seat 0)
            out.insert(out.end(), in.begin() + kRelayHeaderBytes, in.end());
            relay.send_to("127.0.0.1", b.local_port(), out.data(), out.size());
            forwarded = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(forwarded);

    std::vector<std::uint8_t> got;
    bool received = false;
    const auto d2 = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < d2) {
        if (rb.poll(&got)) {
            received = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(received);
    CHECK(got == payload);          // the session above sees the payload verbatim
    CHECK(rb.last_src_seat() == 0);  // and knows which seat sent it
}

TEST_CASE("RelayedTransport drops junk and other allocations' traffic") {
    UdpTransport client;
    UdpTransport sender;
    if (!client.bind(0) || !sender.bind(0)) {
        MESSAGE("UDP sockets unavailable in this environment; skipping");
        return;
    }
    const auto mine = alloc_bytes(0x33);
    RelayedTransport rt(client, "127.0.0.1", sender.local_port(), mine, /*dst_seat=*/1);

    // Too short to carry the header, and a full datagram addressed to a DIFFERENT
    // allocation — both must be dropped, not surfaced to the session.
    const std::vector<std::uint8_t> runt = {1, 2, 3};
    sender.send_to("127.0.0.1", client.local_port(), runt.data(), runt.size());
    const auto other = alloc_bytes(0x99);
    std::vector<std::uint8_t> foreign(other.begin(), other.end());
    foreign.push_back(0);
    foreign.insert(foreign.end(), {9, 9});
    sender.send_to("127.0.0.1", client.local_port(), foreign.data(), foreign.size());

    std::vector<std::uint8_t> got;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    bool surfaced = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (rt.poll(&got)) {
            surfaced = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK_FALSE(surfaced);
}
