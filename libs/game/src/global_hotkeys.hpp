#pragma once

#include <SDL3/SDL.h>

#include "bomber/render/asset_store.hpp"
#include "bomber/render/sequences.hpp"

// The presentation shortcuts that must work from EVERY screen. Wired as a global
// SDL_EventFilter, which runs synchronously inside SDL_PumpEvents before the event
// reaches any screen's own SDL_PollEvent loop — so one installation covers every
// nested loop in the front end, and a swallowed key never leaks into a screen's "any
// key" handling (the asset screens' advance-on-any-key, the editor's text input).
//
// All PORT ENHANCEMENTS: the 1997 binary has no fullscreen, no HD artwork, no vsync
// toggle, no cadence lever and no diagnostic panel, so none of this is RE'd.

namespace bomber::game {

// The shell members the shortcuts flip, bundled by reference like ScreenContext.
struct HotkeySlots {
    AssetStore& assets;
    SequenceSet& seqs;
    SDL_Window* window = nullptr;
    SDL_Renderer* sdl = nullptr;
    bool& fullscreen;
    bool& uncap_fps;
    bool& native_cadence;
    bool& show_fps;
    bool& show_netstats;
    bool& options_dirty;
};

// SDL_EventFilter's contract: true keeps the event for the caller's own loop,
// false swallows it. Every shortcut below swallows.
bool handle_global_hotkey(const HotkeySlots& s, const SDL_Event& ev);

}  // namespace bomber::game
