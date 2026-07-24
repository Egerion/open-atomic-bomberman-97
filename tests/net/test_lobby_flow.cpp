// bomber::net LobbyFlow tests (ADR-0011, design §5.1): the pre-match state
// machine — lobby identity, roster, candidate exchange, START, and the punch.
//
// The machine is driven through handle_server_message() with synthetic server
// frames, so NO matchmaker binary is needed. The final case is the real payoff:
// two flows on two real localhost sockets, handed each other's candidates, punch
// each other for real and both reach Ready — the whole lobby→rendezvous→play
// handoff proven end-to-end. Soft-skips if the sandbox forbids sockets.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include "bomber/net/lobby_client.hpp"
#include "bomber/net/lobby_flow.hpp"
#include "bomber/net/udp_transport.hpp"

using namespace bomber::net;  // NOLINT(google-build-using-namespace) — test-local

namespace {

LobbyServerMessage lobby_created(const std::string& code, int seat) {
    LobbyServerMessage m;
    m.type = LobbyMsgType::LobbyCreated;
    m.code = code;
    m.lobby_id = "L1";
    m.host_token = "TOK";
    m.your_seat = seat;
    return m;
}

LobbyServerMessage join_accepted(int seat) {
    LobbyServerMessage m;
    m.type = LobbyMsgType::JoinAccepted;
    m.lobby_id = "L1";
    m.your_seat = seat;
    m.roster = {{0, "Ege", false, true, -1}, {seat, "Ada", false, false, -1}};
    return m;
}

LobbyServerMessage start_match(std::uint32_t seed, std::uint16_t local_mask) {
    LobbyServerMessage m;
    m.type = LobbyMsgType::StartMatch;
    m.seed = seed;
    m.input_delay = 2;
    m.local_seats_mask = local_mask;
    m.hub_seat = 0;
    m.seat_assign = {0, 1};
    return m;
}

LobbyServerMessage peer_candidates(int seat, const std::string& addr) {
    LobbyServerMessage m;
    m.type = LobbyMsgType::PeerCandidates;
    m.candidates_seat = seat;
    m.candidates = {LobbyCandidate{"host", addr, ""}};
    return m;
}

LobbyFlow::Config test_config(const std::string& name, std::uint16_t sink_port) {
    LobbyFlow::Config c;
    c.server_url = "ws://127.0.0.1:1/ws";  // never connected in these tests
    c.stun_host = "127.0.0.1";
    c.stun_port = sink_port;  // a bound-but-silent socket: probes are swallowed
    c.player_name = name;
    c.build_hash = 0xABCDEF01u;
    return c;
}

}  // namespace

TEST_CASE("LobbyFlow tracks lobby identity and roster") {
    UdpTransport tp;
    UdpTransport sink;
    if (!tp.bind(0) || !sink.bind(0)) {
        MESSAGE("UDP sockets unavailable in this environment; skipping");
        return;
    }
    LobbyClient client;
    LobbyFlow flow(test_config("Ege", sink.local_port()), tp, client);

    CHECK(flow.phase() == LobbyFlow::Phase::Idle);

    flow.handle_server_message(lobby_created("K7Q2MP", 0));
    CHECK(flow.phase() == LobbyFlow::Phase::InLobby);
    CHECK(flow.code() == "K7Q2MP");
    CHECK(flow.my_seat() == 0);
    CHECK(flow.is_host());

    LobbyServerMessage ru;
    ru.type = LobbyMsgType::RosterUpdate;
    ru.roster = {{0, "Ege", true, true, -1}, {1, "Ada", false, false, -1}};
    flow.handle_server_message(ru);
    REQUIRE(flow.roster().size() == 2);
    CHECK(flow.roster()[1].name == "Ada");
    CHECK(flow.roster()[0].ready);
    CHECK(flow.is_host());  // still seat 0
}

