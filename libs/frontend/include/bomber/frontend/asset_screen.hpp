#pragma once

#include "bomber/game_util/app_flow.hpp"
#include "bomber/game_util/net_round_gate.hpp"
#include "bomber/ui/screen.hpp"
#include "bomber/ui/screen_context.hpp"

// Runs one asset-driven full-screen image (logo / title / results / draw /
// victory) to completion. sub_42A088 CUTS between screens, so there is no
// transition out — the next screen simply replaces this one. A free function
// rather than a class because the boot chain, the results tail and the app driver
// all share it.

namespace bomber::game {

// `gate` (null on every local path) turns this into an ONLINE between-rounds
// screen: dismissed by the HOST's accept and by the guest only when the host's
// commitment arrives. The dwell auto-advance is suppressed while a gate is
// present, because the two peers must leave together and only the gate knows when.
AppInput present_asset_screen(ScreenContext ctx, const ScreenDef& def,
                              NetRoundGate* gate = nullptr);

}  // namespace bomber::game
