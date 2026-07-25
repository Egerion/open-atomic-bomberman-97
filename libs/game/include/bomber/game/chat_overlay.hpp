#pragma once

#include <SDL3/SDL.h>

#include <string>

#include "bomber/game/screen_context.hpp"

// THE LOBBY CHAT OVERLAY — A PORT-ONLY ADDITION, NOT A REVERSE-ENGINEERED
// SCREEN.
//
// Atomic Bomberman (1997) has no chat of any kind: no chat window, no text
// entry outside the editor's own prompts, and no network message that could
// carry a line of text (docs/re/network-screens.md; the multiplayer audit in
// docs/re/audit/multiplayer-deep.md §2.5 records that even a "waiting for
// players" screen was never located in the binary). This panel exists because
// the maintainer asked for it — "once you are in a lobby, players can chat; a
// key opens a small translucent tab in the bottom right" — and it is flagged as
// such here so that nobody later reads it as an RE'd screen and goes hunting for
// the sub_XXXX that draws it. THERE IS NONE, and no such citation appears in
// this file or its .cpp. The wire side is equally new: PROTOCOL.md §7.
//
// What IS borrowed is everything that could be. The panel washes the chrome's
// own base coat (kDialogFill = dword_45C46C) and uses both of its real inks
// (kDialogInk = byte_49D38F, kDialogDim = dword_45C478); every glyph goes
// through the shared outlined-text primitive (draw_dialog_text, sub_41696C) in
// the front end's FONT6. So it sits inside the 1997 palette and typeface even
// though the layout — a translucent corner tab — is new by request.
//
// THE KEY IS F2 (see kToggleKey in the .cpp for the collision survey).
//
// USAGE. Three hooks per screen, and all three matter:
//   1. handle_event() FIRST in the event loop, right after the SDL_QUIT check.
//      It returns true when it consumed the event; while the panel is open it
//      consumes every key and text event, which is what stops typing from also
//      driving the screen underneath.
//   2. pump() once per frame, so the control link keeps delivering (and keeps
//      heart-beating — the matchmaker reaps a member that goes quiet).
//   3. draw() last, just before SDL_RenderPresent, so the panel sits on top.
// A default-constructed overlay (no flow) is inert: every hook is a no-op and
// draws nothing, which is what the local/LAN paths get.

namespace bomber::net {
class LobbyFlow;  // borrowed by pointer; the .cpp includes the real header
}  // namespace bomber::net

namespace bomber::game {

class ChatOverlay {
public:
    // `flow` is BORROWED and must outlive this object. nullptr = no lobby (local
    // play, a LAN game, or a build without BOMBER_ENABLE_LOBBY) and the whole
    // overlay goes inert.
    explicit ChatOverlay(net::LobbyFlow* flow = nullptr) : flow_(flow) {}
    ~ChatOverlay();
    ChatOverlay(const ChatOverlay&) = delete;
    ChatOverlay& operator=(const ChatOverlay&) = delete;

    bool live() const { return flow_ != nullptr; }
    bool open() const { return open_; }

    // Feed one SDL event. True = CONSUMED; the caller must skip its own handling
    // of it. Never consumes SDL_EVENT_QUIT (check that first anyway).
    // ScreenContext travels BY VALUE here, as it does everywhere else — it is a
    // bundle of references, so a copy costs nothing and callers can hand over a
    // temporary sctx().
    bool handle_event(const SDL_Event& ev, ScreenContext ctx);

    // One pump of the lobby control link on the SDL clock. Screens that already
    // step the flow themselves (the waiting room) must NOT also call this.
    void pump();

    // Paint the tab (closed) or the panel (open) in the bottom-right corner.
    void draw(ScreenContext ctx);

    // Drop the draft and stop SDL text input on whichever window it was started
    // on. Called by the destructor; call it explicitly when a screen tears down
    // while the panel might still be open.
    void close();

private:
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
