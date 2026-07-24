#include "bomber/game/screens/netplay_connect_screen.hpp"

#include <SDL3/SDL.h>

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "bomber/game/dialog_chrome.hpp"  // draw_acknowledge_dialog / draw_text_entry_dialog
#include "bomber/game/sprites.hpp"        // Sprite
#include "bomber/net/handshake.hpp"       // net::SeedHandshake
#include "bomber/net/udp_transport.hpp"   // net::UdpTransport
#include "bomber/platform/frame_clock.hpp"

namespace bomber::game {

namespace {

// Presentation-only tunables for the connect modals (not RE'd — the original
// has no netplay; ADR-0010 is our port's own online path).
constexpr std::uint64_t kHandshakeTimeoutMs = 10000;  // give up → back to the menu
constexpr std::uint64_t kSettleMs = 300;  // keep broadcasting after done() so the peer finishes too
constexpr std::size_t kAddressMax = 40;   // host:port line-edit cap
constexpr float kJoinPromptY = 180.0f;    // same anchor the save-as prompt uses (sub_4028D2)
const char kCancelLabel[] = "Cancel";

// Trim ASCII spaces/tabs from both ends.
std::string trim(const std::string& s) {
    std::size_t a = 0;
    std::size_t b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}

// Parse "host:port" (or a bare "host", defaulting the port). Rejects an empty
// host or a non-numeric / out-of-range port. IPv4/hostname only (single colon).
bool parse_host_port(const std::string& in, std::uint16_t default_port, std::string& host,
                     std::uint16_t& port) {
    const std::string s = trim(in);
    const std::size_t colon = s.find(':');
    const std::string host_part = trim(colon == std::string::npos ? s : s.substr(0, colon));
    const std::string port_part = colon == std::string::npos ? std::string() : trim(s.substr(colon + 1));
    if (host_part.empty()) return false;
    long p = default_port;
    if (!port_part.empty()) {
        for (char c : port_part)
            if (std::isdigit(static_cast<unsigned char>(c)) == 0) return false;
        p = std::strtol(port_part.c_str(), nullptr, 10);
        if (p <= 0 || p > 65535) return false;
    }
    host = host_part;
    port = static_cast<std::uint16_t>(p);
    return true;
}

}  // namespace

void NetplayConnectScreen::draw_backdrop() {
    SDL_SetRenderDrawColor(ctx_.sdl, 0, 0, 0, 255);
    SDL_RenderClear(ctx_.sdl);
    const Sprite& bg = ctx_.assets.frontend_pcx("MAINMENU");
    if (bg.tex) {
        SDL_FRect d{0, 0, static_cast<float>(bg.w), static_cast<float>(bg.h)};
        SDL_RenderTexture(ctx_.sdl, bg.tex, nullptr, &d);
    }
}

NetplayConnectResult NetplayConnectScreen::pump_handshake(net::UdpTransport& transport, bool is_host,
                                                         std::uint32_t host_seed,
                                                         const std::string& line1,
                                                         const std::string& line2) {
    NetplayConnectResult result;
    net::SeedHandshake handshake(transport, is_host, host_seed);
    platform::FrameClock frame_clock(ctx_.window);
    const std::uint64_t start = SDL_GetTicks();
    bool settling = false;
    std::uint64_t done_at = 0;
    const Sprite* winz = &ctx_.assets.frontend_pcx("WINZ");

    while (true) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) {
                result.window_closed = true;
                return result;
            }
            if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat && ev.key.key == SDLK_ESCAPE) {
                ctx_.audio.play(20);  // nav blip; cancel back to the menu
                return result;
            }
        }

        handshake.step();
        if (handshake.done()) {
            if (!settling) {
                settling = true;
                done_at = SDL_GetTicks();
                result.connected = true;
                result.seed = handshake.seed();
            }
            // Keep pumping (both sides re-broadcast) for a short settle so the
            // PEER also latches done() — send-then-poll can mark us done the
            // instant we receive the peer's reply, before our own datagram has
            // carried the payload it needs (the host's seed / the guest's ack).
            if (SDL_GetTicks() - done_at >= kSettleMs) return result;
        } else if (SDL_GetTicks() - start >= kHandshakeTimeoutMs) {
            return result;  // timed out (connected stays false) → return to the menu
        }

        ctx_.audio.update_music();
        draw_backdrop();
        draw_acknowledge_dialog(ctx_.sdl, ctx_.front_font, winz, line1, line2, kCancelLabel,
                                kDialogInkR, kDialogInkG, kDialogInkB);
        SDL_RenderPresent(ctx_.sdl);
        frame_clock.pace();
    }
}

