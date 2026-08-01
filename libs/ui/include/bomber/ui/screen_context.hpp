#pragma once

#include <SDL3/SDL.h>

#include "bomber/assets/reslist.hpp"
#include "bomber/audio/audio_engine.hpp"
#include "bomber/audio/sound_director.hpp"
#include "bomber/game_util/cursor_indicator.hpp"
#include "bomber/input/gamepad.hpp"
#include "bomber/input/input.hpp"
#include "bomber/render/asset_store.hpp"
#include "bomber/render/sequences.hpp"
#include "bomber/ui/bmscreen.hpp"
#include "bomber/ui/screen.hpp"

// The shared presentation services every front-end screen needs, bundled so a
// screen can be its own class in its own file rather than a GameApp method
// (ADR-0008). GameApp builds a fresh ScreenContext on demand from its stable
// members and hands it to each screen.
//
// A cheap value type (references and raw pointers only), copied by value into
// each screen, so a screen never holds a reference into a temporary: everything
// referenced is a GameApp member that outlives every screen. Match-coupled
// screens take the live renderer/sim separately — this bundle stays front-end
// only.

namespace bomber::game {

struct ScreenContext {
    AssetStore& assets;
    AudioEngine& audio;
    SoundDirector& sounds;
    KeyboardMapper& keyboard;
    GamepadMapper& gamepads;
    FontTextures& front_font;
    CursorIndicator& cursor_blink;
    Screen& asset_screen;  // the sub_42A088 full-screen image presenter
    SequenceSet& seqs;     // shared ANI sequences (the Goldman wheel's icons)
    const assets::res::ValueList& values;
    SDL_Renderer* sdl = nullptr;
    SDL_Window* window = nullptr;
};

}  // namespace bomber::game
