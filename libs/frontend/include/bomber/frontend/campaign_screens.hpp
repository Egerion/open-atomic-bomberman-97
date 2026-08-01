#pragma once

#include <filesystem>
#include <string>

#include "bomber/frontend/campaign_state.hpp"
#include "bomber/frontend/match_backdrop.hpp"
#include "bomber/game_util/app_flow.hpp"  // AppInput
#include "bomber/ui/screen_context.hpp"

// The hidden CAMPAIGN cluster plus the in-round F1 HELP MODAL. The dialogs and
// the modal composite over the LAST rendered match frame WITHOUT ever ticking the
// sim, so they take a MatchBackdrop SEPARATELY from the front-end-service-only
// ScreenContext. Each owns its own nested SDL event loop.
//
// HelpBrowserModal is the ONLY reason match_runner.cpp depends on this header; it
// is a generic modal and belongs in libs/ui beside the other two. Moving it is a
// cross-package change.

namespace bomber::game {

// sub_4015C6: globs `*.cam` in the install root and, on a confirmed selection,
// parses it and arms campaign mode. A cancelled picker, an unreadable file or a
// zero-stage file leaves campaign mode untouched. Reached ONLY via the setup
// screen's 'C'×5 trigger, so it is not part of the AppState flow graph.
class CampaignPickerScreen {
public:
    CampaignPickerScreen(ScreenContext ctx, CampaignState state, MatchBackdrop backdrop)
        : ctx_(ctx), state_(state), backdrop_(backdrop) {}
    void run();

private:
    void arm_campaign(const std::filesystem::path& file);
    void disarm_campaign();

    ScreenContext ctx_;
    CampaignState state_;
    MatchBackdrop backdrop_;
};

// The campaign-activation confirmation dialog (sub_4015C6): getstring(95)="NOTE!"
// over getstring(1210)="Campaign Mode Activated!", dismissed by
// Enter/Space/Escape, every other key a no-op. Draws over the live match frame.
class CampaignConfirmScreen {
public:
    CampaignConfirmScreen(ScreenContext ctx, MatchBackdrop backdrop)
        : ctx_(ctx), backdrop_(backdrop) {}
    AppInput run();

private:
    float measure(const std::string& s) const;

    ScreenContext ctx_;
    MatchBackdrop backdrop_;
};

// The campaign stage-start banner (sub_40133F): "(<stage name>)" over "Prepare to
// begin Campaign!", shown once per stage transition. Dismissed by any confirm key
// or a short dwell.
class CampaignBannerScreen {
public:
    CampaignBannerScreen(ScreenContext ctx, CampaignState state, MatchBackdrop backdrop)
        : ctx_(ctx), state_(state), backdrop_(backdrop) {}
    AppInput run();

private:
    ScreenContext ctx_;
    CampaignState state_;
    MatchBackdrop backdrop_;
};

// "Congratulations! You made it through the whole campaign!" (sub_40133F's
// stage-exhausted branch, getstring 1220/1225), shown once the last stage is
// cleared, before returning to the menu.
class CampaignCompleteScreen {
public:
    CampaignCompleteScreen(ScreenContext ctx, MatchBackdrop backdrop)
        : ctx_(ctx), backdrop_(backdrop) {}
    AppInput run();

private:
    ScreenContext ctx_;
    MatchBackdrop backdrop_;
};

// The ONE modal a campaign round is allowed to show in place of the whole
// DRAW/RESULTS/VICTORY tier. sub_42A3F6 puts it up at 0x42A660 only when the
// pacing verdict is 2 — clock out, or no human survived — NEVER for "stage clear".
class CampaignUnsuccessfulScreen {
public:
    CampaignUnsuccessfulScreen(ScreenContext ctx, MatchBackdrop backdrop)
        : ctx_(ctx), backdrop_(backdrop) {}
    AppInput run();

private:
    ScreenContext ctx_;
    MatchBackdrop backdrop_;
};

// The in-round F1 help browser: the SAME generic *.BM browser the menu row opens,
// composited over the LAST rendered match frame with the sim frozen (this loop
// never ticks) — the sub_42A16F(1)/(0) bracket.
class HelpBrowserModal {
public:
    HelpBrowserModal(ScreenContext ctx, MatchBackdrop backdrop)
        : ctx_(ctx), backdrop_(backdrop) {}
    AppInput run();

private:
    ScreenContext ctx_;
    MatchBackdrop backdrop_;
};

}  // namespace bomber::game
