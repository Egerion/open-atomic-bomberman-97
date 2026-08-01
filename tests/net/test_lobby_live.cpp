// LIVE interop test: the C++ lobby stack against the REAL Go matchmaker
// (services/matchmaker). This is the one check that proves the two independently
// written sides of PROTOCOL.md actually agree on the wire — every other suite
// tests one side against a fake.
//
// OPT-IN: skips unless BOMBER_MATCHMAKER_URL is set, so it never blocks the
// pre-push gate. Run it with a server up:
//
//   go build -o mm.exe ./services/matchmaker && ./mm.exe &
//   set BOMBER_MATCHMAKER_URL=ws://127.0.0.1:8080/ws
//   ctest -R net_lobby_live --output-on-failure
//
// BOMBER_MATCHMAKER_STUN_PORT overrides the UDP STUN port (default 8081).
//
// doctest has NO runtime skip, so an early return prints the same word as a full
// pass and every one of the four cases below took it on every machine: ctest said
// PASSED for a suite that had executed nothing, which is precisely the shape this
// repo has been bitten by five times. LOBBY_LIVE_SKIP is the marker
// tests/net/CMakeLists.txt hands to SKIP_REGULAR_EXPRESSION; all four cases gate
// on the same variable, so the suite is all-or-nothing and one ctest status can
// tell the truth about it.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>

#include "bomber/net/build_hash.hpp"
#include "bomber/net/lobby_client.hpp"
#include "bomber/net/lobby_flow.hpp"
#include "bomber/net/udp_transport.hpp"

using namespace bomber::net;  // NOLINT(google-build-using-namespace) — test-local

namespace {

std::string env_or(const char* name, const std::string& fallback) {
#ifdef _MSC_VER
    char* buf = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&buf, &len, name) == 0 && buf != nullptr) {
        std::string v(buf);
        std::free(buf);
        return v;
    }
    return fallback;
#else
    const char* v = std::getenv(name);
    return v != nullptr ? std::string(v) : fallback;
#endif
}

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// The STUN echo lives on the SAME host as the WebSocket (PROTOCOL.md §2), so
// derive it from the URL rather than assuming loopback — hard-coding 127.0.0.1
// silently pointed the probe at nothing whenever the tests ran against a
// deployed matchmaker, which is exactly the case worth exercising.
// BOMBER_MATCHMAKER_STUN_HOST still overrides.
std::string stun_host_from(const std::string& ws_url) {
    const std::string env = env_or("BOMBER_MATCHMAKER_STUN_HOST", "");
    if (!env.empty()) return env;
    std::size_t start = ws_url.find("://");
    start = (start == std::string::npos) ? 0 : start + 3;
    const std::size_t end = ws_url.find_first_of(":/", start);
    const std::string host = ws_url.substr(start, end == std::string::npos ? end : end - start);
    return host.empty() ? std::string("127.0.0.1") : host;
}

// Pump both flows until `pred` holds or `timeout_ms` of real time elapses.
template <typename Pred>
bool pump_until(LobbyFlow& a, LobbyFlow& b, Pred pred, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        a.step(now_ms());
        b.step(now_ms());
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

}  // namespace

