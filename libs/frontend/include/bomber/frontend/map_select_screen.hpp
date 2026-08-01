#pragma once

#include "bomber/frontend/map_select_state.hpp"
#include "bomber/game_util/app_flow.hpp"  // AppInput
#include "bomber/netui/chat_overlay.hpp"
#include "bomber/netui/net_setup_link.hpp"
#include "bomber/ui/screen_context.hpp"

// The LEVEL & ROUNDS screen (sub_406DDE) — screen 2 of the pre-match flow. Rows,
// keys, the sample-block preview and the online half are all in
// docs/frontend-setup-screens.md. Advance starts the match, Back aborts the WHOLE
// Play flow to the menu (forfeiting the pending Goldman winner), Quit on window
// close.
//
// The preview cells re-roll off the shared presentation LCG — never
// sim::State::rng — and its draw order and count are observable.
//
// ONLINE (docs/re/network-screens.md §7): the DEFAULTED NetSetupLink makes this
// the net game's map screen too, which is what the original does — both network
// screens commit into sub_42A3F6, so the map for a net match comes from THIS
// handler. A default-constructed link is ordinary local play and every net branch
// is inert. The F2 chat overlay is the one thing here that is NOT
// reverse-engineered.

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
