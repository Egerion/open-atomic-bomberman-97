#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "bomber/ui/screen_context.hpp"

// The two netplay CONNECTION modals (ADR-0010 §3.3 step 5 "lobby/session UI"):
// the direct/LAN rows of the NETWORK GAME menu. Each opens a UDP socket, runs
// the pre-match SeedHandshake (handshake.hpp) over the SAME transport the match
// then borrows, and reports the agreed seed back to NetplayRunner, which owns
// the transport. JOIN's address line-edit follows the save-as prompt's pattern
// (scheme_filename_prompt.cpp); a bare host defaults the port.
//
// Both are Esc-cancellable and time out after a few seconds; the window-close
// case is surfaced so GameApp can propagate a hard quit. Reuses the shared
// dialog chrome (dialog_chrome.hpp) — no new UI primitives.

namespace bomber::net {
class UdpTransport;  // borrowed by reference; the .cpp includes the real header
}  // namespace bomber::net

namespace bomber::game {

// The outcome of a connection attempt. `connected` gates the match run;
// `window_closed` distinguishes an SDL_QUIT (propagate AppInput::Quit) from a
// plain Esc/timeout cancel (return to the menu).
struct NetplayConnectResult {
    bool connected = false;
    bool window_closed = false;
    std::uint32_t seed = 0;
};

class NetplayConnectScreen {
public:
    explicit NetplayConnectScreen(ScreenContext ctx) : ctx_(ctx) {}

    // Bind `port`, then run the seed handshake as host announcing `host_seed`.
    NetplayConnectResult run_host(net::UdpTransport& transport, std::uint16_t port,
                                  std::uint32_t host_seed);
    // Prompt for a host address (prefilled 127.0.0.1:`default_port`), bind an
    // ephemeral local port, aim at the host, and run the handshake as guest.
    NetplayConnectResult run_join(net::UdpTransport& transport, std::uint16_t default_port);

private:
    // What the shared handshake pump SHOWS while it runs, and which side of the
    // handshake it is. One value rather than four arguments (§3).
    struct HandshakePrompt {
        std::string line1;
        std::string line2;
        std::uint32_t host_seed = 0;
        bool is_host = false;
    };

    // Pump SeedHandshake, draw the status modal over a WINZ dialog, Esc/close
    // cancel, ~10 s timeout. Returns connected + the agreed seed on done().
    NetplayConnectResult pump_handshake(net::UdpTransport& transport,
                                        const HandshakePrompt& prompt);
    // Engaged = leave; the bool is "the window closed" rather than "Esc".
    std::optional<bool> pump_cancel();
    // The bind-failed modal, which is the one path with no handshake to pump.
    NetplayConnectResult show_bind_failure(std::uint16_t port);

    ScreenContext ctx_;
};

}  // namespace bomber::game
