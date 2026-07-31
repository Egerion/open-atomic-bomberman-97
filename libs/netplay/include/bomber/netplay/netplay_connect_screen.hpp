#pragma once

#include <cstdint>
#include <string>

#include "bomber/ui/screen_context.hpp"

// The two netplay CONNECTION modals (netplay increment 5c, ADR-0010 §3.3 step 5
// "lobby/session UI"): small screens driven from the main menu's START/JOIN NET
// GAME rows. Each opens a UDP socket, runs the pre-match SeedHandshake
// (handshake.hpp) over the SAME transport the lockstep match then borrows, and
// reports the agreed seed back to GameApp — which owns the transport and calls
// run_netplay_match() on success.
//
//   HOST: bind a known local port, show "HOSTING ON PORT n / WAITING FOR A
//         PLAYER...", and pump the handshake as host (a fixed host seed) until a
//         guest connects.
//   JOIN: a text-input modal (reusing the save-as line-edit pattern,
//         scheme_filename_prompt.cpp) prefilled "127.0.0.1:<port>"; Enter parses
//         host:port (a bare host defaults the port), then "CONNECTING TO
//         host:port..." pumps the handshake as guest until the host's seed is
//         adopted.
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
    // Draw the shared MAINMENU backdrop the connect modals sit over (same look
    // as the menu's own quit-confirm overlay).
    void draw_backdrop();
    // The per-frame handshake loop shared by host + join: pump SeedHandshake,
    // draw the status modal (`line1`/`line2` over a WINZ dialog), Esc/close
    // cancel, ~10 s timeout. Returns connected + the agreed seed on done().
    NetplayConnectResult pump_handshake(net::UdpTransport& transport, bool is_host,
                                        std::uint32_t host_seed, const std::string& line1,
                                        const std::string& line2);

    ScreenContext ctx_;
};

}  // namespace bomber::game
