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
    // See WaitLoop for where each of these three behaviours is in the binary.
    const bool accept = key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE ||
                        key == SDLK_ESCAPE;
    if (def_.wait == WaitLoop::TimedCut) {
        // No key loop at all under a timed cut — the display is a blocking
        // delay. Report the accept so the caller can still route an Escape,
        // but make no sound the original does not make.
        if (accept && def_.skippable) done_ = true;
        return accept;
    }
    audio_->play(20);
    if (accept) {
        const bool silent_escape = def_.wait == WaitLoop::RoundEnd && key == SDLK_ESCAPE;
        if (!silent_escape) audio_->play(10);
        if (def_.skippable) done_ = true;
    }
    return accept;
}

void Screen::update(std::uint64_t now_ms) {
    ++frame_;
    // Dwell timeout: both loops implement it by forcing key = 13 (Enter) and
    // letting it fall through the ordinary accept path, but they do it at
    // DIFFERENT POINTS, and that changes what you hear.
    //
    // sub_42A088 overrides FIRST (0x42A117) and only then tests the key against
    // the -1/-2 no-key codes (0x42A11E), so the synthesized 13 is seen as a real
    // key: the blip fires, then the accept sting. Both, exactly like a manual
    // press.
    //
    // The round-end loops read the key and blip on it (0x42A73F-0x42A75A /
    // 0x42ADE9-0x42AE04) and only override AFTERWARDS (0x42A79D / 0x42AE4C). On
    // an idle dwell the real key was -1, so the blip is SKIPPED and the accept
    // sting plays alone. The port used to play both on every screen.
    if (def_.dwell_ms != 0 && !done_ && now_ms - entered_ms_ >= def_.dwell_ms) {
        if (def_.wait != WaitLoop::TimedCut) {
            if (def_.wait == WaitLoop::AssetScreen) audio_->play(20);
            audio_->play(10);
        }
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
