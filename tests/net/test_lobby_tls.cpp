// The lobby control plane over TLS (SECURITY.md S1). Two properties, and they
// are different properties: that a wss:// URL CONNECTS, and that a certificate
// which does not verify is REFUSED. A backend that compiles proves neither, and
// a backend that connects while trusting anything is worse than no TLS at all —
// it looks encrypted and authenticates nobody.
//
// Case 1 is offline and always runs: it pins the compile-time contract that a
// wss:// URL is never quietly downgraded to a cleartext socket.
//
// Cases 2 and 3 need the internet, so they are OPT-IN behind BOMBER_TLS_LIVE=1
// and never block the pre-push gate:
//
//   set BOMBER_TLS_LIVE=1
//   ctest -R net_lobby_tls --output-on-failure
//
// Both endpoints are overridable — BOMBER_TLS_GOOD_URL (default: the deployed
// matchmaker) and BOMBER_TLS_BAD_URL (default: badssl.com's untrusted-root
// host, the standard public fixture for exactly this check).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

#include "bomber/net/build_hash.hpp"
#include "bomber/net/lobby_client.hpp"

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

bool live_enabled() {
    return !env_or("BOMBER_TLS_LIVE", "").empty();
}

// Spin until `pred` holds or the budget runs out. Nothing is drained here — the
// WebSocket delivers on its own thread and poll() is destructive, so a caller
// that cares about frames does its own poll inside `pred`.
template <typename Pred>
bool wait_until(Pred pred, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

}  // namespace

TEST_CASE("a wss:// URL is never silently downgraded to cleartext") {
    // The security property that holds with NO network, which is why this case
    // dials loopback rather than the deployed server: asking for wss:// on a
    // build with no TLS backend must fail loudly. A client that answered a
    // wss:// URL with a plaintext socket would hand the host_token to the path
    // while the player believes the link is encrypted.
    // "WSS://" is the same scheme (RFC 3986 §3.1) and must not slip past into
    // IXWebSocket's plaintext path, which compares `protocol == "wss"` literally.
    for (const std::string url : {"wss://127.0.0.1:1/ws", "WSS://127.0.0.1:1/ws"}) {
        LobbyClient c;
        const bool accepted = c.connect(url);
        INFO("url=" << url << " err='" << c.last_error() << "'");
#if defined(BOMBER_HAS_LOBBY_TLS)
        CHECK(accepted);  // a TLS build takes it (whether it connects is case 2's job)
        c.close();
#else
        CHECK_FALSE(accepted);
        CHECK(c.last_error().find("TLS") != std::string::npos);
#endif
    }
}

#if defined(BOMBER_HAS_LOBBY_TLS)

TEST_CASE("live: wss:// to the deployed matchmaker completes a verified handshake") {
    if (!live_enabled()) {
        MESSAGE("BOMBER_TLS_LIVE unset; skipping the live TLS handshake test");
        return;
    }
    const std::string url =
        env_or("BOMBER_TLS_GOOD_URL", "wss://open-bomberman-matchmaker.fly.dev/ws");
    REQUIRE(url.rfind("wss://", 0) == 0);

    LobbyClient c;
    REQUIRE(c.connect(url));
    const bool opened = wait_until([&] { return c.is_open(); }, 15000);
    INFO("url=" << url << " err='" << c.last_error() << "'");
    REQUIRE(opened);

    // Open is the TLS + WebSocket-upgrade proof. Carry one real control-plane
    // round trip on top of it, so the assertion is "the encrypted channel works
    // for what we use it for", not merely "a socket opened". ListPublic is the
    // one request that needs no lobby and leaves nothing behind on the server.
    bool answered = false;
    c.list_public(build_hash());
    const bool got = wait_until(
        [&] {
            c.poll_messages([&](const LobbyServerMessage& m) {
                if (m.type == LobbyMsgType::PublicList) answered = true;
            });
            return answered;
        },
        15000);
    CHECK(got);
    c.close();
}

TEST_CASE("live: a certificate that does not verify is refused") {
    if (!live_enabled()) {
        MESSAGE("BOMBER_TLS_LIVE unset; skipping the certificate-rejection test");
        return;
    }
    // The counter-proof for the test above. Without it, "it connected" is also
    // what a build with peer verification switched off looks like. Two distinct
    // failures, because they exercise two distinct switches:
    //   untrusted-root — the chain does not reach a trusted anchor (authmode)
    //   wrong.host     — a perfectly valid certificate for another name (SNI /
    //                    hostname validation, the half that is easiest to leave
    //                    off without noticing)
    // Asserting on the REASON matters: a DNS or routing failure would also
    // produce "did not open", and would prove nothing about verification.
    const std::string url = env_or("BOMBER_TLS_BAD_URL", "wss://untrusted-root.badssl.com/");
    const std::string wrong_host_url =
        env_or("BOMBER_TLS_WRONGHOST_URL", "wss://wrong.host.badssl.com/");

    for (const std::string& u : {url, wrong_host_url}) {
        if (u.empty()) continue;  // an override can blank one endpoint out
        LobbyClient c;
        REQUIRE(c.connect(u));
        // It must never open, and it must say why. Automatic reconnection is
        // off, so one failed handshake is the whole story; the wait is only for
        // it to happen.
        const bool opened = wait_until([&] { return c.is_open(); }, 15000);
        const std::string err = c.last_error();
        INFO("url=" << u << " err='" << err << "'");
        CHECK_FALSE(opened);
        // mbedTLS reports both as "X509 - Certificate verification failed …";
        // accept either spelling so an OpenSSL/SecureTransport build still fits.
        const bool blamed_the_certificate =
            err.find("X509") != std::string::npos || err.find("ertificate") != std::string::npos;
        CHECK(blamed_the_certificate);
        c.close();
    }
}

#endif  // BOMBER_HAS_LOBBY_TLS
