#pragma once

#include <SDL3/SDL.h>

#include <optional>

#include "bomber/assets/reslist.hpp"
#include "bomber/assets/sch.hpp"
#include "bomber/audio/audio_engine.hpp"
#include "bomber/game/app_options.hpp"
#include "bomber/input/gamepad.hpp"
#include "bomber/render/asset_store.hpp"
#include "bomber/render/renderer.hpp"
#include "bomber/render/sequences.hpp"
#include "bomber/sim/tuning.hpp"
#include "bomber/ui/bmscreen.hpp"  // FontTextures
#include "bomber/ui/screen.hpp"

// The heavy half of boot: everything behind the two "Loading ..." dialogs the
// original shows before its logo chain (sub_42BE22 -> sub_41095A -> sub_41D695
// then sub_42896E).

namespace bomber::game {

// The shell members the data load fills in, bundled by reference like
// ScreenContext. Built fresh on demand; every reference outlives the call.
struct DataLoadSlots {
    const AppOptions& opts;
    SDL_Renderer* ren = nullptr;
    AssetStore& assets;
    SequenceSet& seqs;
    FontTextures& front_font;
    GamepadMapper& gamepads;
    AudioEngine& audio;
    std::optional<Renderer>& renderer;
    std::optional<Screen>& screen;
    sim::Tuning& base_tuning;
    const assets::sch::Scheme& scheme;
    const assets::res::ValueList& values;
};

// Decode, recolour and preload everything a match and a menu need, animating the
// boot progress dialog across the whole of it. False if the asset decode failed;
// a missing audio device is a warning, not a failure.
bool load_game_data(const DataLoadSlots& s);

}  // namespace bomber::game
