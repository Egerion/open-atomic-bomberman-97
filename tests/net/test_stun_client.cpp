// bomber::net StunClient test (ADR-0011 §3 / PROTOCOL.md §2): the client probes
// a STUN echo server from the SAME socket the match will punch on and learns its
// reflexive (public, post-NAT) address. A second UdpTransport plays the server —
// it observes the probe's true source and echoes it back, exactly like
// services/matchmaker's stun.go — so this needs no Go binary running.
// Soft-skips if the sandbox forbids sockets.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "bomber/net/lobby_messages.hpp"
#include "bomber/net/stun_client.hpp"
#include "bomber/net/udp_transport.hpp"

using namespace bomber::net;  // NOLINT(google-build-using-namespace) — test-local

namespace {

// Minimal stand-in for stun.go: reply to a StunProbe with the source address we
// observed. Returns true if it answered a probe this pump.
bool pump_fake_stun_server(UdpTransport& server) {
    std::vector<std::uint8_t> buf;
    std::string src_ip;
    std::uint16_t src_port = 0;
    if (!server.poll_from(&buf, &src_ip, &src_port)) return false;
    const std::string text(reinterpret_cast<const char*>(buf.data()), buf.size());
    // Pull the nonce back out of the probe the way the Go server does.
    const std::size_t key = text.find("\"nonce\"");
    if (key == std::string::npos) return false;
    const std::size_t open_q = text.find('"', text.find(':', key) + 1);
    const std::size_t close_q = text.find('"', open_q + 1);
    if (open_q == std::string::npos || close_q == std::string::npos) return false;
    const std::string nonce = text.substr(open_q + 1, close_q - open_q - 1);

    const std::string reply = "{\"type\":\"StunReply\",\"nonce\":\"" + nonce +
                              "\",\"your_addr\":\"" + src_ip + ":" +
                              std::to_string(src_port) + "\"}";
    server.send_to(src_ip, src_port, reinterpret_cast<const std::uint8_t*>(reply.data()),
                   reply.size());
    return true;
}

}  // namespace

TEST_CASE("StunClient learns its reflexive address from an echo server") {
    UdpTransport client;
    UdpTransport server;
    if (!client.bind(0) || !server.bind(0)) {
        MESSAGE("UDP sockets unavailable in this environment; skipping");
        return;
    }
    const std::uint16_t client_port = client.local_port();

    StunClient stun(client, "127.0.0.1", server.local_port(), "nonce-abc");

    std::int64_t now = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!stun.done() && std::chrono::steady_clock::now() < deadline) {
        stun.step(now);
        pump_fake_stun_server(server);
        now += 10;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    REQUIRE(stun.ok());
    // Over loopback the "public" address IS the local one — the point is that the
    // server reported the SOURCE PORT of the very socket the match will use.
    CHECK(stun.reflexive_ip() == "127.0.0.1");
    CHECK(stun.reflexive_port() == client_port);
    CHECK(stun.reflexive_addr() == "127.0.0.1:" + std::to_string(client_port));
}

TEST_CASE("StunClient fails cleanly when nothing answers") {
    UdpTransport client;
    if (!client.bind(0)) {
        MESSAGE("UDP sockets unavailable in this environment; skipping");
        return;
    }
    // Port 1 on loopback: nothing listens, so the probes go unanswered.
    StunClient stun(client, "127.0.0.1", 1, "nonce-x", /*timeout_ms=*/100);
    for (std::int64_t t = 0; t <= 200 && !stun.done(); t += 20) stun.step(t);
    CHECK(stun.done());
    CHECK(stun.failed());
    CHECK(stun.reflexive_addr().empty());
}

TEST_CASE("local_ip_toward reports a usable local address") {
    // Routing toward loopback must yield loopback; toward a public address it
    // yields this machine's LAN address (or "" with no route — then the caller
    // simply offers no host candidate).
    CHECK(local_ip_toward("127.0.0.1", 9) == "127.0.0.1");
    const std::string lan = local_ip_toward("8.8.8.8", 9);
    if (!lan.empty()) CHECK(lan.find('.') != std::string::npos);
}
