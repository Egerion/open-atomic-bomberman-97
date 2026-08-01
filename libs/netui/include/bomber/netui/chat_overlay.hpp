#pragma once

#include <SDL3/SDL.h>

#include <string>

#include "bomber/ui/screen_context.hpp"

// THE LOBBY CHAT OVERLAY — A PORT-ONLY ADDITION, NOT A REVERSE-ENGINEERED
// SCREEN. Atomic Bomberman (1997) has no chat of any kind and no network message
// that could carry a line of text (docs/re/network-screens.md;
// docs/re/audit/multiplayer-deep.md §2.5 records that even a "waiting for
// players" screen was never located in the binary). Flagged here so nobody later
// reads it as an RE'd screen and goes hunting for the sub_XXXX that draws it:
// THERE IS NONE. The wire side is equally new — PROTOCOL.md §7.
//
// What IS borrowed is everything that could be: the chrome's own base coat
// (kDialogFill = dword_45C46C), both of its real inks (kDialogInk =
// byte_49D38F, kDialogDim = dword_45C478), and the shared outlined-text
// primitive (draw_dialog_text, sub_41696C) in the front end's FONT6.
//
// THE KEY IS F2 (see kToggleKey in the .cpp for the collision survey).
//
// USAGE, and the ORDER matters: handle_event() FIRST in the event loop, right
// after the SDL_QUIT check, because an open panel consumes every key and text
// event and that is what stops typing from also driving the screen underneath;
// pump() once per frame, so the control link keeps heart-beating (the matchmaker
// reaps a member that goes quiet); draw() last, before SDL_RenderPresent.

namespace bomber::net {
class LobbyFlow;  // borrowed by pointer; the .cpp includes the real header
}  // namespace bomber::net

namespace bomber::game {

class ChatOverlay {
public:
    // `flow` is BORROWED and must outlive this object. nullptr = no lobby (local
    // play, a LAN game, or a build without BOMBER_ENABLE_LOBBY).
    explicit ChatOverlay(net::LobbyFlow* flow = nullptr) : flow_(flow) {}
    ~ChatOverlay();
    ChatOverlay(const ChatOverlay&) = delete;
    ChatOverlay& operator=(const ChatOverlay&) = delete;

    bool live() const { return flow_ != nullptr; }
    bool open() const { return open_; }

    // True = CONSUMED; the caller must skip its own handling. Never consumes
    // SDL_EVENT_QUIT. ScreenContext travels BY VALUE, as everywhere else — it is
    // a bundle of references, so callers can hand over a temporary sctx().
    bool handle_event(const SDL_Event& ev, ScreenContext ctx);

    // One pump of the lobby control link. Screens that already step the flow
    // themselves (the waiting room) must NOT also call this.
    void pump();

    // Paint the tab (closed) or the panel (open) in the bottom-right corner.
    void draw(ScreenContext ctx);

    // Drop the draft and stop SDL text input on whichever window it was started
    // on. Called by the destructor; call it explicitly when a screen tears down
    // while the panel might still be open.
    void close();

private:
    // The two halves of handle_event: the closed panel's single toggle key, and
    // the open panel's editing keys.
    bool open_panel(const SDL_Event& ev, ScreenContext ctx);
    void handle_key(SDL_Keycode key, ScreenContext ctx);

    // Pointer-first, then the heap members, then the counter and the flag —
    // grouped by alignment, per .clang-tidy's
    // clang-analyzer-optin.performance.Padding.
    net::LobbyFlow* flow_ = nullptr;
    SDL_Window* text_input_window_ = nullptr;  // non-null while SDL text input is on
    std::string entry_;                        // the line being typed
    unsigned seen_revision_ = 0;               // chat_revision() as of the last look
    bool open_ = false;
};

}  // namespace bomber::game
