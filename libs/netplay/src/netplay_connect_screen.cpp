#include "bomber/netplay/netplay_connect_screen.hpp"

#include <SDL3/SDL.h>

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "bomber/net/handshake.hpp"      // net::SeedHandshake
#include "bomber/net/udp_transport.hpp"  // net::UdpTransport
#include "bomber/platform/frame_clock.hpp"
#include "bomber/render/sprites.hpp"    // Sprite
#include "bomber/ui/dialog_chrome.hpp"  // draw_acknowledge_dialog / draw_text_entry_dialog
#include "lobby_chrome.hpp"             // the backdrop / ack modal / SFX ids these screens share

namespace bomber::game {

namespace {

// Presentation-only tunables for the connect modals (not RE'd — the original has
// no netplay; ADR-0010 is our port's own online path).
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
    const std::string port_part =
        colon == std::string::npos ? std::string() : trim(s.substr(colon + 1));
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

// The host:port line edit, running: the draft, the label above it (which doubles
// as the rejection message — a bad address re-labels the prompt in place), and
// the address it parsed to.
struct AddressEntry {
    std::string draft;
    std::string error;
    std::string host;
    std::uint16_t port = 0;
    std::uint16_t default_port = 0;
};

enum class AddressOutcome : std::uint8_t { Continue, Targeted, Cancelled, WindowClosed };

AddressOutcome address_event(ScreenContext ctx, AddressEntry& a, const SDL_Event& ev) {
    if (ev.type == SDL_EVENT_QUIT) return AddressOutcome::WindowClosed;
    if (ev.type == SDL_EVENT_TEXT_INPUT) {
        if (ev.text.text != nullptr && a.draft.size() < kAddressMax) a.draft += ev.text.text;
        return AddressOutcome::Continue;
    }
    if (ev.type != SDL_EVENT_KEY_DOWN) return AddressOutcome::Continue;
    if (ev.key.key == SDLK_BACKSPACE) {
        if (!a.draft.empty()) a.draft.pop_back();
        return AddressOutcome::Continue;
    }
    if (ev.key.key == SDLK_ESCAPE) {
        ctx.audio.play(kSfxBlip);
        return AddressOutcome::Cancelled;
    }
    if (ev.key.key != SDLK_RETURN && ev.key.key != SDLK_KP_ENTER) return AddressOutcome::Continue;
    if (parse_host_port(a.draft, a.default_port, a.host, a.port)) {
        ctx.audio.play(kSfxAccept);
        return AddressOutcome::Targeted;
    }
    ctx.audio.play(kSfxBlip);
    a.error = "INVALID ADDRESS - USE host:port";
    return AddressOutcome::Continue;
}

// Sit on the line edit until it parses, is cancelled, or the window closes.
AddressOutcome prompt_address(ScreenContext ctx, AddressEntry& a) {
    platform::FrameClock frame_clock(ctx.window);
    while (true) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            const AddressOutcome out = address_event(ctx, a, ev);
            if (out != AddressOutcome::Continue) return out;
        }
        ctx.audio.update_music();
        draw_lobby_backdrop(ctx);
        draw_text_entry_dialog(
            DialogPen{ctx.sdl, ctx.front_font}, kJoinPromptY,
            TextEntryLabels{a.error.empty() ? std::string("JOIN - HOST ADDRESS:") : a.error,
                            a.draft, "Connect", "Cancel"});
        SDL_RenderPresent(ctx.sdl);
        frame_clock.pace();
    }
}

}  // namespace

// Esc or the window close, the only two keys this screen reads. Engaged = leave
// (with `connected` false, i.e. back to the menu).
std::optional<bool> NetplayConnectScreen::pump_cancel() {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_EVENT_QUIT) return true;  // window closed
        if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat && ev.key.key == SDLK_ESCAPE) {
            ctx_.audio.play(kSfxBlip);
            return false;  // cancelled
        }
    }
    return std::nullopt;
}

