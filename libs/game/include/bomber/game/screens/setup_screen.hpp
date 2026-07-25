#pragma once

#include "bomber/game/app_flow.hpp"  // AppInput
#include "bomber/game/chat_overlay.hpp"
#include "bomber/game/screen_context.hpp"
#include "bomber/game/screens/campaign_state.hpp"
#include "bomber/game/screens/match_backdrop.hpp"
#include "bomber/game/screens/net_setup_link.hpp"
#include "bomber/game/screens/setup_state.hpp"

// The PLAYER INPUT TYPE SELECTION screen (sub_410F81), extracted VERBATIM from
// GameApp (ADR-0009 §7): screen 1 of the pre-match flow reached from Play. A
// random GLUE<n> backdrop under the 1020 track, header getstring(50), and the
// 10-slot input-type list (OFF/COMPUTER/KEYBOARD/JOYSTICK) — each row prefixed by
// getstring(51) (Player %u) and TINTED with the slot's intrinsic colour (VALUELST
// 200-247, AssetStore::slot_color), plus a team marker when Team Play is on. Keys
// (docs/re/setup-screens.md): Up/Down pick a slot, Right cycles its type
// (OFF->COMPUTER->KEYBOARD sub0/1->JOY0..n->OFF), Left/'0'/'o' set it OFF, 'T'
// toggles team, Enter/Space advance (past the two start guards + 1 s debounce),
// Escape cancels to the menu, F1 opens the *.BM help browser. It also owns the
// hidden 'C'×5 campaign trigger, which opens the CampaignPickerScreen. run() owns
// the outer event loop; returns Advance to go on to the LEVEL screen, Back to
// cancel, Quit on window close.
//
// Four seams, all stored BY VALUE: ScreenContext (shared front-end services) +
// SetupState (the setup-specific mutable state GameApp still owns). CampaignState
// and MatchBackdrop are threaded in ONLY so the 'C'×5 trigger can construct the
// CampaignPickerScreen(ScreenContext, CampaignState, MatchBackdrop) — the setup
// body itself reads/writes every non-service member through SetupState.
//
// ONLINE (docs/re/network-screens.md §7, net_setup_link.hpp): the fifth,
// DEFAULTED seam turns this same screen into the net game's roster screen — the
// original does exactly that (both network screens commit into `sub_42A3F6`, so
// the roster/AI for a net match come from THIS handler, `sub_410F81`). With a
// live NetSetupLink the HOST publishes a preview after every edit and the GUEST
// renders that preview read-only, buzzing SFX 40 at any edit key
// (`sub_410F81`'s LABEL_159). A default-constructed link (`session == nullptr`)
// is ordinary local play and every net branch below is inert.
//
// A SIXTH seam, and the one thing here that is NOT reverse-engineered: the F2
// lobby-chat overlay (chat_overlay.hpp), a PORT-ONLY addition the maintainer
// asked for. An online setup screen is still a lobby state, so the conversation
// started in the waiting room carries on over this screen. nullptr — every local
// path — and it is inert.

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
    // Advance a slot's input type one step in the setup cycle (sub_421E80).
    // Called only by run()'s Right-key handler; delegates to the pure
    // cycle_slot_input_type (input.hpp) so the wrap order stays unit-tested.
    void cycle_input_type(int slot);

    ScreenContext ctx_;
    SetupState state_;
    CampaignState campaign_;
    MatchBackdrop backdrop_;
    NetSetupLink net_;
    ChatOverlay* chat_ = nullptr;  // BORROWED; owned by GameApp::present_net_online
};

}  // namespace bomber::game
