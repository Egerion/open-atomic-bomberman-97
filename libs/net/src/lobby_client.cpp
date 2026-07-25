#include "bomber/net/lobby_client.hpp"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

#include <atomic>
#include <cctype>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>

#if !defined(_WIN32)
#include <cstdlib>
#include <fstream>
#endif

namespace bomber::net {
namespace {

// ix::initNetSystem() does WSAStartup on Windows (a no-op elsewhere). Call it
// exactly once per process even with several LobbyClients live.
std::once_flag g_net_init;
void ensure_net_system() {
    std::call_once(g_net_init, [] { ix::initNetSystem(); });
}

// Lower-case the scheme. URI schemes are case-insensitive (RFC 3986 §3.1) but
// IXWebSocket's are not: it decides TLS with `protocol == "wss"`, so a URL typed
// as "WSS://…" would take the PLAINTEXT socket path. It would then fail rather
// than leak (a TLS server does not answer a cleartext upgrade), but the check
// below must not disagree with the one downstream, so both see the same string.
std::string normalize_scheme(const std::string& url) {
    const std::size_t sep = url.find("://");
    if (sep == std::string::npos) return url;
    std::string out = url;
    for (std::size_t i = 0; i < sep; ++i) {
        out[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(out[i])));
    }
    return out;
}

bool is_secure_url(const std::string& url) {
    return url.rfind("wss://", 0) == 0;
}

#if defined(BOMBER_HAS_LOBBY_TLS)
// Where the trust anchors come from.
//
// Windows: IXWebSocket's mbedTLS backend answers caFile=="SYSTEM" by
// enumerating the CurrentUser\Root certificate store through wincrypt, so we
// verify against the machine's own trust decisions and inherit whatever the
// admin (or corporate policy) has put there. Nothing to configure here.
//
// Elsewhere mbedTLS has no system-store hook at all — IXWebSocket's
// loadSystemCertificates() is a bare `return false` off Windows, which would
// fail EVERY wss:// handshake with an empty reason. So name the platform CA
// bundle explicitly. SSL_CERT_FILE (the OpenSSL/curl convention) wins, then the
// usual distro locations; macOS ships the keychain roots at /etc/ssl/cert.pem.
// An empty result means "no trust store found" and connect() refuses rather
// than falling back to an unverified session.
std::string find_ca_bundle() {
#if defined(_WIN32)
    return "SYSTEM";
#else
    if (const char* env = std::getenv("SSL_CERT_FILE"); env != nullptr && *env != '\0') {
        return env;
    }
    for (const char* path : {"/etc/ssl/certs/ca-certificates.crt",  // Debian/Ubuntu/Alpine
                             "/etc/pki/tls/certs/ca-bundle.crt",    // Fedora/RHEL
                             "/etc/ssl/ca-bundle.pem",              // openSUSE
                             "/etc/ssl/cert.pem",                   // macOS/FreeBSD
                             "/usr/local/etc/openssl/cert.pem"}) {  // Homebrew
        if (std::ifstream(path).good()) return path;
    }
    return {};
#endif
}
#endif  // BOMBER_HAS_LOBBY_TLS

}  // namespace

struct LobbyClient::Impl {
    ix::WebSocket ws;
    std::mutex mu;                 // guards inbox + last_error (WS thread writes)
    std::deque<std::string> inbox;  // inbound JSON frames, drained by poll()
    std::string last_error;
    std::atomic<bool> open{false};
};

LobbyClient::LobbyClient() : impl_(std::make_unique<Impl>()) {
    ensure_net_system();
    // The WS thread only enqueues; the game thread drains in poll(). Keeping the
    // callback tiny (no user code) avoids running lobby logic off-thread.
    impl_->ws.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
        switch (msg->type) {
            case ix::WebSocketMessageType::Open:
                impl_->open.store(true);
                break;
            case ix::WebSocketMessageType::Close:
                impl_->open.store(false);
                break;
            case ix::WebSocketMessageType::Error: {
                std::lock_guard<std::mutex> lock(impl_->mu);
                impl_->last_error = msg->errorInfo.reason;
                break;
            }
            case ix::WebSocketMessageType::Message: {
                std::lock_guard<std::mutex> lock(impl_->mu);
                impl_->inbox.push_back(msg->str);
                break;
            }
            default:  // Ping / Pong / Fragment — ignored
                break;
        }
    });
}

