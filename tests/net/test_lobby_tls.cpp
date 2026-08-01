// The lobby control plane over TLS (SECURITY.md S1). Two properties, and they
// are different properties: that a wss:// URL CONNECTS, and that a certificate
// which does not verify is REFUSED. A backend that compiles proves neither, and
// a backend that connects while trusting anything is worse than no TLS at all —
// it looks encrypted and authenticates nobody.
//
// TWO cases are offline and always run. One pins the compile-time contract that
// a wss:// URL is never quietly downgraded to a cleartext socket. The other is
// the certificate rejection itself, against a self-signed listener this suite
// stands up on loopback — it used to be reachable only over the internet, so it
// was dormant on every developer machine and every CI run.
//
// The two remaining cases need the internet, so they are OPT-IN behind
// BOMBER_TLS_LIVE=1 and never block the pre-push gate:
//
//   set BOMBER_TLS_LIVE=1
//   ctest -R net_lobby_tls --output-on-failure
//
// Both endpoints are overridable — BOMBER_TLS_GOOD_URL (default: the deployed
// matchmaker) and BOMBER_TLS_BAD_URL (default: badssl.com's untrusted-root
// host, the standard public fixture for exactly this check).
//
// THE `live:` PREFIX IS LOAD-BEARING. ctest registers this one binary TWICE,
// filtered on that prefix (tests/net/CMakeLists.txt), because a single
// registration cannot report the truth: the offline cases really run and the
// live ones really do not, and one status word covering both is wrong whichever
// word it picks.
// `net_lobby_tls` excludes the prefix and must always PASS; `net_lobby_tls_live`
// selects it and SKIPs on the LOBBY_TLS_SKIP marker. Name a new case
// accordingly, and note that a name filter matching NOTHING is a silent pass —
// which is why the no-TLS build below still declares a `live:` case rather than
// leaving the second registration with an empty selection.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "bomber/net/build_hash.hpp"
#include "bomber/net/lobby_client.hpp"

#if defined(BOMBER_HAS_LOBBY_TLS)
#include <ixwebsocket/IXWebSocketServer.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>
#endif

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

#if defined(BOMBER_HAS_LOBBY_TLS)

// A throwaway self-signed EC certificate and its key, written to `dir` as PEM.
//
// GENERATED, never committed. A checked-in test key is a private key in a public
// repository — it trips secret scanners, reads as a leak to anyone skimming the
// tree, and would eventually expire and rot the suite. mbedTLS is already linked
// (it is the TLS backend under test), so the fixture costs one keygen at
// run time and the certificate's validity window can simply be absurd.
//
// Self-signed is the whole point: no trust store on any machine contains this
// anchor, so a client that verifies its peer MUST refuse it, and one that does
// not will happily connect.
bool write_selfsigned(const std::filesystem::path& cert_path,
                      const std::filesystem::path& key_path) {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_pk_context key;
    mbedtls_x509write_cert crt;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&crt);

    unsigned char pem[8192];
    unsigned char serial[] = {0x01};
    static const char kSeed[] = "bomber-offline-tls-fixture";
    bool ok = false;
    const auto write_pem = [](const std::filesystem::path& p, const unsigned char* data) {
        std::ofstream out(p, std::ios::binary);
        out << reinterpret_cast<const char*>(data);
        return out.good();
    };

    // One do/while so every mbedTLS handle is freed on any failure path; the C
    // API has no RAII and an early return here would leak the DRBG's entropy
    // source.
    do {
        if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                  reinterpret_cast<const unsigned char*>(kSeed),
                                  sizeof(kSeed) - 1) != 0)
            break;
        if (mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0) break;
        if (mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key),
                                mbedtls_ctr_drbg_random, &drbg) != 0)
            break;
        if (mbedtls_pk_write_key_pem(&key, pem, sizeof(pem)) != 0) break;
        if (!write_pem(key_path, pem)) break;

        mbedtls_x509write_crt_set_subject_key(&crt, &key);
        mbedtls_x509write_crt_set_issuer_key(&crt, &key);  // self-signed
        if (mbedtls_x509write_crt_set_subject_name(&crt, "CN=127.0.0.1") != 0) break;
        if (mbedtls_x509write_crt_set_issuer_name(&crt, "CN=127.0.0.1") != 0) break;
        mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
        mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
        if (mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial)) != 0) break;
        // Wide enough that this fixture cannot expire and turn a security check
        // red for a reason that has nothing to do with security.
        if (mbedtls_x509write_crt_set_validity(&crt, "20200101000000", "20991231235959") != 0)
            break;
        if (mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1) != 0) break;
        if (mbedtls_x509write_crt_pem(&crt, pem, sizeof(pem), mbedtls_ctr_drbg_random, &drbg) != 0)
            break;
        ok = write_pem(cert_path, pem);
    } while (false);

    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&key);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    return ok;
}

#endif  // BOMBER_HAS_LOBBY_TLS

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

