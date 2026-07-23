#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>
#include <vector>

#include "bomber/game/asset_store.hpp"
#include "bomber/audio/audio_engine.hpp"

// The generic asset-driven front-end Screen — the SDL-side realisation of the
// original's one screen primitive `sub_42A088(name, wait)`
// (docs/re/frontend-flow.md). Data, not code: a screen is a full-screen PCX
// background, optional ANI overlays advanced by a frame counter, and a "wait
// for keypress OR dwell timeout" policy. Missing art logs and is skipped rather
// than aborting. Presentation only: never touches the sim or its RNG (ADR-0004).
//
// Music is NOT a screen concern: in the original, `sub_42A088` only loads the
// palette, blits the image, and runs the wait loop — the background track is
// started separately by `sub_42741E` (the music player) in the boot/menu
// callers (`sub_42B060`/`sub_42B9CE`). The boot track is therefore started ONCE
// before the logos and plays CONTINUOUSLY across IPLOGO -> HSLOGO -> TITLE; a
// Screen must never (re)start it, or the track would restart on every screen.

namespace bomber::game {

// One ANI overlay drawn on top of the background: the ANI to resolve a sequence
// from, the sequence name, and where its hotspot-anchored frames land.
struct ScreenOverlay {
    const AniTextures* ani = nullptr;  // the ANI holding the overlay sequence
    std::string sequence;              // sequence name within that ANI
    int x = 0, y = 0;                  // top-left blit origin (screen space)
};

// The declarative description of a screen. Enough to render title/logo/results
// with no per-screen code; a polished interactive screen keeps this as its
// backdrop and adds its own input handling on top.
struct ScreenDef {
    std::string background;            // front-end PCX base name (AssetStore key)
    std::vector<ScreenOverlay> overlays;
    // Dwell before the screen auto-advances (attract timeout). The original
    // waits on getvalue(12) (sub_42A088); we express it in ms so it is
    // resolution-independent. 0 = wait indefinitely for a key.
    std::uint32_t dwell_ms = 0;
    bool skippable = true;             // an accept key may cut the dwell short
};

// A running screen. draw() blits background + overlays each frame; the app
// shell polls done() and, once true, transitions out. The Screen itself only
// tracks wall-clock time and a frame counter — no sim state.
class Screen {
public:
    Screen(const AssetStore& assets, AudioEngine& audio) : assets_(&assets), audio_(&audio) {}

    // (Re)start this screen: remembers the def, resets the clock/counter. Does
    // NOT touch music — the background track is a caller concern (sub_42741E),
    // started once for the whole boot chain, not per screen (see header note).
    void enter(const ScreenDef& def, std::uint64_t now_ms);

    // A key was pressed while this screen is up. Mirrors the sub_42A088 wait
    // loop's key handling exactly: ANY real key plays the nav blip (SOUNDLST 20,
    // sub_427961(20)); the accept keys Enter / Space / Escape additionally play
    // the accept sting (SOUNDLST 10, sub_427961(10)) and, on a skippable screen,
    // finish it. Music is never stopped by a skip — only the screen changes.
    // Returns true iff this key was an accept (Enter/Space/Escape), so the caller
    // can distinguish an Escape "back" from an Enter/Space "advance".
    bool on_key(SDL_Keycode key);

    // Advance the frame counter (call once per rendered frame) and re-evaluate
    // the dwell timeout against the wall clock.
    void update(std::uint64_t now_ms);

    // Blit the background then the overlays for the current frame.
    void draw(SDL_Renderer* ren) const;

    // True once a key accepted a skippable screen or the dwell elapsed.
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
