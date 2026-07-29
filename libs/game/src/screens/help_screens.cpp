#include "bomber/game/screens/help_screens.hpp"

#include <SDL3/SDL.h>

#include "bomber/game/asset_store.hpp"
#include "bomber/game/bmscreen.hpp"
#include "bomber/game/dialog_chrome.hpp"  // dispatch_list_mouse

namespace bomber::game {

AppInput BmTextScreen::run(const std::string& bm_name) {
    // The `.BM` text-screen viewer (sub_41302D): MAINMENU.PCX as the persistent
    // backdrop (the original composites the scroll window over the menu page),
    // the parsed .BM text + inline images over it, keyboard line/page scroll,
    // and Enter/Escape to dismiss. No auto-scroll or dwell — it waits for input
    // exactly like the original.
    BmScreen bm(ctx_.assets, ctx_.front_font);
    bm.enter(bm_name);
    AppInput result = AppInput::Advance;
    while (!bm.done()) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
            if (ev.type == SDL_EVENT_KEY_DOWN) {
                if (ev.key.key == SDLK_ESCAPE) result = AppInput::Back;
                bm.on_key(ev.key.key);
            }
        }
        ctx_.audio.update_music();
        SDL_SetRenderDrawColor(ctx_.sdl, 0, 0, 0, 255);
        SDL_RenderClear(ctx_.sdl);
        // Backdrop: keep the menu art behind the text panel.
        const Sprite& bg = ctx_.assets.frontend_pcx("MAINMENU");
        if (bg.tex) {
            SDL_FRect d{0, 0, static_cast<float>(bg.w), static_cast<float>(bg.h)};
            SDL_RenderTexture(ctx_.sdl, bg.tex, nullptr, &d);
        }
        bm.draw(ctx_.sdl);
        SDL_RenderPresent(ctx_.sdl);
        SDL_Delay(2);
    }

    // No wipe out: the .BM viewer (sub_41302D) dismisses back to the menu by a
    // cut, like every sub_42A088-style screen — the menu is redrawn from scratch
    // on the next frame. No screen-to-screen transition here.
    return result;
}

AppInput HelpBrowserScreen::run() {
    // sub_41431C -> sub_414235 (docs/re/results-and-options.md §4): glob every
    // *.BM in the install root, show the list, open the pick through the same
    // .BM viewer, and re-show the list on return (HelpBrowser owns that
    // loop-back internally) until Esc cancels the list itself. MAINMENU stays
    // the persistent backdrop behind both the list and the viewer, matching
    // BmTextScreen's own convention (the original composites over whatever
    // screen was already up — the menu here, the live match field at the
    // in-round F1 call site, where the caller paints its own frame first).
    HelpBrowser browser(ctx_.assets, ctx_.front_font);
    // getvalue(15) ("is the online manual enabled?", default 1, §4): gate
    // BEFORE the glob, matching sub_414235's own order.
    browser.enter(ctx_.values.at_or(15, 1) != 0);
    while (!browser.done()) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
            // sub_42DBCC is mouse-first; the list's own arrows, track and rows
            // are live widgets, not decoration.
            if (dispatch_list_mouse(ctx_.sdl, ev, browser)) continue;
            if (ev.type != SDL_EVENT_KEY_DOWN) continue;
            browser.on_key(ev.key.key, ctx_.audio);
        }
        if (browser.viewing() && browser.viewer().done()) browser.close_viewer();
        ctx_.audio.update_music();
        SDL_SetRenderDrawColor(ctx_.sdl, 0, 0, 0, 255);
        SDL_RenderClear(ctx_.sdl);
        const Sprite& bg = ctx_.assets.frontend_pcx("MAINMENU");
        if (bg.tex) {
            SDL_FRect d{0, 0, static_cast<float>(bg.w), static_cast<float>(bg.h)};
            SDL_RenderTexture(ctx_.sdl, bg.tex, nullptr, &d);
        }
        browser.draw(ctx_.sdl);
        SDL_RenderPresent(ctx_.sdl);
        SDL_Delay(2);
    }
    // No wipe out — same cut-back-to-caller convention as BmTextScreen.
    return AppInput::Advance;
}

}  // namespace bomber::game