TEST_CASE("LobbyFlow surfaces join rejections as player-facing errors") {
    UdpTransport tp;
    UdpTransport sink;
    if (!tp.bind(0) || !sink.bind(0)) {
        MESSAGE("UDP sockets unavailable in this environment; skipping");
        return;
    }
    LobbyClient client;
    LobbyFlow flow(test_config("Ada", sink.local_port()), tp, client);

    LobbyServerMessage rej;
    rej.type = LobbyMsgType::JoinRejected;
    rej.reason = "build_mismatch";
    flow.handle_server_message(rej);
    CHECK(flow.phase() == LobbyFlow::Phase::Failed);
    CHECK(flow.error().find("VERSION") != std::string::npos);
}

TEST_CASE("LobbyFlow fails cleanly when START arrives with no peer address") {
    UdpTransport tp;
    UdpTransport sink;
    if (!tp.bind(0) || !sink.bind(0)) {
        MESSAGE("UDP sockets unavailable in this environment; skipping");
        return;
    }
    LobbyClient client;
    LobbyFlow flow(test_config("Ege", sink.local_port()), tp, client);
    flow.handle_server_message(lobby_created("K7Q2MP", 0));
    flow.handle_server_message(start_match(0x1234u, 0b01));
    CHECK(flow.phase() == LobbyFlow::Phase::Rendezvous);
    flow.step(0);  // no candidates were ever exchanged
    CHECK(flow.phase() == LobbyFlow::Phase::Failed);
    CHECK_FALSE(flow.error().empty());
}

TEST_CASE("two LobbyFlows punch each other and both reach Ready") {
    UdpTransport ta;
    UdpTransport tb;
    UdpTransport sink;
    if (!ta.bind(0) || !tb.bind(0) || !sink.bind(0)) {
        MESSAGE("UDP sockets unavailable in this environment; skipping");
        return;
    }
    LobbyClient ca;
    LobbyClient cb;
    LobbyFlow a(test_config("Ege", sink.local_port()), ta, ca);
    LobbyFlow b(test_config("Ada", sink.local_port()), tb, cb);

    // Seats + lobby identity, as the server would hand them out.
    a.handle_server_message(lobby_created("K7Q2MP", 0));
    b.handle_server_message(join_accepted(1));
    REQUIRE(a.phase() == LobbyFlow::Phase::InLobby);
    REQUIRE(b.phase() == LobbyFlow::Phase::InLobby);

    // The server's candidate fan-out (each learns the other's address).
    const std::string addr_a = "127.0.0.1:" + std::to_string(ta.local_port());
    const std::string addr_b = "127.0.0.1:" + std::to_string(tb.local_port());
    a.handle_server_message(peer_candidates(1, addr_b));
    b.handle_server_message(peer_candidates(0, addr_a));

    // The host presses START; the server broadcasts identical parity payloads
    // with a per-recipient local_seats_mask.
    a.handle_server_message(start_match(0xC0FFEEu, 0b01));
    b.handle_server_message(start_match(0xC0FFEEu, 0b10));

    std::int64_t now = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while ((a.phase() == LobbyFlow::Phase::Rendezvous || b.phase() == LobbyFlow::Phase::Rendezvous) &&
           std::chrono::steady_clock::now() < deadline) {
        a.step(now);
        b.step(now);
        now += 5;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    REQUIRE(a.phase() == LobbyFlow::Phase::Ready);
    REQUIRE(b.phase() == LobbyFlow::Phase::Ready);

    // Both adopted the SAME authoritative match parameters, with their own seats.
    CHECK(a.match_start().seed == 0xC0FFEEu);
    CHECK(b.match_start().seed == 0xC0FFEEu);
    CHECK(a.match_start().input_delay == b.match_start().input_delay);
    CHECK(a.match_start().local_seats_mask == 0b01);
    CHECK(b.match_start().local_seats_mask == 0b10);

    // The punch left both transports pointed at each other, so the match's
    // ordinary send()/poll() now flows.
    const std::vector<std::uint8_t> payload = {4, 2};
    ta.send(payload.data(), payload.size());
    std::vector<std::uint8_t> got;
    bool received = false;
    const auto d2 = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < d2) {
        if (tb.poll(&got)) {
            received = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(received);
    CHECK(got == payload);
}
