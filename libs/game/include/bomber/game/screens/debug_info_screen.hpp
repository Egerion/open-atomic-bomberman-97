#pragma once

#include "bomber/game/app_flow.hpp"
#include "bomber/game/screen_context.hpp"

// The hidden Alt+D "Internal debugging information" window (sub_413D45),
// extracted verbatim from GameApp::present_debug_info_modal (ADR-0008): a
// 450x300 WINZ-9-patch modal over the frozen MAINMENU backdrop, dismissed by
// Enter/Escape only.

namespace bomber::game {

class DebugInfoScreen {
public:
    explicit DebugInfoScreen(ScreenContext ctx) : ctx_(ctx) {}
    AppInput run();

private:
    ScreenContext ctx_;
};

}  // namespace bomber::game
