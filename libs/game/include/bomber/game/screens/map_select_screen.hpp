#pragma once

#include "bomber/game/app_flow.hpp"  // AppInput
#include "bomber/game/screen_context.hpp"
#include "bomber/game/screens/map_select_state.hpp"

// The LEVEL & ROUNDS (map-select) screen (sub_406DDE), extracted VERBATIM from
// GameApp (ADR-0009 §7): screen 2 of the pre-match flow, a 2-row list on a random
// GLUE<n> backdrop — row 0 = LEVEL (RANDOM + the 11 built-ins), row 1 = NUMBER OF
// WINS (1..100). Left/Right cycle the highlighted row's value (the level wraps
// [-1 .. 10] over getvalue(35); wins +-1 or +-5 on PgUp/PgDn), Up/Down switch
// rows, Enter/Space commit the working level/wins into selected_level_/win_target_
// and start the match, Escape aborts the whole Play flow back to the menu
// (forfeiting the pending Goldman winner). It draws a SAMPLE-BLOCK preview via
// AssetStore::stage_preview and re-rolls the preview cells off the shared
// presentation LCG (setup_lcg — NOT sim::State::rng); the LCG draw order/count is
// observable. run() owns the outer event loop; the F1 *.BM help browser
// composites over the same per-frame draw. Returns Advance to start the match,
// Back to abort, Quit on window close.
//
// Two seams, both stored BY VALUE: ScreenContext (the shared front-end services)
// and MapSelectState (the map-select-specific mutable state GameApp still owns).

namespace bomber::game {

class MapSelectScreen {
public:
    MapSelectScreen(ScreenContext ctx, MapSelectState state) : ctx_(ctx), state_(state) {}
    AppInput run();

private:
    ScreenContext ctx_;
    MapSelectState state_;
};

}  // namespace bomber::game
