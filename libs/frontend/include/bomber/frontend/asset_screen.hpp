#pragma once

#include "bomber/game_util/app_flow.hpp"
#include "bomber/game_util/net_round_gate.hpp"
#include "bomber/ui/screen.hpp"
#include "bomber/ui/screen_context.hpp"

// Runs one asset-driven full-screen image (logo / title / results / draw /
// victory) to completion via the shared Screen presenter. Extracted verbatim
// from GameApp::present_screen (ADR-0009): sub_42A088 CUTS between screens (no
// wipe), so there is no transition out — the next screen simply replaces this
// one. Shared by the boot chain, the results tail, and the run_app driver, so it
// lives as a free function rather than inside any one screen class.

namespace bomber::game {

// `gate` (null on every local path) turns this into an ONLINE between-rounds
// screen: pumped every frame, dismissed by the HOST's accept and by the guest
// only when the host's commitment arrives — see net_round_gate.hpp. The dwell
// auto-advance is suppressed while a gate is present, since the two peers must
// leave together and only the gate knows when that is.
AppInput present_asset_screen(ScreenContext ctx, const ScreenDef& def,
                              NetRoundGate* gate = nullptr);

}  // namespace bomber::game
