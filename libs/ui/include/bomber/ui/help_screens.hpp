#pragma once

#include <string>

#include "bomber/game_util/app_flow.hpp"
#include "bomber/ui/screen_context.hpp"

// The `.BM` text-screen viewers, extracted verbatim from GameApp (ADR-0008
// god-object decomposition): each owns its own nested SDL event loop and
// returns the AppInput that ended it, exactly like the GameApp method it
// replaced. MAINMENU stays the persistent backdrop (the original composites the
// scroll window / list dialog over whatever screen was already up).

namespace bomber::game {

// sub_41302D: one `.BM` screen (Credits / Network / ...) over MAINMENU with
// keyboard line/page scroll, Enter/Escape to dismiss. Was
// GameApp::present_bm_screen.
class BmTextScreen {
public:
    explicit BmTextScreen(ScreenContext ctx) : ctx_(ctx) {}
    AppInput run(const std::string& bm_name);

private:
    ScreenContext ctx_;
};

// sub_41431C -> sub_414235: the generic *.BM help browser — glob every *.BM,
// list them, open the pick through the same viewer, MAINMENU backdrop, until
// Esc cancels the list. Was GameApp::present_help_browser.
class HelpBrowserScreen {
public:
    explicit HelpBrowserScreen(ScreenContext ctx) : ctx_(ctx) {}
    AppInput run();

private:
    ScreenContext ctx_;
};

}  // namespace bomber::game
