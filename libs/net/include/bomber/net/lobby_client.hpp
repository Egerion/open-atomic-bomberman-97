#pragma once

#include <functional>
#include <memory>
#include <string>

// The online lobby CONTROL plane (ADR-0011 / docs/online-multiplayer-design.md
// §1): a thin WebSocket/TLS client to the matchmaking server. It carries ONLY
// pre-match signaling (create/join/list/ready/start + candidate exchange) as
// JSON text frames; the in-match DATA plane stays the raw-UDP Transport the
// RollbackSession borrows (this class never touches gameplay State). SDL-free
// and compiled only under BOMBER_ENABLE_LOBBY, so the deterministic netcode
// keeps building without a TLS backend on the headless gate.
//
// Threading: IXWebSocket delivers on its own background thread; this wrapper
// buffers inbound frames and hands them to the caller on the game thread via
// poll(), so the lobby layer above stays single-threaded like the rest of the
// game loop. Pimpl keeps <ixwebsocket/...> out of this public header.
namespace bomber::net {

class LobbyClient {
public:
    // One inbound server frame (raw JSON text). The lobby layer parses it.
    using MessageHandler = std::function<void(const std::string&)>;

    LobbyClient();
    ~LobbyClient();
    LobbyClient(const LobbyClient&) = delete;
    LobbyClient& operator=(const LobbyClient&) = delete;

    // Open the WS(S) connection to `url` ("ws://127.0.0.1:8080" locally,
    // "wss://mm.example.com" for a TLS-fronted deploy). Non-blocking; connection
    // progress is observed through is_open()/last_error() + poll(). Returns
    // false only on an obviously empty/malformed URL.
    bool connect(const std::string& url);
    void close();

    // Queue a raw JSON text frame to the server.
    void send(const std::string& json);

    // Drain buffered inbound frames on the CALLING (game) thread. Call once per
    // game tick; `handler` runs synchronously here, never on the WS thread.
    void poll(const MessageHandler& handler);

    bool is_open() const;
    std::string last_error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace bomber::net
