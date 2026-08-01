#include "bomber/frontend/goldman_screen.hpp"

#include <cmath>
#include <cstddef>
#include <numbers>

namespace bomber::game {

// x(a) = cx + rx*cos(2*pi*freq_x*a/T); y(a) = cy - ry*sin(2*pi*freq_y*a/T)
// (doc §3 "Geometry", sub_403382/sub_40341F). Float math — presentation only,
// never the sim.
SDL_FPoint GoldmanScreen::lissajous_xy(int angle, int circle_steps, const WheelGeometry& geo) {
    const double theta = 2.0 * std::numbers::pi * angle / static_cast<double>(circle_steps);
    return SDL_FPoint{
        static_cast<float>(geo.cx) + static_cast<float>(geo.rx * std::cos(theta * geo.freq_x)),
        static_cast<float>(geo.cy) - static_cast<float>(geo.ry * std::sin(theta * geo.freq_y))};
}

void GoldmanScreen::draw(SDL_Renderer* ren, const WheelGeometry& geo) const {
    if (!ren) return;
    draw_backdrop(ren);
    draw_prize_icons(ren, geo);
    // Pointer: ANI sequence "ring" at the ring position.
    draw_anim_step(ren, ring_anim_, lissajous_xy(wheel_.ring.pos, circle_steps_, geo));
    draw_result_text(ren, geo);
}

void GoldmanScreen::draw_backdrop(SDL_Renderer* ren) const {
    const Sprite& bg = assets_->frontend_pcx("ROULETTE");
    if (bg.tex == nullptr) {
        SDL_SetRenderDrawColor(ren, 10, 10, 20, 255);
        SDL_RenderClear(ren);
        return;
    }
    SDL_FRect d{0, 0, static_cast<float>(bg.w), static_cast<float>(bg.h)};
    SDL_RenderTexture(ren, bg.tex, nullptr, &d);
}

// The 6 prize icons at angles wheel + k*segment_steps (doc §3), drawn with the
// shared floor-powerup ANI — the wheel reuses the normal in-game icons, with no
// roulette-specific art. Clogs (13) draws its OWN icon; CONFIRMED that the
// original draws all 6 slots uniformly via sub_425C7F(x,y,kind) with no
// special-case skip for slot 13 (doc §9.4). It is simply not a sim::PowerupType
// (never a sim inventory kind, §8).
void GoldmanScreen::draw_prize_icons(SDL_Renderer* ren, const WheelGeometry& geo) const {
    for (int k = 0; k < kWheelSegments; ++k) {
        const int angle = wheel_.wheel.pos + k * segment_steps_;
        const SDL_FPoint at = lissajous_xy(angle, circle_steps_, geo);
        const int prize_id = kWheelPrizeIds[static_cast<std::size_t>(k)];
        if (prize_id == kClogsPrizeId) {
            draw_anim_step(ren, seqs_->clogs_anim, at);
            continue;
        }
        const sim::PowerupType pt = wheel_prize_to_powerup(prize_id);
        if (pt == sim::PowerupType::None) continue;
        draw_anim_step(ren, seqs_->powerup_anim[static_cast<int>(pt)], at);
    }
}

void GoldmanScreen::draw_result_text(SDL_Renderer* ren, const WheelGeometry& geo) const {
    if (wheel_.phase != WheelPhase::Settled || !font_ || !font_->loaded()) return;
    const float cx = static_cast<float>(geo.cx);
    const float ty = static_cast<float>(geo.cy) - 30.0f;
    draw_centered(ren, assets_->getstring(790, "YOU HAVE WON"), cx, ty);
    draw_centered(ren, assets_->getstring(800 + wheel_.result, "A PRIZE"), cx, ty + 20.0f);
    // getstring(791) is empty in the shipped file; draw_centered skips it.
    draw_centered(ren, assets_->getstring(791, ""), cx, ty + 40.0f);
}

// STATIC frame 0 (sub_425C7F / sub_41DAA7(v, 0)): both the 6 prize icons and the
// ring pointer are drawn at ANI frame 0 — they never per-sprite animate. The
// visible spin is the wheel ROTATION (the Lissajous position each is placed at),
// not a frame cycle, so feeding an incrementing frame counter here was wrong.
void GoldmanScreen::draw_anim_step(SDL_Renderer* ren, const Anim& a, SDL_FPoint at) const {
    if (a.steps.empty()) return;
    const Sprite& sp = a.steps[0];
    if (!sp.tex) return;
    SDL_FRect d{at.x - static_cast<float>(sp.hx), at.y - static_cast<float>(sp.hy),
                static_cast<float>(sp.w), static_cast<float>(sp.h)};
    SDL_RenderTexture(ren, sp.tex, nullptr, &d);
}

void GoldmanScreen::draw_centered(SDL_Renderer* ren, const std::string& s, float cx,
                                  float y) const {
    if (!font_ || !font_->loaded() || s.empty()) return;
    const float w = static_cast<float>(font_->measure(s));
    font_->draw(ren, s, cx - w / 2.0f, y, 255, 255, 255);
}

}  // namespace bomber::game
