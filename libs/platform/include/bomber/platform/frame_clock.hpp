#pragma once

#include <SDL3/SDL.h>

#include <cstdint>

namespace bomber::platform {

// Refresh-boundary frame pacer — the single home for what used to be GameApp's
// `refresh_period_ns()` + `pace_to_refresh()`, hand-copied into every front-end
// loop (menu, setup, Goldman wheel, ...). Owns the target period (the window's
// display refresh, 60 Hz fallback) and the running pace target; `pace()` sleeps
// until the next boundary after the caller's present.
//
// Rationale (ADR-0008): on Windows windowed mode SDL_RenderPresent does not
// reliably block on vblank (DWM's multi-frame flip queue), so an explicit
// refresh-period sleep supplies the cadence vsync doesn't. Unknown refresh
// falls back to 60 Hz, which still bounds the loop. Part of the `bomber::platform`
// engine-base layer so front-end code never re-implements pacing.
class FrameClock {
public:
    explicit FrameClock(SDL_Window* window) : period_ns_(period_for(window)) {
        target_ns_ = SDL_GetTicksNS() + period_ns_;
    }

    // Nanoseconds per displayed frame at the window's refresh (60 Hz fallback).
    std::uint64_t period_ns() const { return period_ns_; }

    // Sleep until the next refresh boundary after the just-issued present. A
    // no-op when present already blocked past the target (the else-branch keeps
    // the target phase-locked to the real vblank train); supplies the missing
    // block when it didn't.
    void pace() {
        const std::uint64_t now = SDL_GetTicksNS();
        if (now < target_ns_) {
            SDL_DelayNS(target_ns_ - now);
            target_ns_ += period_ns_;
        } else {
            target_ns_ = now + period_ns_;
        }
    }

private:
    static std::uint64_t period_for(SDL_Window* window) {
        std::uint64_t period = 1'000'000'000ull / 60;
        if (const SDL_DisplayMode* mode =
                SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
            mode && mode->refresh_rate_numerator > 0 && mode->refresh_rate_denominator > 0) {
            period = 1'000'000'000ull * mode->refresh_rate_denominator /
                     static_cast<std::uint64_t>(mode->refresh_rate_numerator);
        }
        return period;
    }

    std::uint64_t period_ns_;
    std::uint64_t target_ns_;
};

}  // namespace bomber::platform
