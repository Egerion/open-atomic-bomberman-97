#pragma once

#include "bomber/render/renderer.hpp"
#include "bomber/sim/state.hpp"

// The two live-frame handles a screen needs to draw the LAST rendered match frame
// as its own backdrop. The in-round help browser and the campaign
// confirm/banner/complete dialogs composite their widget over that frame rather
// than cutting to MAINMENU, and the sim is NEVER ticked while they run — the
// frame is genuinely frozen, which is what makes holding a reference safe.
//
// Kept OUT of ScreenContext on purpose: these are match-runtime members, not
// front-end presentation services, so only the handful of match-coupled screens
// take this bundle alongside the ScreenContext they still need.

namespace bomber::game {

struct MatchBackdrop {
    Renderer& renderer;
    const sim::State& state;
};

}  // namespace bomber::game
