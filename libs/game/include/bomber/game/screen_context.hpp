#pragma once

#include <SDL3/SDL.h>

#include "bomber/assets/reslist.hpp"
#include "bomber/audio/audio_engine.hpp"
#include "bomber/audio/sound_director.hpp"
#include "bomber/game/asset_store.hpp"
#include "bomber/game/bmscreen.hpp"
#include "bomber/game/cursor_indicator.hpp"
#include "bomber/game/gamepad.hpp"
#include "bomber/game/input.hpp"
#include "bomber/game/screen.hpp"
#include "bomber/game/sequences.hpp"

// The shared presentation services every front-end screen needs, bundled so a
// screen can be its own class in its own file instead of a GameApp method
// (ADR-0008, the package-structure / god-object decomposition goal). GameApp
// builds a fresh ScreenContext on demand (sctx()) from its stable members and
// hands it to each screen — the decoupling seam that lets the ~30 present_*
// methods move out of GameApp one at a time without threading its whole member
// set through every screen.
//
// A cheap value type (references + raw pointers only); copied by value into
// each screen, so a screen never holds a reference into a temporary. The
// referenced services all outlive every screen (they are GameApp members for
// the app's lifetime). Match-coupled screens that need the live renderer/sim as
// a backdrop take those separately for now — this bundle stays front-end-only.

namespace bomber::game {

struct ScreenContext {
    AssetStore& assets;
    AudioEngine& audio;
    SoundDirector& sounds;
    KeyboardMapper& keyboard;
    GamepadMapper& gamepads;
    FontTextures& front_font;
    CursorIndicator& cursor_blink;
    Screen& asset_screen;  // the sub_42A088 full-screen image presenter (logo/title/results)
    SequenceSet& seqs;     // shared ANI sequences (the Goldman wheel's prize/ring icons)
    const assets::res::ValueList& values;
    SDL_Renderer* sdl = nullptr;
    SDL_Window* window = nullptr;
};

}  // namespace bomber::game
