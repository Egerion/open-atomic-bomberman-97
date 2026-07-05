#pragma once

#include <SDL3/SDL.h>

#include <cstdint>

#include "bomber/game/asset_store.hpp"

// The screen-to-screen transition primitive (docs/re/frontend-flow.md). The
// original covers a screen change with HEADWIPE.ANI played on the standard ANI
// pacer (step = counter % statecnt, sub_41DAA7) — there is no per-step
// duration, so the wipe is "done" once every step has shown once. When
// HEADWIPE.ANI is absent this falls back to a wall-clock alpha fade to black.
// Presentation only: no sim state, no determinism impact (ADR-0004).

namespace bomber::game {

class Transition {
public:
    explicit Transition(const AssetStore& assets) : assets_(&assets) {}

    // Begin a transition. `now_ms` seeds the fade clock (only used by the
    // fallback). Picks the HEADWIPE sequence up front so an empty file cleanly
    // selects the fade path.
    void start(std::uint64_t now_ms);

    // Advance one rendered frame; re-evaluates completion.
    void update(std::uint64_t now_ms);

    // Draw the wipe/fade over whatever is already on the back buffer.
    // `now_ms` drives the fallback fade's alpha (ignored by the wipe path).
    void draw(SDL_Renderer* ren, std::uint64_t now_ms) const;

    bool active() const { return active_; }
    bool done() const { return !active_; }

    // Frames the HEADWIPE wipe steps through before it is complete (our
    // tunable pacing: one step per rendered frame, like the original counter).
    // Also the fade duration for the fallback path.
    static constexpr std::uint32_t kFadeMs = 350;

private:
    const AssetStore* assets_ = nullptr;
    bool active_ = false;
    bool use_wipe_ = false;      // HEADWIPE.ANI present -> wipe, else fade
    std::uint64_t started_ms_ = 0;
    std::uint64_t frame_ = 0;    // wipe pacer: step = frame % statecnt
    std::size_t steps_ = 0;      // HEADWIPE step count (statecnt)
};

}  // namespace bomber::game
