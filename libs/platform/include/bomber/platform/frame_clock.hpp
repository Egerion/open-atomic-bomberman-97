#pragma once

#include <SDL3/SDL.h>

#include <cstdint>

#include "bomber/platform/frame_pacer.hpp"

namespace bomber::platform {

// The SDL-facing half of refresh-boundary pacing: owns the window's refresh
// period and defers the decision to FramePacer::plan_resync.
//
// Why an explicit sleep at all (ADR-0008): in Windows windowed mode
// SDL_RenderPresent does not reliably block on vblank (DWM's multi-frame flip
// queue), so the period sleep supplies the cadence vsync does not. Unknown
// refresh falls back to 60 Hz, which still bounds the loop.
class FrameClock {
public:
    explicit FrameClock(SDL_Window* window) : pacer_(period_for(window), SDL_GetTicksNS()) {}

    // Nanoseconds per displayed frame at the window's refresh (60 Hz fallback).
    std::uint64_t period_ns() const { return pacer_.period_ns(); }

    // Sleep until the next refresh boundary after the just-issued present, and
    // a no-op when the present already blocked past it.
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
