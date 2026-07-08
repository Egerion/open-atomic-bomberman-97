#include "bomber/game/goldman_screen.hpp"

#include <cmath>
#include <numbers>

namespace bomber::game {

// x(a) = cx + rx * cos(2*pi * freq_x * a / T); y(a) = cy - ry * sin(2*pi *
// freq_y * a / T) (doc §3 "Geometry", sub_403382/sub_40341F). Float math —
// presentation only, never the sim.
void GoldmanScreen::lissajous_xy(int a, int circle_steps, int cx, int cy, int rx, int ry,
                                 int freq_x, int freq_y, float& out_x, float& out_y) {
    const double t = static_cast<double>(circle_steps);
    const double theta = 2.0 * std::numbers::pi * a / t;
    out_x = static_cast<float>(cx) + static_cast<float>(rx * std::cos(theta * freq_x));
    out_y = static_cast<float>(cy) - static_cast<float>(ry * std::sin(theta * freq_y));
}

void GoldmanScreen::draw_anim_step(SDL_Renderer* ren, const Anim& a, float x, float y) const {
    if (a.steps.empty()) return;
    const Sprite& sp = a.steps[anim_step_index(frame_, a.steps.size())];
    if (!sp.tex) return;
    SDL_FRect d{x - static_cast<float>(sp.hx), y - static_cast<float>(sp.hy),
                static_cast<float>(sp.w), static_cast<float>(sp.h)};
    SDL_RenderTexture(ren, sp.tex, nullptr, &d);
}

void GoldmanScreen::draw_centered(SDL_Renderer* ren, const std::string& s, float cx,
                                  float y) const {
    if (!font_ || !font_->loaded() || s.empty()) return;
    float w = static_cast<float>(font_->measure(s));
    font_->draw(ren, s, cx - w / 2.0f, y, 255, 255, 255);
}

}  // namespace bomber::game
