#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <functional>

// THE SETTLE PUMP — "keep draining the link for a moment before handing it on",
// at every seam where one net session stops pumping a transport and another is
// about to start.
//
// The LAST datagram a peer sends before it leaves a stage is exactly the one it
// cannot re-send: a session only re-acks when the other side's next burst
// arrives, which a peer that left the instant it decoded will never see. So the
// loser of that race sits until its own timeout and reports the other player as
// gone.
//
// Safe against setup_session.hpp's one-pump-at-a-time rule for a reason worth
// stating once: RollbackSession re-sends its WHOLE unconfirmed input window on
// every pump, so an early input datagram swallowed here comes again.
//
// Returns false if the window closed under it — the caller then returns Quit.

namespace bomber::game {

// Drain the OS event queue and report whether the window closed. The ONE event
// a netplay wait loop may never swallow: these loops draw nothing and read no
// keys, so without this a peer sitting in a settle or a headless CLI exchange
// would ignore the close and hang until its own timeout.
inline bool net_window_closed() {
    SDL_Event ev;
    while (SDL_PollEvent(&ev))
        if (ev.type == SDL_EVENT_QUIT) return true;
    return false;
}

inline bool net_settle(std::uint64_t ms, const std::function<void()>& pump) {
    const std::uint64_t settle_until = SDL_GetTicks() + ms;
    while (SDL_GetTicks() < settle_until) {
        if (net_window_closed()) return false;
        pump();
        SDL_Delay(2);
    }
    return true;
}

}  // namespace bomber::game
