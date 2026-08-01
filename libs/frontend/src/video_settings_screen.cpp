#include "bomber/frontend/video_settings_screen.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <string>

#include "bomber/game_util/scale_filter.hpp"
#include "bomber/render/asset_store.hpp"
#include "bomber/render/renderer.hpp"  // kScreenW
#include "bomber/render/sprites.hpp"   // set_scale_filter (the live soft-scaling apply)
#include "bomber/ui/dialog_chrome.hpp"

// PORT-ONLY screen (NOT RE'd) — the video/cadence toggles that otherwise only
// live on the F7/F8/F9 keys, surfaced as a small panel and persisted via the
// Video Settings keys (install.hpp). Kept SEPARATE from the RE'd Options screen
// so its exact 18 rows stay faithful: no invented rows there. Same WINZ-panel
// modal shape as the Alt+D debug window.

namespace bomber::game {

namespace {

constexpr int kRows = 4;
// "Soft scaling", not "Retro": it names the effect (a smoothed upscale) instead
// of implying the crisp default is the unfaithful one — the original scaled
// nothing at all (scale_filter.hpp).
constexpr const char* kRowLabels[kRows] = {"VSync", "Native cadence", "Show FPS", "Soft scaling"};

}  // namespace

void VideoSettingsScreen::run() {
    while (true) {
        if (!pump_events()) return;
        ctx_.audio.update_music();
        draw();
        SDL_RenderPresent(ctx_.sdl);
        SDL_Delay(2);
    }
}

// False means "close" (Escape or window close).
bool VideoSettingsScreen::pump_events() {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_EVENT_QUIT) return false;
        if (ev.type != SDL_EVENT_KEY_DOWN || ev.key.repeat) continue;
        ctx_.audio.play(20);  // nav blip
        if (ev.key.key == SDLK_ESCAPE) return false;
        if (ev.key.key == SDLK_UP) row_ = (row_ + kRows - 1) % kRows;
        if (ev.key.key == SDLK_DOWN) row_ = (row_ + 1) % kRows;
        if (is_toggle_key(ev.key.key)) toggle_row();
    }
    return true;
}

bool VideoSettingsScreen::is_toggle_key(SDL_Keycode k) {
    return k == SDLK_LEFT || k == SDLK_RIGHT || k == SDLK_RETURN || k == SDLK_KP_ENTER ||
           k == SDLK_SPACE;
}

// Every toggle applies LIVE and marks options_dirty so flush_options round-trips
// it. set_scale_filter re-stamps every already-uploaded classic texture, so the
// panel behind this frame is smoothed (or crisp again) on the very next present
// — no restart.
void VideoSettingsScreen::toggle_row() {
    switch (row_) {
        case 0:
            *toggles_.uncap_fps = !*toggles_.uncap_fps;  // "VSync" On == uncap OFF
            SDL_SetRenderVSync(ctx_.sdl, *toggles_.uncap_fps ? 0 : 1);
            break;
        case 1: *toggles_.native_cadence = !*toggles_.native_cadence; break;
        case 2: *toggles_.show_fps = !*toggles_.show_fps; break;
        default:
            *toggles_.soft_scaling = !*toggles_.soft_scaling;
            set_scale_filter(*toggles_.soft_scaling ? ScaleFilter::Soft : ScaleFilter::Crisp);
            break;
    }
    *toggles_.options_dirty = true;
}

void VideoSettingsScreen::draw() {
    SDL_SetRenderDrawColor(ctx_.sdl, 0, 0, 0, 255);
    SDL_RenderClear(ctx_.sdl);
    const Sprite& bg = ctx_.assets.frontend_pcx("MAINMENU");
    if (bg.tex) {
        SDL_FRect d{0, 0, static_cast<float>(bg.w), static_cast<float>(bg.h)};
        SDL_RenderTexture(ctx_.sdl, bg.tex, nullptr, &d);
    }
    const float lh = static_cast<float>(ctx_.front_font.line_height());
    // Tall enough for kRows rows plus the centred hint line, whatever FONT6's
    // line height is — a 4th row must not re-create the spill this panel already
    // had once. Floored at the original 190 so the 3-row look is unchanged on
    // this install's font.
    const float win_h =
        std::max(190.0f, 38.0f + 3.0f * lh + static_cast<float>(kRows) * (lh + 6.0f));
    const DialogRect win{(kScreenW - 380.0f) / 2.0f, 140.0f, 380.0f, win_h};
    draw_dialog_chrome(ctx_.sdl, win, &ctx_.assets.frontend_pcx("WINZ"));
    draw_dialog_text(ctx_.sdl, ctx_.front_font, "VIDEO SETTINGS (port)", win.x + 24.0f,
                     win.y + 16.0f, 255, 255, 255);
    draw_rows(win, lh);
    // Centred so it can't spill past the panel edge (the reported overflow).
    const std::string hint = "Enter toggle    Esc close";
    draw_dialog_text(ctx_.sdl, ctx_.front_font, hint,
                     win.x + (win.w - static_cast<float>(ctx_.front_font.measure(hint))) / 2.0f,
                     win.y + win.h - 16.0f - lh, 255, 255, 255);
}

void VideoSettingsScreen::draw_rows(const DialogRect& win, float lh) {
    const bool vals[kRows] = {!*toggles_.uncap_fps, *toggles_.native_cadence, *toggles_.show_fps,
                              *toggles_.soft_scaling};
    float ry = win.y + 16.0f + 2.0f * lh;
    for (int i = 0; i < kRows; ++i) {
        const bool sel = i == row_;
        const std::string shown =
            std::string(sel ? "> " : "  ") + kRowLabels[i] + ":  " + (vals[i] ? "On" : "Off");
        // Selected row yellow, others a dim white — the same read-at-a-glance
        // convention as the fps overlay's green/white.
        draw_dialog_text(ctx_.sdl, ctx_.front_font, shown, win.x + 24.0f, ry, sel ? 255 : 200,
                         sel ? 220 : 200, sel ? 80 : 200);
        ry += lh + 6.0f;
    }
}

}  // namespace bomber::game
