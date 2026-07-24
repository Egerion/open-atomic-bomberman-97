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

}  // namespace bomber::net
