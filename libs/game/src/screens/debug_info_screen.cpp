#include "bomber/game/screens/debug_info_screen.hpp"

#include <SDL3/SDL.h>

#include <string>

#include "bomber/game/asset_store.hpp"
#include "bomber/game/dialog_chrome.hpp"
#include "bomber/game/hud_format.hpp"
#include "bomber/game/renderer.hpp"  // kScreenW

namespace bomber::game {

AppInput DebugInfoScreen::run() {
    // sub_413D45 (declaration doc): the hidden Alt+D "Internal debugging
    // information" window — a 450x300 WINZ-9-patch panel at y=100, centred on
    // x, over the frozen menu backdrop; Enter/Escape dismiss it, nothing else
    // does. The original's stat values (heap/audio memory, net id, retransmit
    // rate, audio cache hits) have no port equivalents — labels are the real
    // getstring rows, values honest placeholders.
    while (true) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
            if (ev.type != SDL_EVENT_KEY_DOWN) continue;
            if (!ev.key.repeat) ctx_.audio.play(20);  // any-real-key blip
            if (ev.key.key == SDLK_RETURN || ev.key.key == SDLK_KP_ENTER ||
                ev.key.key == SDLK_ESCAPE)
                return AppInput::Advance;
        }
        ctx_.audio.update_music();
        SDL_SetRenderDrawColor(ctx_.sdl, 0, 0, 0, 255);
        SDL_RenderClear(ctx_.sdl);
        const Sprite& bg = ctx_.assets.frontend_pcx("MAINMENU");
        if (bg.tex) {
            SDL_FRect d{0, 0, static_cast<float>(bg.w), static_cast<float>(bg.h)};
            SDL_RenderTexture(ctx_.sdl, bg.tex, nullptr, &d);
        }
        const DialogRect win{(kScreenW - 450.0f) / 2.0f, 100.0f, 450.0f, 300.0f};
        draw_dialog_chrome(ctx_.sdl, win, &ctx_.assets.frontend_pcx("WINZ"));
        const float lh = static_cast<float>(ctx_.front_font.line_height());
        float ty = win.y + 16.0f;
        auto line = [&](const std::string& s) {
            draw_dialog_text(ctx_.sdl, ctx_.front_font, s, win.x + 24.0f, ty, 255, 255, 255);
            ty += lh + 6.0f;
        };
        line(ctx_.assets.getstring(400, "Internal debugging information"));
        ty += lh;
        line(fmt_u(ctx_.assets.getstring(405, "Total memory usage: %u"), 0));
        line(fmt_u(ctx_.assets.getstring(410, "Audio memory usage: %u"), 0));
        line(fmt_u(ctx_.assets.getstring(411, "Audio cache hits: %u"), 0));
        line(fmt_u(ctx_.assets.getstring(415, "Network id: %u"), 0));
        line(fmt_u(ctx_.assets.getstring(420, "Retransmit rate: %u"), 0));
        ty = win.y + win.h - 16.0f - lh;
        draw_dialog_text(ctx_.sdl, ctx_.front_font,
                         ctx_.assets.getstring(401, "Press [Enter] or [Esc] to continue"),
                         win.x + 24.0f, ty, 255, 255, 255);
        SDL_RenderPresent(ctx_.sdl);
        SDL_Delay(2);
    }
}

}  // namespace bomber::game
