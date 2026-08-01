#include "bomber/ui/screen.hpp"

#include "bomber/game_util/anim_pace.hpp"
#include "bomber/render/sprites.hpp"

namespace bomber::game {

void Screen::enter(const ScreenDef& def, std::uint64_t now_ms) {
    def_ = def;
    entered_ms_ = now_ms;
    frame_ = 0;
    done_ = false;
}

bool Screen::on_key(SDL_Keycode key) {
    const bool accept = key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE ||
                        key == SDLK_ESCAPE;
    if (def_.wait == WaitLoop::TimedCut) {
        // No key loop at all behind a timed cut; report the accept so the caller
        // can still route an Escape, but make no sound the original does not.
        if (accept && def_.skippable) done_ = true;
        return accept;
    }
    audio_->play(20);
    if (!accept) return false;
    const bool silent_escape = def_.wait == WaitLoop::RoundEnd && key == SDLK_ESCAPE;
    if (!silent_escape) audio_->play(10);
    if (def_.skippable) done_ = true;
    return true;
}

void Screen::update(std::uint64_t now_ms) {
    ++frame_;
    // Both loops implement the dwell by forcing key = 13 and letting it fall
    // through the ordinary accept path, but at DIFFERENT POINTS (see WaitLoop's
    // addresses), and that changes what you hear: sub_42A088 overrides BEFORE
    // the no-key test, so the synthesized Enter blips AND stings; the round-end
    // loops override AFTER, so an idle dwell stings alone.
    if (def_.dwell_ms == 0 || done_ || now_ms - entered_ms_ < def_.dwell_ms) return;
    if (def_.wait != WaitLoop::TimedCut) {
        if (def_.wait == WaitLoop::AssetScreen) audio_->play(20);
        audio_->play(10);
    }
    done_ = true;
}

void Screen::draw(SDL_Renderer* ren) const {
    if (!ren) return;
    // A missing background leaves the cleared frame; a missing overlay sequence
    // simply draws nothing.
    const Sprite& bg = assets_->frontend_pcx(def_.background);
    if (bg.tex) {
        SDL_FRect dst{0.0f, 0.0f, static_cast<float>(bg.w), static_cast<float>(bg.h)};
        SDL_RenderTexture(ren, bg.tex, nullptr, &dst);
    }
    // Overlays: hotspot-anchored ANI frames, paced counter % statecnt like every
    // ANI (sub_41DAA7).
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
