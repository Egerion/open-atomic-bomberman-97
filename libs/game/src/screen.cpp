#include "bomber/game/screen.hpp"

#include "bomber/game/anim_pace.hpp"
#include "bomber/game/sprites.hpp"

namespace bomber::game {

void Screen::enter(const ScreenDef& def, std::uint64_t now_ms) {
    def_ = def;
    entered_ms_ = now_ms;
    frame_ = 0;
    done_ = false;
    // No music here: sub_42A088 never starts a track. The boot/menu music is
    // owned by the caller (sub_42741E) and plays continuously across screens.
}

bool Screen::on_key(SDL_Keycode key) {
    // sub_42A088's wait loop: ANY real key first plays the nav blip (SOUNDLST
    // 20 == sub_427961(20)). Only the three accept keys Enter / Space / Escape
    // additionally play the accept sting (SOUNDLST 10 == sub_427961(10)) and end
    // the wait; every other key just blips and keeps the screen up. (In the
    // original, Escape/Enter/Space are the codes that reach the sound(10) accept
    // path; higher codes exit the wait without an accept — we model that as
    // "not accepted, no finish", collapsing to the same visible result: the
    // screen only advances on a real accept or the dwell timeout.)
    const bool accept = key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE ||
                        key == SDLK_ESCAPE;
    audio_->play(20);
    if (accept) {
        audio_->play(10);
        if (def_.skippable) done_ = true;
    }
    return accept;
}

void Screen::update(std::uint64_t now_ms) {
    ++frame_;
    // Dwell timeout == the original's getvalue(12) attract auto-advance. In
    // sub_42A088's wait loop the timeout does NOT just exit: it forces key=13
    // (Enter), which then falls through the SAME sound path a real accept takes
    // — `if (key != -1 && key != -2) sub_427961(20)` fires the nav blip, then
    // the key==13 branch reaches `sub_427961(10)`. So the synthesized-Enter
    // auto-advance plays BOTH SFX 20 (blip) and SFX 10 (accept), exactly like a
    // manual keypress. We reproduce that here so the dwell advance is audible,
    // not silent. (The blip/accept are one-shot SFX voices, so the looping boot
    // music is untouched — only the screen ends.)
    if (def_.dwell_ms != 0 && !done_ && now_ms - entered_ms_ >= def_.dwell_ms) {
        audio_->play(20);
        audio_->play(10);
        done_ = true;
    }
}

void Screen::draw(SDL_Renderer* ren) const {
    if (!ren) return;
    // Background: the full-screen front-end PCX, blitted 1:1 into the logical
    // surface. A missing image leaves the cleared frame (Screen skips it).
    const Sprite& bg = assets_->frontend_pcx(def_.background);
    if (bg.tex) {
        SDL_FRect dst{0.0f, 0.0f, static_cast<float>(bg.w), static_cast<float>(bg.h)};
        SDL_RenderTexture(ren, bg.tex, nullptr, &dst);
    }
    // Overlays: hotspot-anchored ANI frames, paced counter % statecnt like every
    // ANI (sub_41DAA7). An unresolved/empty sequence simply draws nothing.
    for (const auto& ov : def_.overlays) {
        if (!ov.ani) continue;
        Anim a = resolve_sequence(*ov.ani, ov.sequence);
        if (a.steps.empty()) continue;
        const Sprite& sp = a.steps[anim_step_index(frame_, a.steps.size())];
        if (!sp.tex) continue;
        SDL_FRect dst{static_cast<float>(ov.x - sp.hx), static_cast<float>(ov.y - sp.hy),
                      static_cast<float>(sp.w), static_cast<float>(sp.h)};
        SDL_RenderTexture(ren, sp.tex, nullptr, &dst);
    }
}

}  // namespace bomber::game
