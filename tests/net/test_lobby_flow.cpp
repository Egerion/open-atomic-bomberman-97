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

LobbyServerMessage start_match(std::uint32_t seed, std::uint16_t local_mask,
                               std::vector<int> seats = {0, 1}) {
    LobbyServerMessage m;
    m.type = LobbyMsgType::StartMatch;
    m.seed = seed;
    m.input_delay = 2;
    m.local_seats_mask = local_mask;
    m.hub_seat = 0;
    m.seat_assign = std::move(seats);
    return m;
}

LobbyServerMessage peer_candidates(int seat, const std::string& addr) {
    LobbyServerMessage m;
    m.type = LobbyMsgType::PeerCandidates;
    m.candidates_seat = seat;
    m.candidates = {LobbyCandidate{"host", addr, ""}};
    return m;
}

LobbyServerMessage chat_from(int seat, const std::string& name, const std::string& text) {
    LobbyServerMessage m;
    m.type = LobbyMsgType::Chat;
    m.chat_seat = seat;
    m.chat_name = name;
    m.chat_text = text;
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

TEST_CASE("LobbyFlow keeps a bounded ring of sanitised chat and bumps a revision") {
    // PORT-ONLY lobby chat (PROTOCOL.md §7). Inbound frames are another player's
    // typing arriving over a socket, so the flow re-sanitises them itself rather
    // than trusting whatever the server relayed.
    UdpTransport tp;
    UdpTransport sink;
    if (!tp.bind(0) || !sink.bind(0)) {
        MESSAGE("UDP sockets unavailable in this environment; skipping");
        return;
    }
    LobbyClient client;
    LobbyFlow flow(test_config("Ege", sink.local_port()), tp, client);
    flow.handle_server_message(lobby_created("K7Q2MP", 0));

    CHECK(flow.chat_log().empty());
    const unsigned rev0 = flow.chat_revision();

    flow.handle_server_message(chat_from(1, "Ada", "hello"));
    REQUIRE(flow.chat_log().size() == 1);
    CHECK(flow.chat_log()[0].name == "Ada");
    CHECK(flow.chat_log()[0].text == "hello");
    CHECK(flow.chat_log()[0].seat == 1);
    CHECK(flow.chat_revision() == rev0 + 1);

    // Untrusted input: control codes are stripped, a name is clamped, and a
    // message with nothing drawable left is not a message at all.
    flow.handle_server_message(chat_from(1, std::string(40, 'N'), "a\x07 b"));
    REQUIRE(flow.chat_log().size() == 2);
    CHECK(flow.chat_log()[1].name.size() == kChatMaxNameBytes);
    CHECK(flow.chat_log()[1].text == "a b");

    const unsigned before_junk = flow.chat_revision();
    flow.handle_server_message(chat_from(1, "Ada", "\x01\x02"));
    CHECK(flow.chat_log().size() == 2);              // dropped, not appended
    CHECK(flow.chat_revision() == before_junk);      // and it did not "arrive"

    // The ring is bounded: the GUI renders a corner panel, not a transcript.
    for (int i = 0; i < 20; ++i)
        flow.handle_server_message(chat_from(1, "Ada", "line " + std::to_string(i)));
    CHECK(flow.chat_log().size() == LobbyFlow::kChatLogLines);
    CHECK(flow.chat_log().back().text == "line 19");  // newest last
}

TEST_CASE("LobbyFlow::send_chat refuses empties, seatless peers and floods") {
    UdpTransport tp;
    UdpTransport sink;
    if (!tp.bind(0) || !sink.bind(0)) {
        MESSAGE("UDP sockets unavailable in this environment; skipping");
        return;
    }
    LobbyClient client;
    LobbyFlow flow(test_config("Ege", sink.local_port()), tp, client);

    // No seat yet: the server would refuse it, so nothing goes on the wire.
    CHECK_FALSE(flow.send_chat("hello", 0));

    flow.handle_server_message(lobby_created("K7Q2MP", 0));
    CHECK_FALSE(flow.send_chat("", 0));         // nothing to say
    CHECK_FALSE(flow.send_chat("   ", 0));      // ... still nothing
    CHECK_FALSE(flow.send_chat("\x01", 0));     // nothing drawable survives

    // The token bucket mirrors the server's: the burst goes out, the next one is
    // refused HERE (so the player learns) instead of being dropped silently at
    // the far end, and a wait buys exactly one more.
    std::int64_t now = 0;
    for (int i = 0; i < LobbyFlow::kChatBurstMsgs; ++i) CHECK(flow.send_chat("burst", now));
    CHECK_FALSE(flow.send_chat("one too many", now));
    now += LobbyFlow::kChatCreditPerMsgMs;
    CHECK(flow.send_chat("after waiting", now));
    CHECK_FALSE(flow.send_chat("and again", now));
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

TEST_CASE("three LobbyFlows form the star: hub punches both guests, frames reflect") {
    // Phase 4 end-to-end at the netcode level (ADR-0011 decisions 2+4). The hub
    // must open a path to EVERY guest over its one socket, then wrap it in the
    // star so a guest's frame reaches the other guest without them ever talking.
    UdpTransport th;
    UdpTransport t1;
    UdpTransport t2;
    UdpTransport sink;
    if (!th.bind(0) || !t1.bind(0) || !t2.bind(0) || !sink.bind(0)) {
        MESSAGE("UDP sockets unavailable in this environment; skipping");
        return;
    }
    LobbyClient ch;
    LobbyClient c1;
    LobbyClient c2;
    LobbyFlow hub(test_config("HUB", sink.local_port()), th, ch);
    LobbyFlow g1(test_config("G1", sink.local_port()), t1, c1);
    LobbyFlow g2(test_config("G2", sink.local_port()), t2, c2);

    hub.handle_server_message(lobby_created("K7Q2MP", 0));
    g1.handle_server_message(join_accepted(1));
    g2.handle_server_message(join_accepted(2));

    // The server's candidate fan-out: the hub learns BOTH guests; each guest
    // learns only the hub (the star is linear, not a mesh).
    const std::string addr_h = "127.0.0.1:" + std::to_string(th.local_port());
    hub.handle_server_message(peer_candidates(1, "127.0.0.1:" + std::to_string(t1.local_port())));
    hub.handle_server_message(peer_candidates(2, "127.0.0.1:" + std::to_string(t2.local_port())));
    g1.handle_server_message(peer_candidates(0, addr_h));
    g2.handle_server_message(peer_candidates(0, addr_h));

    // START for a 3-seat match, hub_seat 0, per-recipient seat masks.
    const std::vector<int> seats = {0, 1, 2};
    hub.handle_server_message(start_match(0xABCDEFu, 0b001, seats));
    g1.handle_server_message(start_match(0xABCDEFu, 0b010, seats));
    g2.handle_server_message(start_match(0xABCDEFu, 0b100, seats));

    std::int64_t now = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while ((hub.phase() != LobbyFlow::Phase::Ready || g1.phase() != LobbyFlow::Phase::Ready ||
            g2.phase() != LobbyFlow::Phase::Ready) &&
           std::chrono::steady_clock::now() < deadline) {
        hub.step(now);
        g1.step(now);
        g2.step(now);
        now += 5;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    INFO("hub=" << static_cast<int>(hub.phase()) << " '" << hub.error() << "' g1="
                << static_cast<int>(g1.phase()) << " '" << g1.error() << "' g2="
                << static_cast<int>(g2.phase()) << " '" << g2.error() << "'");
    REQUIRE(hub.phase() == LobbyFlow::Phase::Ready);
    REQUIRE(g1.phase() == LobbyFlow::Phase::Ready);
    REQUIRE(g2.phase() == LobbyFlow::Phase::Ready);

    // Guest 1's frame must reach the HUB and be reflected to guest 2 — the whole
    // point of the star (guests never exchange addresses).
    const std::vector<std::uint8_t> frame = {1, 2, 3};
    g1.transport().send(frame.data(), frame.size());

    bool at_hub = false;
    bool at_g2 = false;
    std::vector<std::uint8_t> got;
    const auto d2 = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while ((!at_hub || !at_g2) && std::chrono::steady_clock::now() < d2) {
        if (!at_hub && hub.transport().poll(&got) && got == frame) at_hub = true;
        if (!at_g2 && g2.transport().poll(&got) && got == frame) at_g2 = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(at_hub);
    CHECK(at_g2);
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