LobbyClient::~LobbyClient() {
    if (impl_) {
        impl_->ws.stop();
    }
}

bool LobbyClient::connect(const std::string& url) {
    if (url.empty()) {
        return false;
    }
    const std::string dial = normalize_scheme(url);
    // wss:// vs ws:// is decided by the URL alone: the deployed matchmaker is
    // reached over TLS, a locally-run one (--matchmaker ws://127.0.0.1:8080/ws)
    // has no certificate and stays plain. IXWebSocket picks the socket kind off
    // the scheme; the TLS options below only matter for the secure one.
    if (is_secure_url(dial)) {
#if defined(BOMBER_HAS_LOBBY_TLS)
        const std::string ca = find_ca_bundle();
        if (ca.empty()) {
            std::lock_guard<std::mutex> lock(impl_->mu);
            impl_->last_error = "no certificate store found for TLS";
            return false;
        }
        ix::SocketTLSOptions tls;
        tls.caFile = ca;
        // Defaults, restated because they are the security property: peer
        // verification stays REQUIRED (any caFile other than "NONE") and the
        // certificate's name is checked against the host we dialled. A client
        // that skips either is not a TLS client.
        tls.disable_hostname_validation = false;
        impl_->ws.setTLSOptions(tls);
#else
        // Built without a TLS backend (-DBOMBER_LOBBY_TLS=OFF). Say so instead
        // of connecting in the clear behind the player's back: a wss:// URL is
        // a request for confidentiality, and silently downgrading it is worse
        // than not connecting.
        std::lock_guard<std::mutex> lock(impl_->mu);
        impl_->last_error = "this build has no TLS support (wss:// unavailable)";
        return false;
#endif
    }
    impl_->ws.setUrl(dial);
    // The lobby drives its own lifecycle (a dropped signaling link is a lobby
    // event, not something to silently reconnect under it).
    impl_->ws.disableAutomaticReconnection();
    impl_->ws.start();
    return true;
}

void LobbyClient::close() {
    impl_->open.store(false);
    impl_->ws.stop();
}

void LobbyClient::send(const std::string& json) {
    impl_->ws.sendText(json);
}

void LobbyClient::poll(const MessageHandler& handler) {
    std::deque<std::string> drained;
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        drained.swap(impl_->inbox);
    }
    if (handler) {
        for (const auto& frame : drained) {
            handler(frame);
        }
    }
}

bool LobbyClient::is_open() const {
    return impl_->open.load();
}

std::string LobbyClient::last_error() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->last_error;
}

// --- Typed control-plane helpers -------------------------------------------

void LobbyClient::create_lobby(const std::string& visibility, const std::string& name,
                               int max_seats, std::uint32_t build_hash, const std::string& player) {
    send(encode_create_lobby(visibility, name, max_seats, build_hash, player));
}

void LobbyClient::join_by_code(const std::string& code, std::uint32_t build_hash,
                               const std::string& player) {
    send(encode_join_by_code(code, build_hash, player));
}

void LobbyClient::list_public(std::uint32_t build_hash) {
    send(encode_list_public(build_hash));
}

void LobbyClient::set_ready(bool ready) {
    send(encode_set_ready(ready));
}

void LobbyClient::heartbeat() {
    send(encode_heartbeat());
}

void LobbyClient::send_candidates(const std::string& lobby_id, int seat,
                                  const std::vector<LobbyCandidate>& list) {
    send(encode_candidates(lobby_id, seat, list));
}

void LobbyClient::start_match(const std::string& lobby_id, const std::string& host_token,
                              int input_delay, std::uint32_t match_config_digest) {
    send(encode_start_match(lobby_id, host_token, input_delay, match_config_digest));
}

void LobbyClient::reanchor(const std::string& code, const std::string& roster_digest) {
    send(encode_reanchor(code, roster_digest));
}

void LobbyClient::match_over(const std::string& lobby_id) {
    send(encode_match_over(lobby_id));
}

void LobbyClient::send_chat(const std::string& text) {
    send(encode_chat(text));
}

void LobbyClient::poll_messages(const ServerMessageHandler& handler) {
    poll([&handler](const std::string& frame) {
        if (handler) handler(parse_server_message(frame));
    });
}

}  // namespace bomber::net
