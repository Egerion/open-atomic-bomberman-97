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

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "bomber/net/lobby_client.hpp"
#include "bomber/net/lobby_flow.hpp"
#include "bomber/net/relayed_transport.hpp"
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

LobbyServerMessage relay_allocated(const std::string& addr, const std::string& alloc_id) {
    LobbyServerMessage m;
    m.type = LobbyMsgType::RelayAllocated;
    m.relay_addr = addr;
    m.alloc_id = alloc_id;
    return m;
}

// A stand-in for services/matchmaker's UDP forwarder, faithful in the ONE
// respect the asymmetric case turns on: a datagram whose DESTINATION seat holds
// no allocation is DROPPED and counted. That counter is `drop_unknown_dst` in
// internal/relay/relay.go's Table.Forward, and it is what the production logs
// showed climbing while `forwarded` sat at seven datagrams — one seat had a
// relay handle and the other never asked for one.
class FakeRelay {
public:
    explicit FakeRelay(UdpTransport& socket) : socket_(&socket) {}

    // Mint a handle for one seat and return its 32-hex control-plane form.
    std::string allocate(int seat) {
        Alloc a;
        a.seat = seat;
        a.id.fill(static_cast<std::uint8_t>(0xA0 + seat));
        allocs_.push_back(a);
        static const char* kHex = "0123456789abcdef";
        std::string out;
        for (const std::uint8_t byte : a.id) {
            out.push_back(kHex[byte >> 4]);
            out.push_back(kHex[byte & 0x0FU]);
        }
        return out;
    }

    // Forward everything currently queued. Learns each seat's return path off its
    // own datagrams, exactly as the real forwarder does.
    void pump() {
        std::vector<std::uint8_t> in;
        std::string ip;
        std::uint16_t port = 0;
        while (socket_->poll_from(&in, &ip, &port)) {
            if (in.size() <= kRelayHeaderBytes) continue;
            Alloc* from = nullptr;
            for (Alloc& a : allocs_)
                if (std::equal(a.id.begin(), a.id.end(), in.begin())) from = &a;
            if (from == nullptr) continue;
            from->ip = ip;
            from->port = port;
            from->known = true;

            const int dst_seat = in[kRelayAllocIdBytes];
            Alloc* to = nullptr;
            for (Alloc& a : allocs_)
                if (a.seat == dst_seat) to = &a;
            if (to == nullptr) {
                ++drop_unknown_dst_;  // the production symptom, reproduced
                continue;
            }
            if (!to->known) continue;

            std::vector<std::uint8_t> out(to->id.begin(), to->id.end());
            out.push_back(static_cast<std::uint8_t>(from->seat));
            out.insert(out.end(), in.begin() + kRelayHeaderBytes, in.end());
            socket_->send_to(to->ip, to->port, out.data(), out.size());
        }
    }

    int drop_unknown_dst() const { return drop_unknown_dst_; }

private:
    struct Alloc {
        std::array<std::uint8_t, kRelayAllocIdBytes> id{};
        std::string ip;
        int seat = 0;
        std::uint16_t port = 0;
        bool known = false;
    };
    UdpTransport* socket_;
    std::vector<Alloc> allocs_;
    int drop_unknown_dst_ = 0;
};

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
    // NOT on the first pump: START does not wait for the candidate exchange, so
    // a missing address means "not yet" for kCandidateWaitMs (790d876 — a fast
    // START against a remote matchmaker used to fail here while passing
    // locally). It must still give up rather than sit in Rendezvous forever.
    flow.step(0);  // no candidates were ever exchanged
    CHECK(flow.phase() == LobbyFlow::Phase::Rendezvous);
    for (std::int64_t now = 0; now <= 8000 && flow.phase() != LobbyFlow::Phase::Failed; now += 500)
        flow.step(now);
    CHECK(flow.phase() == LobbyFlow::Phase::Failed);
    CHECK_FALSE(flow.error().empty());
}

