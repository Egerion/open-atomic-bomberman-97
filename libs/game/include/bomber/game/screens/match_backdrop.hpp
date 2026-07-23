#pragma once

#include "bomber/game/renderer.hpp"
#include "bomber/sim/state.hpp"

// The match-coupled backdrop seam (ADR-0009 §"match-coupled screens ... take
// Renderer& + const sim::State& SEPARATELY — ScreenContext stays front-end-
// service-only"): the two live-frame handles a screen needs to draw the LAST
// rendered match frame as its own backdrop. The in-round help browser and the
// campaign confirm/banner/complete dialogs all composite their widget over
// renderer_->draw_frame(sim_.state()) rather than cutting to MAINMENU, and the
// sim is never ticked while they run (a genuinely frozen frame). Kept OUT of
// ScreenContext on purpose — those are match-runtime members (renderer_/sim_),
// not front-end presentation services, so only the handful of match-coupled
// screens take this bundle, alongside the ScreenContext they still need.
//
// A cheap value type (two references), copied by value into each screen; the
// referenced Renderer/State are GameApp members that outlive every screen.
// GameApp::match_backdrop() builds a fresh one on demand, exactly like sctx().

namespace bomber::game {

struct MatchBackdrop {
    Renderer& renderer;
    const sim::State& state;
};

}  // namespace bomber::game
