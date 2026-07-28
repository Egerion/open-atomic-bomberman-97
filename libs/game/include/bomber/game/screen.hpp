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

// WHICH wait loop the original puts this picture behind. All three show the
// image the same way (sub_42A088 blits and cuts — no wipe); they differ only in
// what they do afterwards, and that difference is entirely audible.
//
// The port ran every screen on the first rule, which invented an Escape sting
// on DRAW, an auto-advance blip on the round-end screens, and a whole key
// handler on VICTORY, where the original reads no key at all.
enum class WaitLoop : std::uint8_t {
    // sub_42A088(name, 1) — the routine's OWN wait loop (boot logos, TITLE).
    // Blip 20 on any real key. Escape is one of its three accept codes
    // (0x42A136-0x42A155 admits 27, 32 and 13), so it stings like Enter. The
    // getvalue(12) timeout is applied at 0x42A117, BEFORE the -1/-2 no-key test
    // at 0x42A11E, so an auto-advance is seen as a real Enter: blip AND sting.
    AssetScreen,
    // sub_42A3F6's own round-end loops — DRAW at 0x42A73A, the RESULTS tally at
    // 0x42ADE9. Same blip on any real key, but Escape (0x42A7EF / 0x42AE9E)
    // only sets the abort flag `dword_464A68 = 2` and leaves: no sting. And the
    // 6 s timeout is applied at 0x42A79D / 0x42AE4C, AFTER the blip test, so an
    // idle auto-advance blips not at all and plays the accept sting alone.
    RoundEnd,
    // sub_42A088(name, 0) followed by a blocking sub_413CB0 delay — the
    // VICTORY/TEAM tail at 0x42AF80. No key is read for the whole display, so
    // nothing here can make a sound: not a keypress, not the timeout.
    TimedCut,
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
    WaitLoop wait = WaitLoop::AssetScreen;
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

    // A key was pressed while this screen is up. ANY real key plays the nav blip
    // (SOUNDLST 20, sub_427961(20)) — both wait loops agree on that. Enter and
    // Space additionally play the accept sting (SOUNDLST 10) and, on a skippable
    // screen, finish it. Escape finishes either way but only stings on a
    // NON-round-end screen (see ScreenDef::round_end). Music is never stopped by
    // a skip — only the screen changes. Returns true iff this key was an accept
    // (Enter/Space/Escape), so the caller can distinguish an Escape "back" from
    // an Enter/Space "advance".
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