TEST_CASE("MatchStart::all_seats_mask is DERIVED from the server's seat_assign") {
    // The whole match layer's seat topology — the RollbackSession's `all_seats`
    // and the SetupSession's ack set — comes from this one field. It used to be a
    // hard-coded 0b11 in GameApp, which is what capped an online game at two
    // machines however many seats the lobby actually had.
    UdpTransport tp;
    UdpTransport sink;
    if (!tp.bind(0) || !sink.bind(0)) {
        MESSAGE("UDP sockets unavailable in this environment; skipping");
        return;
    }

    SUBCASE("a sparse assignment becomes exactly those bits") {
        LobbyClient client;
        LobbyFlow flow(test_config("Ege", sink.local_port()), tp, client);
        flow.handle_server_message(lobby_created("K7Q2MP", 0));
        // Seats need not be contiguous: players leaving a lobby free the middle.
        flow.handle_server_message(start_match(0x1u, 0b000001, {0, 2, 5}));
        CHECK(flow.match_start().all_seats_mask == 0b100101);
    }

    SUBCASE("an out-of-range seat is ignored, not shifted into the mask") {
        LobbyClient client;
        LobbyFlow flow(test_config("Ege", sink.local_port()), tp, client);
        flow.handle_server_message(lobby_created("K7Q2MP", 0));
        // Untrusted: the field is a server-supplied index into a 16-bit mask.
        flow.handle_server_message(start_match(0x1u, 0b0001, {0, 1, -3, 99}));
        CHECK(flow.match_start().all_seats_mask == 0b11);
    }

    SUBCASE("a full ten-seat lobby is nothing special") {
        LobbyClient client;
        LobbyFlow flow(test_config("Ege", sink.local_port()), tp, client);
        flow.handle_server_message(lobby_created("K7Q2MP", 0));
        flow.handle_server_message(start_match(0x1u, 0b1, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}));
        CHECK(flow.match_start().all_seats_mask == 0b1111111111);
    }

    SUBCASE("no seat_assign falls back to the roster, and our own seat is never missing") {
        LobbyClient client;
        LobbyFlow flow(test_config("Ada", sink.local_port()), tp, client);
        flow.handle_server_message(join_accepted(1));  // roster = seats 0 and 1
        LobbyServerMessage m = start_match(0x1u, 0b0100, {});
        flow.handle_server_message(m);
        // Seats 0+1 from the roster, plus the local mask the server sent us.
        CHECK(flow.match_start().all_seats_mask == 0b0111);
    }
}

