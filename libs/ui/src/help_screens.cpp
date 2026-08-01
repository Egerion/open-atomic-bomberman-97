#include "bomber/ui/help_screens.hpp"

#include <SDL3/SDL.h>

#include "bomber/render/asset_store.hpp"

namespace bomber::game {

namespace {

// MAINMENU is the persistent backdrop behind both the scroll window and the
// list: the original composites them over whatever screen was already up, and
// neither widget paints one of its own.
void draw_menu_art(const ScreenContext& ctx) {
    const Sprite& bg = ctx.assets.frontend_pcx("MAINMENU");
    if (!bg.tex) return;
    SDL_FRect d{0, 0, static_cast<float>(bg.w), static_cast<float>(bg.h)};
    SDL_RenderTexture(ctx.sdl, bg.tex, nullptr, &d);
}

}  // namespace

AppInput BmTextScreen::run(const std::string& bm_name) {
    // The `.BM` text-screen viewer (sub_41302D): the parsed text + inline images
    // over the menu art, keyboard line/page scroll, Enter/Escape to dismiss. No
    // auto-scroll or dwell — it waits for input exactly like the original.
    BmScreen bm(ctx_.assets, ctx_.front_font);
    bm.enter(bm_name);
    AppInput result = AppInput::Advance;
    while (!bm.done()) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
            if (ev.type != SDL_EVENT_KEY_DOWN) continue;
            if (ev.key.key == SDLK_ESCAPE) result = AppInput::Back;
            bm.on_key(ev.key.key);
        }
        ctx_.audio.update_music();
        SDL_SetRenderDrawColor(ctx_.sdl, 0, 0, 0, 255);
        SDL_RenderClear(ctx_.sdl);
        draw_menu_art(ctx_);
        bm.draw(ctx_.sdl);
        SDL_RenderPresent(ctx_.sdl);
        SDL_Delay(2);
    }
    // No wipe out: sub_41302D dismisses back to the menu by a cut, like every
    // sub_42A088-style screen — the menu is redrawn from scratch next frame.
    return result;
}

AppInput HelpBrowserScreen::run() {
    return run_help_browser(ctx_, [this] { draw_menu_art(ctx_); });
}

}  // namespace bomber::game
