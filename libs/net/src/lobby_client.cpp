#include "bomber/net/lobby_client.hpp"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

#include <atomic>
#include <deque>
#include <mutex>

namespace bomber::net {
namespace {

// ix::initNetSystem() does WSAStartup on Windows (a no-op elsewhere). Call it
// exactly once per process even with several LobbyClients live.
std::once_flag g_net_init;
void ensure_net_system() {
    std::call_once(g_net_init, [] { ix::initNetSystem(); });
}

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
    impl_->ws.setUrl(url);
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