TEST_CASE("an untrusted certificate from a local listener is refused") {
    // THE SECURITY CHECK, RUN OFFLINE. Its live sibling below is the same
    // property against badssl.com, and it was dormant on every developer machine
    // and every CI run — a green security test that never executed, which is
    // worse than an absent one because it occupies the space where someone would
    // notice the absence.
    //
    // Nothing here needs the internet: mbedTLS serves a self-signed certificate
    // on loopback, and no trust store anywhere contains that anchor. A client
    // with peer verification REQUIRED must refuse it. A client that trusts
    // anything (caFile "NONE", mbedTLS authmode NONE) opens the socket instead,
    // and that is the regression this case exists to make impossible to ship.
    //
    // WHAT IT DOES NOT COVER, and cannot offline: the OTHER verification switch,
    // hostname validation. Isolating that needs a chain that verifies but
    // carries the wrong name, which means the client trusting a test CA — and
    // find_ca_bundle() reads SSL_CERT_FILE only off Windows, answering "SYSTEM"
    // (the machine's own store) here. So the wrong.host half stays with the live
    // cases; this one proves the chain is checked at all.
    //
    // Failure to listen is a FAILURE, not a soft return. This suite's whole
    // history is a security check quietly not running.
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "bomber_offline_tls";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path cert = dir / "selfsigned.crt.pem";
    const fs::path key = dir / "selfsigned.key.pem";
    REQUIRE(write_selfsigned(cert, key));

    // WSAStartup. LobbyClient's ctor does this for its own sockets, so running
    // this case AFTER another one hid the omission and running it alone (a
    // `--test-case=` filter, which is how the split registration works) failed
    // every bind with WSANOTINITIALISED. Order dependence in a security check is
    // its own defect.
    ix::initNetSystem();

    // The server binds a fixed port, so try a small range: a stale listener or a
    // parallel ctest job must not decide a security check's verdict.
    // ix::SocketServer never reports back an ephemeral port (it keeps the port it
    // was constructed with), so port 0 is not an option.
    std::unique_ptr<ix::WebSocketServer> server;
    std::string listen_error;
    int port = 0;
    for (int candidate = 45081; candidate <= 45095 && !server; ++candidate) {
        auto s = std::make_unique<ix::WebSocketServer>(candidate, "127.0.0.1");
        ix::SocketTLSOptions opts;
        opts.tls = true;
        opts.certFile = cert.string();
        opts.keyFile = key.string();
        opts.caFile = "NONE";  // server side: do not ask the CLIENT for a certificate
        s->setTLSOptions(opts);
        const auto [ok, err] = s->listen();
        if (ok) {
            port = candidate;
            server = std::move(s);
        } else {
            listen_error = err;
        }
    }
    INFO("last listen error: " << listen_error);
    REQUIRE(server != nullptr);
    // A server that completes the WebSocket upgrade, not just the TCP accept.
    // Without it the client fails one layer up ("failed reading HTTP status
    // line") even when it accepted the certificate, and CHECK_FALSE(opened)
    // below would stay green with peer verification switched off — leaving the
    // whole security property resting on the error-string assertion alone.
    server->setOnClientMessageCallback([](const std::shared_ptr<ix::ConnectionState>&,
                                          ix::WebSocket&, const ix::WebSocketMessagePtr&) {});
    server->start();

    LobbyClient c;
    const std::string url = "wss://127.0.0.1:" + std::to_string(port) + "/ws";
    REQUIRE(c.connect(url));
    // Wait for EITHER outcome, not just for the open to time out: a loopback
    // handshake settles in milliseconds, and blocking the full budget on the
    // expected result would put ten seconds into every pre-push gate.
    wait_until([&] { return c.is_open() || !c.last_error().empty(); }, 10000);
    const bool opened = c.is_open();
    const std::string err = c.last_error();
    INFO("url=" << url << " err='" << err << "'");
    CHECK_FALSE(opened);
    // Asserting on the REASON is what separates "verification worked" from "the
    // connection failed for some other reason" — a refused port would also
    // produce "did not open" and would prove nothing. mbedTLS says
    // "X509 - Certificate verification failed …"; accept either spelling so an
    // OpenSSL or SecureTransport build still fits.
    CHECK((err.find("X509") != std::string::npos || err.find("ertificate") != std::string::npos));

    c.close();
    server->stop();
    fs::remove_all(dir, ec);
}

TEST_CASE("live: wss:// to the deployed matchmaker completes a verified handshake") {
    if (!live_enabled()) {
        MESSAGE(
            "LOBBY_TLS_SKIP: BOMBER_TLS_LIVE unset; the live handshake case asserted "
            "nothing");
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
        MESSAGE(
            "LOBBY_TLS_SKIP: BOMBER_TLS_LIVE unset; the live certificate-rejection case "
            "asserted nothing");
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

    int endpoints_checked = 0;
    for (const std::string& u : {url, wrong_host_url}) {
        if (u.empty()) continue;  // an override can blank one endpoint out
        ++endpoints_checked;
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
    // BOMBER_TLS_LIVE set but BOTH overrides blanked out skipped every endpoint
    // and passed — this suite's own "green, having verified nothing" shape, one
    // level below the build switch that produced it the first time.
    CHECK(endpoints_checked > 0);
}

#else  // !BOMBER_HAS_LOBBY_TLS

TEST_CASE("live: TLS verification cases are absent from this build") {
    // Without this, `--test-case=live:*` selects nothing and doctest reports a
    // clean pass over zero cases — the same "green having verified nothing"
    // shape one level down from the build switch that produced it originally.
    MESSAGE(
        "LOBBY_TLS_SKIP: built with BOMBER_LOBBY_TLS=OFF; the certificate- and "
        "hostname-rejection cases are preprocessed away, not skipped");
}

#endif  // BOMBER_HAS_LOBBY_TLS
