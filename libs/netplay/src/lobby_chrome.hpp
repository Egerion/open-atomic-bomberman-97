#pragma once

#include <SDL3/SDL.h>

#include <string>

#include "bomber/render/sprites.hpp"    // Sprite
#include "bomber/ui/dialog_chrome.hpp"  // the pinned chrome primitives + inks
#include "bomber/ui/screen_context.hpp"

// The chrome libs/netplay's menu screens share. INTERNAL to the package (it
// lives in src/, not include/): the lobby screens and the direct connect modals
// draw the same picture — the MAINMENU backdrop with one RE'd dialog primitive
// over it — and carried byte-identical copies of it in two files.

namespace bomber::game {

// The front end's SFX ids (frontend-flow.md §SFX). 40 is the net "you can't do
// that here" buzz, which is a guest pressing an edit key the host owns.
inline constexpr int kSfxBlip = 20;
inline constexpr int kSfxAccept = 10;
inline constexpr int kSfxDenied = 40;

// renderer.hpp's kScreenW — what the centred dialogs measure against.
inline constexpr float kScreenW = 640.0f;

inline void draw_lobby_backdrop(ScreenContext ctx) {
    SDL_SetRenderDrawColor(ctx.sdl, 0, 0, 0, 255);
    SDL_RenderClear(ctx.sdl);
    const Sprite& bg = ctx.assets.frontend_pcx("MAINMENU");
    if (bg.tex == nullptr) return;
    SDL_FRect d{0, 0, static_cast<float>(bg.w), static_cast<float>(bg.h)};
    SDL_RenderTexture(ctx.sdl, bg.tex, nullptr, &d);
}

// One centred line of the pinned outlined dialog text (sub_41696C).
inline void draw_centred(ScreenContext ctx, const std::string& s, float y) {
    const float w = static_cast<float>(ctx.front_font.measure(s));
    draw_dialog_text(ctx.sdl, ctx.front_font, s, (kScreenW - w) / 2.0f, y, kDialogInkR, kDialogInkG,
                     kDialogInkB);
}

// The sub_414340 acknowledge modal's two chrome pieces, resolved once per screen
// rather than once per frame.
struct AckChrome {
    const Sprite* winz = nullptr;
    std::string ok_label;
};

inline AckChrome ack_chrome(ScreenContext ctx) {
    return AckChrome{&ctx.assets.frontend_pcx("WINZ"), ctx.assets.getstring(27, " Ok ")};
}

inline void draw_ack(ScreenContext ctx, const AckChrome& c, const std::string& top,
                     const std::string& body) {
    draw_acknowledge_dialog(ctx.sdl, ctx.front_font, c.winz, top, body, c.ok_label, kDialogInkR,
                            kDialogInkG, kDialogInkB);
}

// sub_414340's own key loop: the modal closes on Enter / Space / Esc only, and
// blips on any other real key.
inline bool ack_dismiss_key(ScreenContext ctx, SDL_Keycode key) {
    if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE || key == SDLK_ESCAPE)
        return true;
    ctx.audio.play(kSfxBlip);
    return false;
}

}  // namespace bomber::game
