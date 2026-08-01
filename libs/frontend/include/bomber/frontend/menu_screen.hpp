#pragma once

#include "bomber/frontend/menu_state.hpp"
#include "bomber/game_util/app_flow.hpp"  // AppInput
#include "bomber/ui/screen_context.hpp"

// The navigable main MENU screen (sub_42B9CE), resolving the selected row into a
// concrete AppInput. It also owns the ATTRACT idle timer and the hidden triggers.
// docs/frontend-menu.md has the row table and the RE pins.
//
// restore_from_attract stays a shell method because the app calls it on every
// path back to the menu, not just this screen's.

namespace bomber::game {

class MenuScreen {
public:
    MenuScreen(ScreenContext ctx, MenuState state) : ctx_(ctx), state_(state) {}
    AppInput run();

private:
    ScreenContext ctx_;
    MenuState state_;
};

}  // namespace bomber::game
