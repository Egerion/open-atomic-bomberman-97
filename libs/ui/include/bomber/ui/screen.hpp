#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>
#include <vector>

#include "bomber/audio/audio_engine.hpp"
#include "bomber/render/asset_store.hpp"

// The generic asset-driven front-end Screen — the SDL side of the original's one
// screen primitive `sub_42A088(name, wait)` (docs/re/frontend-flow.md): a
// full-screen PCX, optional ANI overlays paced by a frame counter, and a "key OR
// dwell timeout" wait policy. Presentation only (ADR-0004).
//
// A Screen must NEVER touch music: sub_42A088 only blits and waits, and the
// background track is started separately by sub_42741E in the boot/menu callers
// (sub_42B060/sub_42B9CE) ONCE for the whole IPLOGO -> HSLOGO -> TITLE chain, so
// starting it here would restart it per screen.

namespace bomber::game {

struct ScreenOverlay {
    const AniTextures* ani = nullptr;
    std::string sequence;
    int x = 0, y = 0;  // top-left blit origin, screen space
};

// WHICH wait loop the original puts this picture behind. All three blit the same
// way (no wipe) and differ only in what they do afterwards — and that difference
// is entirely audible. Collapsing them onto the first rule, as the port once
// did, invents an Escape sting on DRAW, an auto-advance blip on the round-end
// screens, and a key handler on VICTORY where no key is read at all.
enum class WaitLoop : std::uint8_t {
    // sub_42A088(name, 1) — the routine's OWN loop (boot logos, TITLE). Blip 20
    // on any real key; Escape is one of its three accept codes (0x42A136-
    // 0x42A155 admits 27, 32, 13), so it stings like Enter. getvalue(12)'s
    // timeout is applied at 0x42A117, BEFORE the -1/-2 no-key test at 0x42A11E,
    // so an auto-advance is seen as a real Enter: blip AND sting.
    AssetScreen,
    // sub_42A3F6's round-end loops — DRAW @0x42A73A, the RESULTS tally
    // @0x42ADE9. Same blip, but Escape (0x42A7EF / 0x42AE9E) only sets the abort
    // flag dword_464A68 = 2: no sting. Their 6 s timeout is applied at 0x42A79D
    // / 0x42AE4C, AFTER the blip test, so an idle dwell stings alone.
    RoundEnd,
    // sub_42A088(name, 0) plus a blocking sub_413CB0 delay — the VICTORY/TEAM
    // tail @0x42AF80. No key is read, so nothing here can make a sound.
    TimedCut,
};

// Enough to render title/logo/results with no per-screen code; an interactive
// screen keeps it as its backdrop.
struct ScreenDef {
    std::string background;  // front-end PCX base name (AssetStore key)
    std::vector<ScreenOverlay> overlays;
    // getvalue(12)'s attract dwell (sub_42A088), in ms so it is
    // resolution-independent. 0 waits indefinitely for a key.
    std::uint32_t dwell_ms = 0;
    bool skippable = true;
    WaitLoop wait = WaitLoop::AssetScreen;
};

class Screen {
public:
    Screen(const AssetStore& assets, AudioEngine& audio) : assets_(&assets), audio_(&audio) {}

    void enter(const ScreenDef& def, std::uint64_t now_ms);

    // One keypress; what it sounds like is WaitLoop's business. Returns true iff
    // the key was an accept (Enter/Space/Escape), so a caller can tell an Escape
    // "back" from an Enter/Space "advance". A skip never stops the music.
    bool on_key(SDL_Keycode key);

    // Advance the overlay frame counter and re-test the dwell timeout.
    void update(std::uint64_t now_ms);

    void draw(SDL_Renderer* ren) const;

    bool done() const { return done_; }

private:
    const AssetStore* assets_ = nullptr;
    AudioEngine* audio_ = nullptr;
    ScreenDef def_;
    std::uint64_t entered_ms_ = 0;
    std::uint64_t frame_ = 0;  // overlay pacer: step = frame % statecnt
    bool done_ = false;
};

}  // namespace bomber::game