TEST_CASE("live: host + join + ready + start against the real matchmaker") {
    const std::string url = env_or("BOMBER_MATCHMAKER_URL", "");
    if (url.empty()) {
        MESSAGE(
            "LOBBY_LIVE_SKIP: BOMBER_MATCHMAKER_URL unset; the live matchmaker case "
            "asserted nothing");
        return;
    }
    const auto stun_port =
        static_cast<std::uint16_t>(std::atoi(env_or("BOMBER_MATCHMAKER_STUN_PORT", "8081").c_str()));

    UdpTransport ta;
    UdpTransport tb;
    REQUIRE(ta.bind(0));
    REQUIRE(tb.bind(0));

    // Both peers must present the SAME build_hash or the server rejects the join
    // (the cross-build door, ADR-0011) — here they are literally one binary.
    LobbyFlow::Config cfg;
    cfg.server_url = url;
    cfg.stun_host = stun_host_from(url);
    cfg.stun_port = stun_port;
    cfg.build_hash = build_hash();

    LobbyFlow::Config cfg_a = cfg;
    cfg_a.player_name = "EGE";
    LobbyFlow::Config cfg_b = cfg;
    cfg_b.player_name = "ADA";

    LobbyClient ca;
    LobbyClient cb;
    LobbyFlow a(cfg_a, ta, ca);
    LobbyFlow b(cfg_b, tb, cb);

    // 1. Host a private 2-seat lobby and get a shareable code back.
    a.host_lobby("interop", /*is_public=*/false, /*max_seats=*/2);
    REQUIRE(pump_until(a, b, [&] { return a.phase() == LobbyFlow::Phase::InLobby; }, 8000));
    REQUIRE(a.code().size() == 6);  // 6-char Crockford base-32
    CHECK(a.is_host());
    CHECK(a.my_seat() == 0);
    MESSAGE("lobby code: " << a.code());

    // 2. Join by that code — the guest lands in the next free seat.
    b.join_lobby(a.code());
    REQUIRE(pump_until(a, b, [&] { return b.phase() == LobbyFlow::Phase::InLobby; }, 8000));
    CHECK(b.my_seat() == 1);
    CHECK_FALSE(b.is_host());

    // 3. The host sees the guest arrive on its roster (server push).
    REQUIRE(pump_until(a, b, [&] { return a.roster().size() == 2; }, 8000));

    // 4. Both ready up; the server broadcasts the roster change to both.
    a.set_ready(true);
    b.set_ready(true);
    REQUIRE(pump_until(
        a, b,
        [&] {
            if (a.roster().size() < 2 || b.roster().size() < 2) return false;
            for (const auto& e : a.roster())
                if (!e.ready) return false;
            return true;
        },
        8000));

    // 5. START: the server validates all-ready + equal build_hash, mints the
    //    seed, and broadcasts it; both peers then punch a direct path.
    a.start_match();
    const bool both_ready = pump_until(
        a, b,
        [&] { return a.phase() == LobbyFlow::Phase::Ready && b.phase() == LobbyFlow::Phase::Ready; },
        15000);
    INFO("a=" << static_cast<int>(a.phase()) << " err='" << a.error() << "'  b="
              << static_cast<int>(b.phase()) << " err='" << b.error() << "'");
    REQUIRE(both_ready);

    // Identical parity payload, per-peer seat ownership — exactly what the
    // deterministic sim needs to start byte-identical on both machines.
    CHECK(a.match_start().seed == b.match_start().seed);
    CHECK(a.match_start().seed != 0u);
    CHECK(a.match_start().input_delay == b.match_start().input_delay);
    CHECK(a.match_start().local_seats_mask == 0b01);
    CHECK(b.match_start().local_seats_mask == 0b10);
    MESSAGE("seed=" << a.match_start().seed << " rtt_a=" << a.rtt_ms() << "ms");
}

TEST_CASE("live: a public lobby shows up in another client's browse") {
    // Phase 3: a PUBLIC lobby is visible to everyone, so a player can find a
    // match without being handed a code. Browsing must not commit the browser to
    // anything — it drops back to Idle so the row can then be joined.
    const std::string url = env_or("BOMBER_MATCHMAKER_URL", "");
    if (url.empty()) {
        MESSAGE(
            "LOBBY_LIVE_SKIP: BOMBER_MATCHMAKER_URL unset; the live browse case "
            "asserted nothing");
        return;
    }

    UdpTransport th;
    UdpTransport tbrowse;
    REQUIRE(th.bind(0));
    REQUIRE(tbrowse.bind(0));

    LobbyFlow::Config cfg;
    cfg.server_url = url;
    cfg.stun_host = stun_host_from(url);
    cfg.stun_port =
        static_cast<std::uint16_t>(std::atoi(env_or("BOMBER_MATCHMAKER_STUN_PORT", "8081").c_str()));
    cfg.build_hash = build_hash();
    LobbyFlow::Config cfg_host = cfg;
    cfg_host.player_name = "EGE";
    LobbyFlow::Config cfg_browser = cfg;
    cfg_browser.player_name = "GUEST";

    LobbyClient ch;
    LobbyClient cbr;
    LobbyFlow host(cfg_host, th, ch);
    LobbyFlow browser(cfg_browser, tbrowse, cbr);

    host.host_lobby("PUBLIC TEST", /*is_public=*/true, /*max_seats=*/4);
    REQUIRE(pump_until(host, browser, [&] { return host.phase() == LobbyFlow::Phase::InLobby; },
                       8000));
    const std::string code = host.code();
    REQUIRE(code.size() == 6);

    const unsigned before = browser.public_list_revision();
    browser.browse_public();
    REQUIRE(pump_until(host, browser,
                       [&] { return browser.public_list_revision() != before; }, 8000));

    // Browsing is not a commitment — the browser is free to act again.
    CHECK(browser.phase() == LobbyFlow::Phase::Idle);

    bool found = false;
    for (const PublicLobby& l : browser.public_lobbies()) {
        if (l.code == code) {
            found = true;
            CHECK(l.name == "PUBLIC TEST");
            CHECK(l.max == 4);
            CHECK(l.players >= 1);
            CHECK(l.build_ok);  // same binary, so the build door is open
        }
    }
    CHECK(found);
    MESSAGE("public list carried " << browser.public_lobbies().size() << " lobby(ies); ours="
                                   << code);
}

