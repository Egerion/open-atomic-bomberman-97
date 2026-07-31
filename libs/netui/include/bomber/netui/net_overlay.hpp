#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>

#include "bomber/game_util/app_flow.hpp"  // AppInput
#include "bomber/ui/screen_context.hpp"

// THE IN-MATCH NETPLAY DIAGNOSTIC OVERLAY — the drawing half of net_stats.hpp.
//
// The split is the same one libs/platform has been getting (FramePacer): the
// STATISTICS are SDL-free and unit-tested headless in libs/net, and only the
// pixels live here. Nothing in this file computes a number; it formats and
// draws the snapshot RollbackSession::stats() already holds.
//
// TOGGLE KEY: F3. The survey behind that choice, since the front end is dense
// with reserved keys — F1 is the RE'd help browser on every screen that has one
// (sub_410F81 / sub_406DDE) and is also the in-round help modal; F2 is the lobby
// chat overlay; F5 refreshes the public-lobby browser; F7 (fps indicator),
// F8 (uncapped framerate), F9 (native cadence), F11 and Alt+Enter (fullscreen)
// and Tab (HD artwork) are all swallowed by GameApp's global SDL_EventFilter
// before any screen sees them; F10 opens the main menu's quit confirm. F3 is
// free everywhere, sits beside the two overlays it is a sibling of, and — unlike
// a letter or the backquote — is the same physical key on every keyboard layout.
//
// OFF BY DEFAULT, AND DELIBERATELY NOT PERSISTED. `show_netstats_` is session-
// only: it is never read from or written to options.ini, so there is no path by
// which a saved setting could reach a capture run. That is strictly stronger
// than the capture-time pin the other three video levers need (game_app.cpp's
// capture_run()), and it is the guarantee tests/visual relies on. The draw is
// additionally gated on a live netplay session, which a capture never has.

namespace bomber::net {
struct NetStats;
struct SessionSummary;
}  // namespace bomber::net

namespace bomber::game {

class FontTextures;

// The SDL_Keycode the match loop watches. Exported so the key lives in exactly
// one place rather than being repeated in a comment somewhere.
inline constexpr SDL_Keycode kNetOverlayToggleKey = SDLK_F3;

// Draw the panel in the top-left of the view. Cheap and stateless: it reads the
// snapshot and nothing else, so it can be called every displayed frame.
// `local_seats` is only used to label which seat is us.
void draw_net_overlay(SDL_Renderer* ren, const FontTextures& font, const net::NetStats& stats,
                      std::uint16_t local_seats);

// "YYYY-MM-DD HH:MM:SS" from the local wall clock — the one place a real clock
// is read for the log, kept out of libs/net so the formatter there stays pure.
std::string net_log_timestamp();

// Append one formatted record to `netdiag.log` NEXT TO THE EXECUTABLE.
//
// This is the part that makes "it suddenly cut out" answerable: by the time the
// player has alt-tabbed to tell us, the window and everything on it are gone, so
// the reason and the last values have to survive on disk. Silent on failure by
// design — a diagnostic must never be the thing that kills a match — and it
// never runs during a capture (there is no netplay session in one).
void append_net_session_log(const net::SessionSummary& summary);

// The on-screen half: a modal that names WHY the session ended and shows the
// numbers it ended with, dismissed with Enter/Escape. Shown only for an abnormal
// end — a match that simply finished has the RESULTS/VICTORY screens to say so.
// Returns Quit if the window closed under it, else Advance.
AppInput present_net_session_end(ScreenContext ctx, const net::SessionSummary& summary);

}  // namespace bomber::game
