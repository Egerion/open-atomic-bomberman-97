#pragma once

#include "bomber/frontend/map_select_state.hpp"
#include "bomber/game_util/app_flow.hpp"  // AppInput
#include "bomber/netui/chat_overlay.hpp"
#include "bomber/netui/net_setup_link.hpp"
#include "bomber/ui/screen_context.hpp"

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
//
// ONLINE (docs/re/network-screens.md §7, net_setup_link.hpp): the third,
// DEFAULTED seam makes this the net game's map screen too — which is what the
// original does, since both network screens commit into `sub_42A3F6` and the map
// for a net match therefore comes from THIS handler, `sub_406DDE`. With a live
// NetSetupLink the HOST broadcasts the level (kind 43) and round count (kind 44)
// as it cycles them, and a GUEST renders those read-only, buzzing SFX 40 at any
// edit key (`sub_406DDE`'s LABEL_97). A default-constructed link is ordinary
// local play and every net branch is inert.
//
// A FOURTH seam, and the one thing here that is NOT reverse-engineered: the F2
// lobby-chat overlay (chat_overlay.hpp), a PORT-ONLY addition the maintainer
// asked for. An online map screen is still a lobby state, so the conversation
// started in the waiting room carries on over it. nullptr on every local path.

namespace bomber::game {

class MapSelectScreen {
public:
    MapSelectScreen(ScreenContext ctx, MapSelectState state, NetSetupLink net = {},
                    ChatOverlay* chat = nullptr)
        : ctx_(ctx), state_(state), net_(net), chat_(chat) {}
    AppInput run();

private:
    ScreenContext ctx_;
    MapSelectState state_;
    NetSetupLink net_;
    ChatOverlay* chat_ = nullptr;  // BORROWED; owned by GameApp::present_net_online
};

}  // namespace bomber::game