NetplayConnectResult NetplayConnectScreen::pump_handshake(net::UdpTransport& transport,
                                                          const HandshakePrompt& prompt) {
    NetplayConnectResult result;
    net::SeedHandshake handshake(transport, prompt.is_host, prompt.host_seed);
    platform::FrameClock frame_clock(ctx_.window);
    const AckChrome ack = ack_chrome(ctx_);
    const std::uint64_t start = SDL_GetTicks();
    std::uint64_t done_at = 0;
    bool settling = false;

    while (true) {
        if (const std::optional<bool> left = pump_cancel()) {
            result.window_closed = *left;
            return result;
        }

        handshake.step();
        if (handshake.done()) {
            if (!settling) {
                settling = true;
                done_at = SDL_GetTicks();
                result.connected = true;
                result.seed = handshake.seed();
            }
            // Keep pumping so the PEER also latches done(): send-then-poll can
            // mark us done the instant we receive its reply, before our own
            // datagram has carried the payload it needs.
            if (SDL_GetTicks() - done_at >= kSettleMs) return result;
        } else if (SDL_GetTicks() - start >= kHandshakeTimeoutMs) {
            return result;  // timed out (connected stays false) → return to the menu
        }

        ctx_.audio.update_music();
        draw_lobby_backdrop(ctx_);
        draw_acknowledge_dialog(DialogPen{ctx_.sdl, ctx_.front_font}, ack.winz,
                                AcknowledgeLabels{prompt.line1, prompt.line2, kCancelLabel},
                                AcknowledgeStyle{kDialogInk});
        SDL_RenderPresent(ctx_.sdl);
        frame_clock.pace();
    }
}

NetplayConnectResult NetplayConnectScreen::show_bind_failure(std::uint16_t port) {
    // Port busy / socket failure: the acknowledge modal, dismissed by any key or
    // the ~10 s timeout. No handshake is pumped since the socket never opened.
    NetplayConnectResult result;
    platform::FrameClock frame_clock(ctx_.window);
    const AckChrome ack = ack_chrome(ctx_);
    const std::uint64_t start = SDL_GetTicks();
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
        draw_lobby_backdrop(ctx_);
        draw_ack(ctx_, ack, "NETWORK ERROR", "PORT " + std::to_string(port) + " UNAVAILABLE");
        SDL_RenderPresent(ctx_.sdl);
        frame_clock.pace();
    }
    return result;
}

NetplayConnectResult NetplayConnectScreen::run_host(net::UdpTransport& transport,
                                                    std::uint16_t port, std::uint32_t host_seed) {
    if (!transport.bind(port)) return show_bind_failure(port);
    return pump_handshake(transport, HandshakePrompt{"HOSTING ON PORT " + std::to_string(port),
                                                     "WAITING FOR A PLAYER...", host_seed,
                                                     /*is_host=*/true});
}

NetplayConnectResult NetplayConnectScreen::run_join(net::UdpTransport& transport,
                                                    std::uint16_t default_port) {
    // Phase 1 — the host:port line-edit (mirrors SchemeFilenamePrompt's SDL
    // text-input loop): a WINZ-less text-entry dialog prefilled with the loopback
    // default. Phase 2 opens the socket; a bind/resolve failure re-labels the
    // prompt and loops back to editing, which is why the two are one loop.
    AddressEntry a;
    a.default_port = default_port;
    a.draft = "127.0.0.1:" + std::to_string(default_port);
    SDL_StartTextInput(ctx_.window);

    while (true) {
        const AddressOutcome out = prompt_address(ctx_, a);
        if (out == AddressOutcome::WindowClosed) {
            SDL_StopTextInput(ctx_.window);
            NetplayConnectResult r;
            r.window_closed = true;
            return r;
        }
        if (out == AddressOutcome::Cancelled) {
            SDL_StopTextInput(ctx_.window);
            return NetplayConnectResult{};  // cancelled → back to the menu
        }
        if (!transport.bind(0) || !transport.set_peer(a.host, a.port)) {
            a.error = "CANNOT REACH " + a.host + ":" + std::to_string(a.port);
            continue;
        }
        SDL_StopTextInput(ctx_.window);
        const std::string where = a.host + ":" + std::to_string(a.port);
        return pump_handshake(transport, HandshakePrompt{"CONNECTING TO " + where + "...",
                                                         "PLEASE WAIT...", /*host_seed=*/0,
                                                         /*is_host=*/false});
    }
}

}  // namespace bomber::game