TEST_CASE("live: lobby chat round-trips through the real matchmaker") {
    // PORT-ONLY lobby chat (PROTOCOL.md §7) against the REAL Go relay — the one
    // check that proves the two independently-written sides agree on the Chat
    // frame. NOTE: this needs a server built from THIS tree; the deployed
    // instance answers `unknown_type` until it is redeployed.
    const std::string url = env_or("BOMBER_MATCHMAKER_URL", "");
    if (url.empty()) {
        MESSAGE(
            "LOBBY_LIVE_SKIP: BOMBER_MATCHMAKER_URL unset; the live chat case "
            "asserted nothing");
        return;
    }
    const auto stun_port =
        static_cast<std::uint16_t>(std::atoi(env_or("BOMBER_MATCHMAKER_STUN_PORT", "8081").c_str()));

    UdpTransport ta;
    UdpTransport tb;
    REQUIRE(ta.bind(0));
    REQUIRE(tb.bind(0));

    LobbyFlow::Config cfg;
    cfg.server_url = url;
    cfg.stun_host = stun_host_from(url);
    cfg.stun_port = stun_port;
    cfg.build_hash = build_hash();
    LobbyFlow::Config cfg_a = cfg;
    cfg_a.player_name = "EGE";
    LobbyFlow::Config cfg_b = cfg;
    cfg_b.player_name = "ADA";

    LobbyClient ca;
    LobbyClient cb;
    LobbyFlow a(cfg_a, ta, ca);
    LobbyFlow b(cfg_b, tb, cb);

    a.host_lobby("chat-test", /*is_public=*/false, /*max_seats=*/2);
    REQUIRE(pump_until(a, b, [&] { return a.phase() == LobbyFlow::Phase::InLobby; }, 8000));
    b.join_lobby(a.code());
    REQUIRE(pump_until(a, b, [&] { return b.phase() == LobbyFlow::Phase::InLobby; }, 8000));
    REQUIRE(pump_until(a, b, [&] { return a.roster().size() == 2; }, 8000));

    // The guest speaks; BOTH sides must hear it, the sender included (the
    // server echoes, so everyone holds one identically-ordered transcript).
    REQUIRE(b.send_chat("gl hf", now_ms()));
    REQUIRE(pump_until(
        a, b, [&] { return !a.chat_log().empty() && !b.chat_log().empty(); }, 8000));
    CHECK(a.chat_log().back().text == "gl hf");
    CHECK(b.chat_log().back().text == "gl hf");
    // Attribution is the SERVER's, off its roster — the guest's own seat 1 and
    // the node name it joined with.
    CHECK(a.chat_log().back().seat == 1);
    CHECK(a.chat_log().back().name == "ADA");

    // And back the other way, so the host's seat/name are checked too.
    REQUIRE(a.send_chat("have fun", now_ms()));
    REQUIRE(pump_until(a, b, [&] { return b.chat_log().size() == 2; }, 8000));
    CHECK(b.chat_log().back().text == "have fun");
    CHECK(b.chat_log().back().seat == 0);
    CHECK(b.chat_log().back().name == "EGE");
}

