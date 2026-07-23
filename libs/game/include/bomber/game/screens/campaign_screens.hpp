#pragma once

#include "bomber/game/app_flow.hpp"  // AppInput
#include "bomber/game/screen_context.hpp"
#include "bomber/game/screens/campaign_state.hpp"
#include "bomber/game/screens/match_backdrop.hpp"

// The hidden CAMPAIGN cluster + the in-round F1 HELP MODAL, extracted VERBATIM
// from GameApp (ADR-0009 §8 — the match-coupled screens). The picker composites
// over a pick_glue GLUE backdrop like the front-end pickers; the confirm/banner/
// complete dialogs and the help modal composite over the LAST rendered match
// frame (renderer_->draw_frame(sim_.state()), never ticking the sim), so per
// ADR-0009 they take a MatchBackdrop (Renderer& + const sim::State&) SEPARATELY
// from the front-end-service-only ScreenContext. Each owns its own nested SDL
// event loop and returns exactly what the GameApp method it replaced returned.
// All seams stored BY VALUE (cheap reference bundles).

namespace bomber::game {

// The hidden campaign-mode picker (was GameApp::present_campaign_picker,
// sub_4015C6): globs `*.cam` in the install root (CampaignFilePicker) over a
// pick_glue GLUE backdrop, and on a confirmed selection parses it
// (assets::res::load_campaign) and, if it yields at least one stage, arms
// campaign mode — seeds the roster from stage 0 (load_campaign_stage) and shows
// the confirm + stage banner dialogs. A cancelled picker, an unreadable file, or
// a zero-stage file leaves campaign mode untouched. Reached ONLY via
// present_setup's raw 'C'×5 trigger; not part of the AppState/AppInput flow
// graph, so run() returns void like the method it replaced.
class CampaignPickerScreen {
public:
    CampaignPickerScreen(ScreenContext ctx, CampaignState state, MatchBackdrop backdrop)
        : ctx_(ctx), state_(state), backdrop_(backdrop) {}
    void run();

private:
    ScreenContext ctx_;
    CampaignState state_;
    MatchBackdrop backdrop_;
};

// The campaign-activation confirmation dialog (was GameApp::present_campaign_confirm,
// sub_4015C6, docs/re/campaign.md "Campaign-activation confirmation dialog"): the
// REAL sub_43C734-chromed two-line modal the picker shows right after a
// successful pick/parse — getstring(95)="NOTE!" on top, getstring(1210)=
// "Campaign Mode Activated!" below. Dismiss keys mirror sub_414340's key loop
// exactly: Enter/Space/Escape confirm, every other key is a no-op (dialog stays
// up). Draws over the live match frame (MatchBackdrop). Called only by the
// picker, which is why no GameApp forwarder remains.
class CampaignConfirmScreen {
public:
    CampaignConfirmScreen(ScreenContext ctx, MatchBackdrop backdrop)
        : ctx_(ctx), backdrop_(backdrop) {}
    AppInput run();

private:
    ScreenContext ctx_;
    MatchBackdrop backdrop_;
};

// The campaign stage-start banner (was GameApp::present_campaign_banner,
// sub_40133F, docs/re/campaign.md "Stage banner"): a blocking two-line dialog,
// "(<stage name>)" (getstring 1235="(%s)") over "Prepare to begin Campaign!"
// (getstring 1230), shown once per stage transition (the first stage from the
// picker, every auto-advance from run_app's Results handler). Draws over the
// live match frame (MatchBackdrop). Dismissed by any confirm key or a short
// dwell.
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

// The "Congratulations! You made it through the whole campaign!" acknowledge
// modal (was GameApp::present_campaign_complete, sub_40133F stage-exhausted
// branch, getstring 1220/1225) shown once the last campaign stage is cleared,
// before returning to the menu. Draws over the live match frame (MatchBackdrop).
class CampaignCompleteScreen {
public:
    CampaignCompleteScreen(ScreenContext ctx, MatchBackdrop backdrop)
        : ctx_(ctx), backdrop_(backdrop) {}
    AppInput run();

private:
    ScreenContext ctx_;
    MatchBackdrop backdrop_;
};

// The in-round F1 help browser (was GameApp::present_help_browser_modal,
// docs/re/in-match-shell.md §1): the SAME generic *.BM help browser the menu
// row opens, but composited over the LAST rendered match frame (MatchBackdrop)
// instead of MAINMENU, with the sim frozen (this loop never ticks) — the
// sub_42A16F(1)/(0) bracket. Called by run_match's F1 key. Returns Quit on
// window close, else Advance.
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
