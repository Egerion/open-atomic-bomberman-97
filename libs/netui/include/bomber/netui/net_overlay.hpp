#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>

#include "bomber/game_util/app_flow.hpp"  // AppInput
#include "bomber/ui/screen_context.hpp"

// THE IN-MATCH NETPLAY DIAGNOSTIC OVERLAY — the drawing half of net_stats.hpp.
// Nothing in this file computes a number; it formats and draws the snapshot
// RollbackSession::stats() already holds.
//
// TOGGLE KEY: F3, and the front end is dense with reserved keys — F1 is the RE'd
// help browser (sub_410F81 / sub_406DDE) and the in-round help modal; F2 is the
// lobby chat overlay; F5 refreshes the public-lobby browser; F7/F8/F9/F11/Tab/
// Alt+Enter are swallowed by GameApp's global SDL_EventFilter before any screen
// sees them; F10 opens the quit confirm. F3 is free everywhere and is the same
// physical key on every keyboard layout.
//
// OFF BY DEFAULT, AND DELIBERATELY NOT PERSISTED. `show_netstats_` is
// session-only — never read from or written to options.ini — so no saved setting
// can reach a capture run. That is strictly stronger than the capture-time pin
// the other three video levers need (game_app.cpp's capture_run()), and it is
// the guarantee tests/visual relies on.

namespace bomber::net {
struct NetStats;
struct SessionSummary;
}  // namespace bomber::net

namespace bomber::game {

class FontTextures;

// The SDL_Keycode the match loop watches. Exported so the key lives in exactly
// one place rather than being repeated in a comment somewhere.
inline constexpr SDL_Keycode kNetOverlayToggleKey = SDLK_F3;

// Cheap and stateless: it reads the snapshot and nothing else, so it can be
// called every displayed frame. `local_seats` only labels which seat is us.
void draw_net_overlay(SDL_Renderer* ren, const FontTextures& font, const net::NetStats& stats,
                      std::uint16_t local_seats);

// "YYYY-MM-DD HH:MM:SS" from the local wall clock — the one place a real clock
// is read for the log, kept out of libs/net so the formatter there stays pure.
std::string net_log_timestamp();

// Append one record to `netdiag.log` NEXT TO THE EXECUTABLE. This is what makes
// "it suddenly cut out" answerable: by the time the player has alt-tabbed to
// tell us, the window and everything on it are gone. Silent on failure by design
// — a diagnostic must never be the thing that kills a match.
void append_net_session_log(const net::SessionSummary& summary);

// The on-screen half: a modal that names WHY the session ended and shows the
// numbers it ended with, dismissed with Enter/Escape. Shown only for an abnormal
// end — a match that simply finished has the RESULTS/VICTORY screens to say so.
// Returns Quit if the window closed under it, else Advance.
AppInput present_net_session_end(ScreenContext ctx, const net::SessionSummary& summary);

}  // namespace bomber::game
