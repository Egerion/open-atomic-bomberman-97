#pragma once

#include "bomber/game_util/app_flow.hpp"
#include "bomber/ui/dialog_chrome.hpp"  // DialogRect
#include "bomber/ui/screen_context.hpp"

// The hidden Alt+D "Internal debugging information" window (sub_413D45): a
// 450x300 WINZ-9-patch modal over the frozen MAINMENU backdrop, dismissed by
// Enter/Escape only.

namespace bomber::game {

class DebugInfoScreen {
public:
    explicit DebugInfoScreen(ScreenContext ctx) : ctx_(ctx) {}
    AppInput run();

private:
    void draw();
    void draw_stats(const DialogRect& win);

    ScreenContext ctx_;
};

}  // namespace bomber::game
