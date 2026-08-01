#pragma once

#include <SDL3/SDL.h>

#include "bomber/assets/reslist.hpp"
#include "bomber/audio/sound_director.hpp"
#include "bomber/game/app_options.hpp"
#include "bomber/render/asset_store.hpp"
#include "bomber/render/renderer.hpp"
#include "bomber/sim/simulation.hpp"
#include "bomber/ui/bmscreen.hpp"  // FontTextures

// The headless capture entry points — the ones a harness drives instead of a player.
// Each renders frames, writes BMPs and returns; none opens an interactive loop.
// tests/visual/run_visual_golden.cmake is the consumer and reads the EXIT CODE and
// the BMP hashes, so the stdout lines are informational (the §11 exception log.hpp
// records).

namespace bomber::game {

// The shell members a capture reads, bundled by reference like ScreenContext.
struct CaptureSlots {
    const AppOptions& opts;
    SDL_Renderer* sdl = nullptr;
    AssetStore& assets;
    FontTextures& front_font;
    const assets::res::ValueList& values;
    sim::Simulation& sim;
    Renderer& renderer;
    SoundDirector& sounds;
};

// --demo / --demo-shots: tick the scripted demo match, saving either one final frame
// or a named frame at each requested tick. The caller has already seeded the match.
// All three return the process exit code.
int run_demo_capture(const CaptureSlots& s);

// --bm-shot: one `.BM` text screen (the sub_41302D viewer) over MAINMENU, scrolled.
int run_bm_capture(const CaptureSlots& s);

// --menu-shot: the main-menu composite, pinned to row 0 and animation frame 0. The
// ground truth is the native oracle's `bm_native --boot-shot`, which renders the
// ORIGINAL's own menu through the DirectDraw->SDL3 shim.
int run_menu_capture(const CaptureSlots& s);

}  // namespace bomber::game
