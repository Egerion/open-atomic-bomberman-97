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
// TWO things differ between openings, and both are parameters so the loop is
// not duplicated — sub_41431C is ONE routine at every F1 site:
//   * the BACKDROP: the main menu's row 5 composites over MAINMENU, the
//     in-round F1 over the frozen match frame (docs/re/in-match-shell.md §1's
//     sub_42A16F(1)/(0) bracket — that loop never ticks the sim), and the two
//     setup screens over their own live frame;
//   * the per-frame PUMP: the online setup/map-select screens must keep their
//     net link and the lobby heartbeat alive under the browser, or the peer
//     times the silent side out mid-help. Screens with nothing to pump pass
//     nothing. This parameter is what used to make those screens re-implement
//     the loop by hand — WITHOUT dispatch_list_mouse, so their F1 help was
//     keyboard-only against sub_42DBCC's mouse-first input model.
//
// `paint_backdrop` runs every frame after the clear and before the widget;
// `pump` runs once per frame before it. Returns Quit on window close, else
// Advance.
template <class PaintBackdrop, class Pump>
inline AppInput run_help_browser(const ScreenContext& ctx, PaintBackdrop paint_backdrop,
                                 Pump pump) {
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
        pump();
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

// The common no-pump opening.
template <class PaintBackdrop>
inline AppInput run_help_browser(const ScreenContext& ctx, PaintBackdrop paint_backdrop) {
    return run_help_browser(ctx, paint_backdrop, [] {});
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
