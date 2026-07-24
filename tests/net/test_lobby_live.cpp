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
        MESSAGE("BOMBER_MATCHMAKER_URL unset; skipping the live matchmaker test");
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
    cfg.stun_host = "127.0.0.1";
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
