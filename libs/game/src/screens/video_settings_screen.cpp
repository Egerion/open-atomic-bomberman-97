#include "bomber/game/screens/video_settings_screen.hpp"

#include <SDL3/SDL.h>

#include <string>

#include "bomber/game/asset_store.hpp"
#include "bomber/game/dialog_chrome.hpp"
#include "bomber/game/renderer.hpp"  // kScreenW

namespace bomber::game {

void VideoSettingsScreen::run() {
    // PORT-ONLY screen (NOT RE'd) — the video/cadence toggles that otherwise
    // only live on the F7/F8/F9 keys (show_fps_/uncap_fps_/native_cadence_),
    // surfaced as a small panel and persisted via the Video Settings keys
    // (install.hpp). Kept SEPARATE from the RE'd Options screen so its exact 18
    // rows stay faithful (no invented rows there — the deliberate design choice
    // for these modern-only settings). Same WINZ-panel modal shape as the
    // Alt+D debug window; Up/Down select, Enter/Space/Left/Right toggle, Esc
    // closes. Toggles apply live and mark options_dirty_ so flush_options
    // round-trips them.
    int row = 0;
    constexpr int kRows = 3;
    while (true) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return;
            if (ev.type != SDL_EVENT_KEY_DOWN || ev.key.repeat) continue;
            ctx_.audio.play(20);  // nav blip
            switch (ev.key.key) {
                case SDLK_UP: row = (row + kRows - 1) % kRows; break;
                case SDLK_DOWN: row = (row + 1) % kRows; break;
                case SDLK_LEFT:
                case SDLK_RIGHT:
                case SDLK_RETURN:
                case SDLK_KP_ENTER:
                case SDLK_SPACE:
                    if (row == 0) {
                        *toggles_.uncap_fps = !*toggles_.uncap_fps;  // "VSync" On == uncap OFF
                        SDL_SetRenderVSync(ctx_.sdl, *toggles_.uncap_fps ? 0 : 1);
                    } else if (row == 1) {
                        *toggles_.native_cadence = !*toggles_.native_cadence;
                    } else {
                        *toggles_.show_fps = !*toggles_.show_fps;
                    }
                    *toggles_.options_dirty = true;
                    break;
                case SDLK_ESCAPE:
                    return;
                default:
                    break;
            }
        }
        ctx_.audio.update_music();
        SDL_SetRenderDrawColor(ctx_.sdl, 0, 0, 0, 255);
        SDL_RenderClear(ctx_.sdl);
        const Sprite& bg = ctx_.assets.frontend_pcx("MAINMENU");
        if (bg.tex) {
            SDL_FRect d{0, 0, static_cast<float>(bg.w), static_cast<float>(bg.h)};
            SDL_RenderTexture(ctx_.sdl, bg.tex, nullptr, &d);
        }
        const DialogRect win{(kScreenW - 380.0f) / 2.0f, 140.0f, 380.0f, 190.0f};
        draw_dialog_chrome(ctx_.sdl, win, &ctx_.assets.frontend_pcx("WINZ"));
        const float lh = static_cast<float>(ctx_.front_font.line_height());
        draw_dialog_text(ctx_.sdl, ctx_.front_font, "VIDEO SETTINGS (port)", win.x + 24.0f,
                         win.y + 16.0f, 255, 255, 255);
        const char* labels[kRows] = {"VSync", "Native cadence", "Show FPS"};
        const bool vals[kRows] = {!*toggles_.uncap_fps, *toggles_.native_cadence,
                                  *toggles_.show_fps};
        float ry = win.y + 16.0f + 2.0f * lh;
        for (int i = 0; i < kRows; ++i) {
            const bool sel = (i == row);
            const std::string shown = std::string(sel ? "> " : "  ") + labels[i] + ":  " +
                                      (vals[i] ? "On" : "Off");
            // Selected row yellow, others a dim white — same read-at-a-glance
            // convention as the fps overlay's green/white.
            draw_dialog_text(ctx_.sdl, ctx_.front_font, shown, win.x + 24.0f, ry, sel ? 255 : 200,
                             sel ? 220 : 200, sel ? 80 : 200);
            ry += lh + 6.0f;
        }
        // Centred so it can't spill past the panel edge (the reported overflow).
        const std::string hint = "Enter toggle    Esc close";
        draw_dialog_text(ctx_.sdl, ctx_.front_font, hint,
                         win.x + (win.w - static_cast<float>(ctx_.front_font.measure(hint))) / 2.0f,
                         win.y + win.h - 16.0f - lh, 255, 255, 255);
        SDL_RenderPresent(ctx_.sdl);
        SDL_Delay(2);
    }
}

}  // namespace bomber::game
