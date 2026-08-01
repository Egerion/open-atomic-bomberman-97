#pragma once

#include "bomber/frontend/campaign_state.hpp"
#include "bomber/frontend/match_backdrop.hpp"
#include "bomber/frontend/setup_state.hpp"
#include "bomber/game_util/app_flow.hpp"  // AppInput
#include "bomber/netui/chat_overlay.hpp"
#include "bomber/netui/net_setup_link.hpp"
#include "bomber/ui/screen_context.hpp"

// The PLAYER INPUT TYPE SELECTION screen (sub_410F81) — screen 1 of the pre-match
// flow. Rows, keys, start guards and the hidden 'C'x5 campaign trigger are in
// docs/frontend-setup-screens.md.
//
// CampaignState and MatchBackdrop are threaded in ONLY so the 'C'x5 trigger can
// construct a CampaignPickerScreen; the body reads and writes every non-service
// member through SetupState.
//
// ONLINE (docs/re/network-screens.md §7): the DEFAULTED NetSetupLink turns this
// same screen into the net game's roster screen, which is what the original does
// too — both network screens commit into sub_42A3F6. A default-constructed link
// is ordinary local play and every net branch is inert. The F2 chat overlay is
// the one thing here that is NOT reverse-engineered.

namespace bomber::game {

class SetupScreen {
public:
    SetupScreen(ScreenContext ctx, SetupState state, CampaignState campaign, MatchBackdrop backdrop,
                NetSetupLink net = {}, ChatOverlay* chat = nullptr)
        : ctx_(ctx),
          state_(state),
          campaign_(campaign),
          backdrop_(backdrop),
          net_(net),
          chat_(chat) {}
    AppInput run();

private:
    // run() is a thin entry point: it applies the two-stage TEAM seeding the
    // original does on load, then hands these six to the SetupLoop in the .cpp's
    // anonymous namespace, which owns the frame loop and every key.
    ScreenContext ctx_;
    SetupState state_;
    CampaignState campaign_;
    MatchBackdrop backdrop_;
    NetSetupLink net_;
    ChatOverlay* chat_ = nullptr;  // BORROWED; owned by GameApp::present_net_online
};

}  // namespace bomber::game