NetplayConnectResult NetplayConnectScreen::run_host(net::UdpTransport& transport,
                                                    std::uint16_t port, std::uint32_t host_seed) {
    if (!transport.bind(port)) {
        // Port busy / socket failure: show a brief NOTE and return to the menu.
        // Reuses the acknowledge modal; dismissed by Esc or the ~10 s timeout,
        // no handshake is pumped since the socket never opened.
        NetplayConnectResult result;
        platform::FrameClock frame_clock(ctx_.window);
        const std::uint64_t start = SDL_GetTicks();
        const Sprite* winz = &ctx_.assets.frontend_pcx("WINZ");
        while (SDL_GetTicks() - start < kHandshakeTimeoutMs) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_EVENT_QUIT) {
                    result.window_closed = true;
                    return result;
                }
                if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) return result;
            }
            ctx_.audio.update_music();
            draw_backdrop();
            draw_acknowledge_dialog(ctx_.sdl, ctx_.front_font, winz, "NETWORK ERROR",
                                    "PORT " + std::to_string(port) + " UNAVAILABLE", " Ok ",
                                    kDialogInkR, kDialogInkG, kDialogInkB);
            SDL_RenderPresent(ctx_.sdl);
            frame_clock.pace();
        }
        return result;
    }
    return pump_handshake(transport, /*is_host=*/true, host_seed,
                          "HOSTING ON PORT " + std::to_string(port), "WAITING FOR A PLAYER...");
}

NetplayConnectResult NetplayConnectScreen::run_join(net::UdpTransport& transport,
                                                    std::uint16_t default_port) {
    // Phase 1 — the host:port line-edit (mirrors SchemeFilenamePrompt's SDL
    // text-input loop): a WINZ-less text-entry dialog prefilled with the
    // loopback default. Enter parses; a bad address re-labels the prompt in
    // place. Esc cancels; the window-close is surfaced for a hard quit.
    std::string entry = "127.0.0.1:" + std::to_string(default_port);
    std::string error;
    platform::FrameClock frame_clock(ctx_.window);
    SDL_StartTextInput(ctx_.window);

    while (true) {
        std::string host;
        std::uint16_t port = 0;
        bool targeted = false;

        while (!targeted) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_EVENT_QUIT) {
                    SDL_StopTextInput(ctx_.window);
                    NetplayConnectResult r;
                    r.window_closed = true;
                    return r;
                }
                if (ev.type == SDL_EVENT_TEXT_INPUT) {
                    if (ev.text.text && entry.size() < kAddressMax) entry += ev.text.text;
                } else if (ev.type == SDL_EVENT_KEY_DOWN) {
                    if (ev.key.key == SDLK_BACKSPACE) {
                        if (!entry.empty()) entry.pop_back();
                    } else if (ev.key.key == SDLK_RETURN || ev.key.key == SDLK_KP_ENTER) {
                        if (parse_host_port(entry, default_port, host, port)) {
                            ctx_.audio.play(10);  // accept sting
                            targeted = true;
                        } else {
                            ctx_.audio.play(20);  // reject blip; stay on the prompt
                            error = "INVALID ADDRESS - USE host:port";
                        }
                    } else if (ev.key.key == SDLK_ESCAPE) {
                        ctx_.audio.play(20);
                        SDL_StopTextInput(ctx_.window);
                        return NetplayConnectResult{};  // cancelled → back to the menu
                    }
                }
            }
            ctx_.audio.update_music();
            draw_backdrop();
            draw_text_entry_dialog(ctx_.sdl, ctx_.front_font, kJoinPromptY,
                                   error.empty() ? std::string("JOIN - HOST ADDRESS:") : error,
                                   entry, "Connect", "Cancel");
            SDL_RenderPresent(ctx_.sdl);
            frame_clock.pace();
        }

        // Phase 2 — open the socket and hand off to the shared handshake pump.
        // A bind/resolve failure re-labels the prompt and loops back to editing.
        if (!transport.bind(0) || !transport.set_peer(host, port)) {
            error = "CANNOT REACH " + host + ":" + std::to_string(port);
            continue;
        }
        SDL_StopTextInput(ctx_.window);
        return pump_handshake(transport, /*is_host=*/false, /*host_seed=*/0,
                              "CONNECTING TO " + host + ":" + std::to_string(port) + "...",
                              "PLEASE WAIT...");
    }
}

}  // namespace bomber::game