TEST_CASE("live: relay fallback carries the match when the punch cannot land") {
    // The Phase 2 proof (ADR-0011 decision 3): force the hole punch to fail —
    // exactly what symmetric NAT / CGNAT does — and check both peers fall back
    // through the server's UDP forwarder and can still exchange datagrams.
    //
    // The server must advertise a ROUTABLE relay host for a client to use it;
    // its default (`-relay-addr :8082`) has no host and is rejected here, which
    // is the same thing its own start-up WARN is about. Run it with
    // `-relay-advertise 127.0.0.1:8082` for this test.
    const std::string url = env_or("BOMBER_MATCHMAKER_URL", "");
    if (url.empty()) {
        MESSAGE(
            "LOBBY_LIVE_SKIP: BOMBER_MATCHMAKER_URL unset; the live relay case "
            "asserted nothing");
        return;
    }
    const auto stun_port =
        static_cast<std::uint16_t>(std::atoi(env_or("BOMBER_MATCHMAKER_STUN_PORT", "8081").c_str()));

    UdpTransport ta;
    UdpTransport tb;
    REQUIRE(ta.bind(0));
    REQUIRE(tb.bind(0));

    LobbyFlow::Config cfg;
    cfg.server_url = url;
    cfg.stun_host = stun_host_from(url);
    cfg.stun_port = stun_port;
    cfg.build_hash = build_hash();
    LobbyFlow::Config cfg_a = cfg;
    cfg_a.player_name = "EGE";
    LobbyFlow::Config cfg_b = cfg;
    cfg_b.player_name = "ADA";

    LobbyClient ca;
    LobbyClient cb;
    LobbyFlow a(cfg_a, ta, ca);
    LobbyFlow b(cfg_b, tb, cb);

    a.host_lobby("relay-test", false, 2);
    REQUIRE(pump_until(a, b, [&] { return a.phase() == LobbyFlow::Phase::InLobby; }, 8000));
    b.join_lobby(a.code());
    REQUIRE(pump_until(a, b, [&] { return b.phase() == LobbyFlow::Phase::InLobby; }, 8000));
    REQUIRE(pump_until(a, b, [&] { return a.roster().size() == 2; }, 8000));

    a.set_ready(true);
    b.set_ready(true);
    REQUIRE(pump_until(
        a, b,
        [&] {
            if (a.roster().size() < 2) return false;
            for (const auto& e : a.roster())
                if (!e.ready) return false;
            return true;
        },
        8000));
    // Let the real candidate fan-out settle first, so our sabotage below is not
    // overwritten by a late PeerCandidates push from the server.
    pump_until(a, b, [] { return false; }, 1500);

    // Sabotage: replace each peer's view of the other with TEST-NET-1
    // (192.0.2.0/24, RFC 5737 — guaranteed unroutable), so no candidate pair can
    // ever complete and the punch must time out.
    LobbyServerMessage bogus_for_a;
    bogus_for_a.type = LobbyMsgType::PeerCandidates;
    bogus_for_a.candidates_seat = 1;
    bogus_for_a.candidates = {LobbyCandidate{"host", "192.0.2.1:9", ""}};
    LobbyServerMessage bogus_for_b = bogus_for_a;
    bogus_for_b.candidates_seat = 0;
    a.handle_server_message(bogus_for_a);
    b.handle_server_message(bogus_for_b);

    a.start_match();
    // Punch timeout (~5 s) + the allocation round trip.
    const bool both = pump_until(
        a, b,
        [&] { return a.phase() == LobbyFlow::Phase::Ready && b.phase() == LobbyFlow::Phase::Ready; },
        30000);
    INFO("a=" << static_cast<int>(a.phase()) << " err='" << a.error() << "'  b="
              << static_cast<int>(b.phase()) << " err='" << b.error() << "'");
    REQUIRE(both);

    // Both got there through the RELAY, not a direct path.
    CHECK(a.is_relayed());
    CHECK(b.is_relayed());

    // And the relayed transport actually carries traffic. The forwarder learns
    // each peer's address from its first datagram, so both sides send until one
    // lands (the game's netcode is likewise loss-tolerant).
    const std::vector<std::uint8_t> payload = {0x42, 0x4F, 0x4D, 0x42};
    bool delivered = false;
    std::vector<std::uint8_t> got;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!delivered && std::chrono::steady_clock::now() < deadline) {
        a.transport().send(payload.data(), payload.size());
        b.transport().send(payload.data(), payload.size());  // teaches the relay b's address
        if (b.transport().poll(&got) && got == payload) delivered = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(delivered);
    MESSAGE("relayed match ready; seed=" << a.match_start().seed);
}
