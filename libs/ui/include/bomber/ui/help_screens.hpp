#pragma once

#include <SDL3/SDL.h>

#include <string>

#include "bomber/game_util/app_flow.hpp"
#include "bomber/ui/bmscreen.hpp"       // HelpBrowser
#include "bomber/ui/dialog_chrome.hpp"  // dispatch_list_mouse
#include "bomber/ui/screen_context.hpp"

// The `.BM` text-screen viewers. Each owns its own nested SDL event loop and
// returns the AppInput that ended it; MAINMENU stays the persistent backdrop,
// since the original composites the scroll window / list dialog over whatever
// screen was already up.

namespace bomber::game {

// sub_41302D: one `.BM` screen (Credits / Network / ...) with keyboard line and
// page scroll, Enter/Escape to dismiss.
class BmTextScreen {
public:
    explicit BmTextScreen(ScreenContext ctx) : ctx_(ctx) {}
    AppInput run(const std::string& bm_name);

private:
    ScreenContext ctx_;
};

// The help browser's whole nested loop — sub_41431C -> sub_414235 (docs/re/
// results-and-options.md §4): glob every *.BM in the install root, show the
// list, open the pick through the same viewer, and re-show the list on return
// (HelpBrowser owns that loop-back) until Esc cancels the list itself.
//
// The BACKDROP is the only thing that differs between the original's two
// openings, which is why it is a parameter and the loop is not duplicated: the
// main menu's row 5 composites over MAINMENU, and the in-round F1 over the
// frozen match frame (docs/re/in-match-shell.md §1's sub_42A16F(1)/(0) bracket —
// that loop never ticks the sim). sub_41431C is ONE routine either way.
//
// `paint_backdrop` runs every frame after the clear and before the widget.
// Returns Quit on window close, else Advance.
template <class PaintBackdrop>
inline AppInput run_help_browser(const ScreenContext& ctx, PaintBackdrop paint_backdrop) {
    HelpBrowser browser(ctx.assets, ctx.front_font);
    // getvalue(15) ("is the online manual enabled?", default 1, §4): gate BEFORE
    // the glob, matching sub_414235's own order.
    browser.enter(ctx.values.at_or(15, 1) != 0);
    while (!browser.done()) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
            // sub_42DBCC is mouse-first; the list's arrows, track and rows are
            // live widgets, not decoration.
            if (dispatch_list_mouse(ctx.sdl, ev, browser)) continue;
            if (ev.type != SDL_EVENT_KEY_DOWN) continue;
            browser.on_key(ev.key.key, ctx.audio);
        }
        if (browser.viewing() && browser.viewer().done()) browser.close_viewer();
        ctx.audio.update_music();
        SDL_SetRenderDrawColor(ctx.sdl, 0, 0, 0, 255);
        SDL_RenderClear(ctx.sdl);
        paint_backdrop();
        browser.draw(ctx.sdl);
        SDL_RenderPresent(ctx.sdl);
        SDL_Delay(2);
    }
    // No wipe out: the browser cuts back to its caller, like every
    // sub_42A088-style screen.
    return AppInput::Advance;
}

// The menu-row opening of the above, over MAINMENU.
class HelpBrowserScreen {
public:
    explicit HelpBrowserScreen(ScreenContext ctx) : ctx_(ctx) {}
    AppInput run();

private:
    ScreenContext ctx_;
};

}  // namespace bomber::game
