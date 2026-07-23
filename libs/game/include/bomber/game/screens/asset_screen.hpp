#pragma once

#include "bomber/game/app_flow.hpp"
#include "bomber/game/screen.hpp"
#include "bomber/game/screen_context.hpp"

// Runs one asset-driven full-screen image (logo / title / results / draw /
// victory) to completion via the shared Screen presenter. Extracted verbatim
// from GameApp::present_screen (ADR-0009): sub_42A088 CUTS between screens (no
// wipe), so there is no transition out — the next screen simply replaces this
// one. Shared by the boot chain, the results tail, and the run_app driver, so it
// lives as a free function rather than inside any one screen class.

namespace bomber::game {

AppInput present_asset_screen(ScreenContext ctx, const ScreenDef& def);

}  // namespace bomber::game
