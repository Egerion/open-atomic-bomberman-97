#pragma once

#include <SDL3/SDL.h>

#include <cstdint>

#include "bomber/platform/frame_pacer.hpp"

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
    explicit FrameClock(SDL_Window* window)
        : pacer_(period_for(window), SDL_GetTicksNS()) {}

    // Nanoseconds per displayed frame at the window's refresh (60 Hz fallback).
    std::uint64_t period_ns() const { return pacer_.period_ns(); }

    // Sleep until the next refresh boundary after the just-issued present. A
    // no-op when present already blocked past the target (FramePacer's Resync
    // rule keeps the target phase-locked to the real vblank train); supplies
    // the missing block when it didn't. The decision itself lives in
    // FramePacer::plan_resync so it is unit-testable without SDL — this is the
    // SDL-facing half only, and the rule is unchanged.
    void pace() {
        const FramePacer::Wait wait = pacer_.plan_resync(SDL_GetTicksNS());
        if (wait.sleep_ns) SDL_DelayNS(wait.sleep_ns);
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

    FramePacer pacer_;
};

}  // namespace bomber::platform
