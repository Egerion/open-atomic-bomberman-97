#include "bomber/game/transition.hpp"

#include "bomber/game/anim_pace.hpp"
#include "bomber/game/sprites.hpp"

namespace bomber::game {

namespace {

// HEADWIPE.ANI holds a single wipe sequence (docs/re/frontend-flow.md). Resolve
// the first non-empty sequence in the file rather than hard-coding its label,
// so the primitive stays robust to the exact CHFILEANI sequence name.
Anim first_sequence(const AniTextures& ani) {
    for (const auto& s : ani.data().sequences) {
        Anim a = resolve_sequence(ani, s.name);
        if (!a.steps.empty()) return a;
    }
    return {};
}

}  // namespace

void Transition::start(std::uint64_t now_ms) {
    active_ = true;
    started_ms_ = now_ms;
    frame_ = 0;
    Anim wipe = first_sequence(assets_->headwipe());
    steps_ = wipe.steps.size();
    use_wipe_ = steps_ != 0;  // no HEADWIPE frames -> fall back to the fade
}

void Transition::update(std::uint64_t now_ms) {
    if (!active_) return;
    ++frame_;
    if (use_wipe_) {
        // One step per rendered frame (the original advances the ANI counter per
        // frame, sub_41DAA7); complete once every step has shown once.
        if (frame_ >= steps_) active_ = false;
    } else {
        if (now_ms - started_ms_ >= kFadeMs) active_ = false;
    }
}

void Transition::draw(SDL_Renderer* ren, std::uint64_t now_ms) const {
    if (!active_ || !ren) return;
    if (use_wipe_) {
        Anim wipe = first_sequence(assets_->headwipe());
        if (wipe.steps.empty()) return;
        const Sprite& sp = wipe.steps[anim_step_index(frame_, wipe.steps.size())];
        if (!sp.tex) return;
        // The wipe mask is authored full-screen; blit it 1:1 over the scene.
        SDL_FRect dst{static_cast<float>(-sp.hx), static_cast<float>(-sp.hy),
                      static_cast<float>(sp.w), static_cast<float>(sp.h)};
        SDL_RenderTexture(ren, sp.tex, nullptr, &dst);
    } else {
        // Fallback: a linear alpha fade to black over the whole logical surface,
        // driven by elapsed wall time (kFadeMs total).
        std::uint64_t elapsed = now_ms - started_ms_;
        Uint8 alpha =
            static_cast<Uint8>((elapsed >= kFadeMs) ? 255u : (elapsed * 255u / kFadeMs));
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, alpha);
        SDL_RenderFillRect(ren, nullptr);
    }
}

}  // namespace bomber::game
