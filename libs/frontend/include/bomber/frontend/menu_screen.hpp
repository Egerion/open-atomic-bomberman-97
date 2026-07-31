#pragma once

#include "bomber/frontend/menu_state.hpp"
#include "bomber/game_util/app_flow.hpp"  // AppInput
#include "bomber/ui/screen_context.hpp"

// The navigable main MENU screen (sub_42B9CE), extracted VERBATIM from GameApp
// (ADR-0009 §6): MAINMENU.PCX backdrop + an up/down highlight over the item
// rows (wrapping), Enter selects, Escape pops the quit-confirm — resolving the
// highlighted row into a concrete AppInput (StartMatch / OpenOptions / ... /
// Quit), or Quit on window close. It ALSO owns the ATTRACT idle timer
// (getvalue(92)) and the hidden triggers: F10 -> the port-only Video Settings
// panel, F1/row-5 -> the *.BM help browser, Alt+D -> the debug-info modal,
// Ctrl+E×6 -> the scheme editor, and the idle-timeout -> roll_attract_match() +
// StartMatch. run() is the outer event loop; roll_attract_match() (the attract
// entry, sub_4224E2) is private to it — GameApp::restore_from_attract stays a
// GameApp method because run_app calls it on every path back to the menu.
//
// Two seams, both stored BY VALUE: ScreenContext (the shared front-end
// services) and MenuState (the menu-specific mutable state GameApp still owns).

namespace bomber::game {

class MenuScreen {
public:
    MenuScreen(ScreenContext ctx, MenuState state) : ctx_(ctx), state_(state) {}
    AppInput run();

private:
    // Attract-mode entry — sub_4224E2's save + sub_410F81's attract branch
    // (docs/re/frontend-flow.md "Attract mode" point 1, pseudo.c 15125-15143).
    // Called ONLY by run()'s idle-timeout / Alt+A branches.
    void roll_attract_match();

    ScreenContext ctx_;
    MenuState state_;
};

}  // namespace bomber::game