TEST_CASE("a >2-seat lobby refuses the relay fallback instead of half-connecting") {
    // RelayedTransport addresses exactly ONE destination seat, so a relayed star
    // would deliver a guest's inputs to nobody and desync on tick 0. The punch is
    // clock-injected, so the 5 s give-up is reached by advancing `now`, not by
    // waiting.
    UdpTransport tp;
    UdpTransport sink;
    if (!tp.bind(0) || !sink.bind(0)) {
        MESSAGE("UDP sockets unavailable in this environment; skipping");
        return;
    }
    LobbyClient client;
    LobbyFlow flow(test_config("Ege", sink.local_port()), tp, client);
    flow.handle_server_message(lobby_created("K7Q2MP", 0));
    // Both guests' addresses point at a bound-but-silent socket: reachable, so
    // the punch starts, and silent, so it gives up.
    const std::string dead = "127.0.0.1:" + std::to_string(sink.local_port());
    flow.handle_server_message(peer_candidates(1, dead));
    flow.handle_server_message(peer_candidates(2, dead));
    flow.handle_server_message(start_match(0xABCu, 0b001, {0, 1, 2}));

    for (std::int64_t now = 0; now <= 8000 && flow.phase() != LobbyFlow::Phase::Failed; now += 250)
        flow.step(now);

    CHECK(flow.phase() == LobbyFlow::Phase::Failed);
    CHECK(flow.error().find("RELAY NEEDS 2 PLAYERS") != std::string::npos);
    CHECK_FALSE(flow.is_relayed());
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

    // All three agree on the SAME seat topology and disagree only about which
    // seat is theirs — the pair GameApp hands the RollbackSession and the
    // SetupSession, so a mismatch here would be a tick-0 desync.
    CHECK(hub.match_start().all_seats_mask == 0b111);
    CHECK(g1.match_start().all_seats_mask == 0b111);
    CHECK(g2.match_start().all_seats_mask == 0b111);
    CHECK(hub.match_start().local_seats_mask == 0b001);
    CHECK(g1.match_start().local_seats_mask == 0b010);
    CHECK(g2.match_start().local_seats_mask == 0b100);

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

TEST_CASE("a star guest does NOT start a match when the hub could not reach everyone") {
    // THE STAR'S OWN ASYMMETRIC OUTCOME, which the case below never reaches
    // because it is a 2-seat pair. Until 2026-07-31 a >2-seat match skipped
    // verification entirely — `phase_ = can_relay() ? Verifying : Ready` — on the
    // reasoning that a star's punch "already required both halves per guest".
    // That holds for the HUB, which runs the multi-peer Rendezvous. It does not
    // hold for a GUEST: begin_rendezvous gives a star guest the 2-PEER form, the
    // single-sided latch LinkProbe exists to remove, and then the probe was
    // skipped on top.
    //
    // Here seat 2's address is a black hole, so the hub's punch can never confirm
    // every guest and correctly fails (a star has no relay to escalate to). Guest
    // 1's own punch succeeds — the hub echoes its PING while it is still
    // punching — so g1 holds a genuine round-trip proof of a path the hub has
    // already given up on. It must NOT play.
    //
    // The discrimination is direct: restore the `can_relay()` ternary and g1
    // reaches Ready here while its hub is in Failed.
    UdpTransport th;
    UdpTransport t1;
    UdpTransport black_hole;  // "seat 2": bound and silent, so reachable but mute
    UdpTransport sink;
    if (!th.bind(0) || !t1.bind(0) || !black_hole.bind(0) || !sink.bind(0)) {
        MESSAGE("UDP sockets unavailable in this environment; skipping");
        return;
    }
    LobbyClient ch;
    LobbyClient c1;
    LobbyFlow hub(test_config("HUB", sink.local_port()), th, ch);
    LobbyFlow g1(test_config("G1", sink.local_port()), t1, c1);

    hub.handle_server_message(lobby_created("K7Q2MP", 0));
    g1.handle_server_message(join_accepted(1));

    const std::string addr_h = "127.0.0.1:" + std::to_string(th.local_port());
    hub.handle_server_message(peer_candidates(1, "127.0.0.1:" + std::to_string(t1.local_port())));
    hub.handle_server_message(peer_candidates(
        2, "127.0.0.1:" + std::to_string(black_hole.local_port())));
    g1.handle_server_message(peer_candidates(0, addr_h));

    const std::vector<int> seats = {0, 1, 2};
    hub.handle_server_message(start_match(0xABCDEFu, 0b001, seats));
    g1.handle_server_message(start_match(0xABCDEFu, 0b010, seats));

    // Long enough to outlast the hub's 5 s punch window AND g1's own verification
    // deadline (kDirectVerifyMs, measured from the punch's start), so a peer that
    // is going to give up has given up.
    std::int64_t now = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (now <= 12000 && std::chrono::steady_clock::now() < deadline) {
        hub.step(now);
        g1.step(now);
        now += 20;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    INFO("hub=" << static_cast<int>(hub.phase()) << " '" << hub.error() << "' g1="
                << static_cast<int>(g1.phase()) << " '" << g1.error() << "'");
    // The hub gave up, as it always did: one unreachable guest fails a multi-peer
    // punch and there is no relayed star to fall back to.
    CHECK(hub.phase() == LobbyFlow::Phase::Failed);
    // THE PIN. The reachable guest must not be Ready — a match whose hub has left
    // is the half-connected state this whole phase exists to prevent.
    CHECK(g1.phase() != LobbyFlow::Phase::Ready);
    CHECK(g1.phase() == LobbyFlow::Phase::Failed);
    // And it says what actually happened rather than blaming its own path or
    // offering advice about a relay that was never its problem.
    CHECK(g1.error().find("COULD NOT BE REACHED") != std::string::npos);
    CHECK_FALSE(g1.is_relayed());
}

TEST_CASE("an ASYMMETRIC punch outcome still converges on a path that carries") {
    // THE PRODUCTION BUG (match 9V9BHE, Turkey <-> Lithuania). The punch decision
    // is per-peer and unsynchronised: one side can hold a genuine round-trip
    // proof while the other's own proof never lands, so one played direct and the
    // other asked for a relay handle its partner never made. The matchmaker's
    // logs told the whole story — "relay allocated seat=0" exactly once, then
    // `drop_unknown_dst` climbing while `forwarded` stayed at 7 datagrams.
    //
    // Modelled here by giving ONE peer a black hole for its partner's address:
    // A can reach B, B can never reach A, so A's punch completes and B's cannot,
    // however long it tries. What must happen is that BOTH ends end up somewhere
    // a datagram actually crosses — asserted at the bottom by crossing one.
    //
    // The symmetric cases (both punch, both fail) passed before this fix and
    // still do; this is the case that did not exist.
    UdpTransport ta;
    UdpTransport tb;
    UdpTransport relay_sock;
    UdpTransport black_hole;  // bound and silent: reachable, never answers
    UdpTransport sink;
    if (!ta.bind(0) || !tb.bind(0) || !relay_sock.bind(0) || !black_hole.bind(0) || !sink.bind(0)) {
        MESSAGE("UDP sockets unavailable in this environment; skipping");
        return;
    }
    LobbyClient ca;
    LobbyClient cb;
    LobbyFlow a(test_config("Ege", sink.local_port()), ta, ca);
    LobbyFlow b(test_config("Ada", sink.local_port()), tb, cb);

    a.handle_server_message(lobby_created("9V9BHE", 0));
    b.handle_server_message(join_accepted(1));
    a.handle_server_message(peer_candidates(1, "127.0.0.1:" + std::to_string(tb.local_port())));
    b.handle_server_message(
        peer_candidates(0, "127.0.0.1:" + std::to_string(black_hole.local_port())));
    a.handle_server_message(start_match(0xC0FFEEu, 0b01));
    b.handle_server_message(start_match(0xC0FFEEu, 0b10));

    // The test plays matchmaker: it answers each AllocateRelay the flow decides
    // to send, exactly as handleAllocateRelay does — only to the sender, with no
    // push telling the other seat anything (PROTOCOL.md is frozen; the fix is
    // client-side or it does not exist).
    FakeRelay forwarder(relay_sock);
    const std::string relay_addr = "127.0.0.1:" + std::to_string(relay_sock.local_port());
    bool a_allocated = false;
    bool b_allocated = false;

    std::int64_t now = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(40);
    while ((a.phase() != LobbyFlow::Phase::Ready || b.phase() != LobbyFlow::Phase::Ready) &&
           a.phase() != LobbyFlow::Phase::Failed && b.phase() != LobbyFlow::Phase::Failed &&
           std::chrono::steady_clock::now() < deadline) {
        a.step(now);
        b.step(now);
        if (!a_allocated && a.phase() == LobbyFlow::Phase::Relaying) {
            a_allocated = true;
            a.handle_server_message(relay_allocated(relay_addr, forwarder.allocate(0)));
        }
        if (!b_allocated && b.phase() == LobbyFlow::Phase::Relaying) {
            b_allocated = true;
            b.handle_server_message(relay_allocated(relay_addr, forwarder.allocate(1)));
        }
        forwarder.pump();
        now += 20;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    INFO("a=" << static_cast<int>(a.phase()) << " '" << a.error() << "' b="
              << static_cast<int>(b.phase()) << " '" << b.error() << "' at t=" << now);
    REQUIRE(a.phase() == LobbyFlow::Phase::Ready);
    REQUIRE(b.phase() == LobbyFlow::Phase::Ready);

    // Both, not one: converging means the peer that WON its punch followed the
    // one that could not, because the relay is the only path neither ever leaves.
    CHECK(a.is_relayed());
    CHECK(b.is_relayed());
    // And the failure really was reproduced on the way: datagrams aimed at a seat
    // that had not allocated yet were dropped, precisely as in production. The
    // difference is that it no longer stays that way.
    CHECK(forwarder.drop_unknown_dst() > 0);

    // The assertion the old code could never have satisfied: a datagram put in at
    // one end comes out at the other. A match that "connects" and carries nothing
    // is what the logs recorded.
    const std::vector<std::uint8_t> payload = {0xBE, 0xEF};
    std::vector<std::uint8_t> got;
    bool crossed = false;
    const auto d2 = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!crossed && std::chrono::steady_clock::now() < d2) {
        a.transport().send(payload.data(), payload.size());
        forwarder.pump();
        while (b.transport().poll(&got))
            if (got == payload) crossed = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(crossed);
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
    while ((a.phase() != LobbyFlow::Phase::Ready || b.phase() != LobbyFlow::Phase::Ready) &&
           a.phase() != LobbyFlow::Phase::Failed && b.phase() != LobbyFlow::Phase::Failed &&
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
