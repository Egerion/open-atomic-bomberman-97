#include "bomber/game/renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace bomber::game {

void Renderer::draw_sprite(const Sprite& sp, float x, float y, Uint8 r, Uint8 g, Uint8 b) {
    if (!sp.tex) return;
    SDL_FRect dst{x - sp.hx, y - sp.hy, static_cast<float>(sp.w), static_cast<float>(sp.h)};
    bool tinted = r != 255 || g != 255 || b != 255;
    if (tinted) SDL_SetTextureColorMod(sp.tex, r, g, b);
    SDL_RenderTexture(ren_, sp.tex, nullptr, &dst);
    if (tinted) SDL_SetTextureColorMod(sp.tex, 255, 255, 255);
}

void Renderer::draw_anim(const Anim& a, std::size_t step, float x, float y, Uint8 r, Uint8 g,
                         Uint8 b) {
    if (a.steps.empty()) return;
    draw_sprite(a.steps[step % a.steps.size()], x, y, r, g, b);
}

std::size_t Renderer::timed_step(const Anim& a, int remaining, int total) {
    if (a.steps.empty() || total <= 0) return 0;
    int elapsed = total - remaining;
    if (elapsed < 0) elapsed = 0;
    std::size_t idx =
        static_cast<std::size_t>(elapsed) * a.steps.size() / static_cast<std::size_t>(total);
    return idx < a.steps.size() ? idx : a.steps.size() - 1;
}

Uint8 Renderer::disease_flash_channel() {
    flash_lcg_ = flash_lcg_ * 1664525u + 1013904223u;
    return static_cast<Uint8>(48 + ((flash_lcg_ >> 16) % 208));  // 48..255, always lively
}

void Renderer::reset_match() {
    deaths_.clear();
    last_tick_ = ~0ull;
    hurry_until_ = 0;
}

void Renderer::on_events(const sim::State& s) {
    for (const auto& ev : s.events) {
        switch (ev.type) {
            case sim::Event::Type::Hurry:
                hurry_until_ = s.tick + 60;  // ~3 s of flashing banner
                break;
            case sim::Event::Type::PlayerDied: {
                const auto& pool = assets_->deaths_for(ev.player);
                if (pool.empty()) break;
                DeathFx fx;
                fx.player = ev.player;
                fx.anim = static_cast<std::size_t>(s.tick + ev.player * 7u) % pool.size();
                fx.x = kFieldOriginX +
                       s.players[ev.player].x / static_cast<float>(sim::kScale);
                fx.y = kFieldOriginY +
                       s.players[ev.player].y / static_cast<float>(sim::kScale) +
                       sim::kTileH / 2.0f - 1.0f;
                fx.start = s.tick;
                deaths_.push_back(fx);
                break;
            }
            default: break;
        }
    }
}

void Renderer::sample_movement(const sim::State& s) {
    if (s.tick == last_tick_) return;
    for (int i = 0; i < sim::kMaxPlayers; ++i) {
        const sim::Player& p = s.players[i];
        bool m = last_tick_ != ~0ull && p.present && p.alive &&
                 (p.x != last_x_[i] || p.y != last_y_[i]);
        moving_[i] = m;
        if (m) ++walk_phase_[i];
        last_x_[i] = p.x;
        last_y_[i] = p.y;
    }
    last_tick_ = s.tick;
}

void Renderer::draw_powerups(const sim::State& s) {
    for (int y = 0; y < sim::kGridHeight; ++y) {
        for (int x = 0; x < sim::kGridWidth; ++x) {
            if (s.floor[y][x] == sim::PowerupType::None) continue;
            const Sprite& p = assets_->powerup(static_cast<int>(s.floor[y][x]));
            if (!p.tex) continue;
            SDL_FRect dst{tile_screen_x(x), tile_screen_y(y), static_cast<float>(p.w),
                          static_cast<float>(p.h)};
            SDL_RenderTexture(ren_, p.tex, nullptr, &dst);
        }
    }
}

void Renderer::draw_world(const sim::State& s) {
    const SequenceSet& q = *seqs_;
    std::size_t pulse = static_cast<std::size_t>(s.tick);

    // Static cells (anchored at bottom-center via their hotspots).
    for (int y = 0; y < sim::kGridHeight; ++y) {
        for (int x = 0; x < sim::kGridWidth; ++x) {
            float sx = tile_screen_x(x) + sim::kTileW / 2.0f;
            float sy = tile_screen_y(y) + sim::kTileH - 1.0f;
            if (s.cells[y][x] == sim::Cell::Solid) draw_anim(q.solid, 0, sx, sy);
            else if (s.cells[y][x] == sim::Cell::Brick) draw_anim(q.brick, 0, sx, sy);
            else if (s.burning[y][x] > 0)
                draw_anim(q.burn, timed_step(q.burn, s.burning[y][x], s.tuning.brick_burn_frames),
                          sx, sy);
        }
    }

    // Flames (cosmetic arm selection from neighbouring flame cells).
    for (int y = 0; y < sim::kGridHeight; ++y) {
        for (int x = 0; x < sim::kGridWidth; ++x) {
            if (s.flame[y][x] == 0) continue;
            auto lit = [&s](int tx, int ty) {
                return tx >= 0 && tx < sim::kGridWidth && ty >= 0 && ty < sim::kGridHeight &&
                       s.flame[ty][tx] > 0;
            };
            bool l = lit(x - 1, y), r = lit(x + 1, y), u = lit(x, y - 1), d = lit(x, y + 1);
            int owner = s.flame_owner[y][x];
            const FlameSet& fset = q.flames[owner < kLocalPlayers ? owner : 0];
            const Anim* a = &fset.center;
            if ((l || r) && !u && !d) {
                if (l && r) a = &fset.mid_h[(x + y) & 1];
                else if (l) a = &fset.tip_e;
                else a = &fset.tip_w;
            } else if ((u || d) && !l && !r) {
                if (u && d) a = &fset.mid_v[(x + y) & 1];
                else if (u) a = &fset.tip_s;
                else a = &fset.tip_n;
            }
            float sx = tile_screen_x(x) + sim::kTileW / 2.0f;
            float sy = tile_screen_y(y) + sim::kTileH - 1.0f;
            draw_anim(*a, timed_step(*a, s.flame[y][x], s.tuning.flame_frames), sx, sy);
        }
    }

    // Bombs (pulse at tick rate, owner-colored; airborne ones arc and wrap).
    for (const auto& b : s.bombs) {
        if (!b.active) continue;
        float bx = b.x / static_cast<float>(sim::kScale);
        float by = b.y / static_cast<float>(sim::kScale);
        float lift = 0.0f;
        if (b.flying && b.fly_total > 0) {
            float t = 1.0f - static_cast<float>(b.fly_ticks) / static_cast<float>(b.fly_total);
            lift = 4.0f * static_cast<float>(b.fly_arc) * t * (1.0f - t);
            const float fw = static_cast<float>(sim::kGridWidth * sim::kTileW);
            const float fh = static_cast<float>(sim::kGridHeight * sim::kTileH);
            bx = std::fmod(std::fmod(bx, fw) + fw, fw);
            by = std::fmod(std::fmod(by, fh) + fh, fh);
        }
        float sx = kFieldOriginX + bx;
        float sy = kFieldOriginY + by + sim::kTileH / 2.0f - 1.0f - lift;
        draw_anim(q.bomb[b.owner < kLocalPlayers ? b.owner : 0], pulse, sx, sy);
    }

    sample_movement(s);

    // Players, bottom-anchored, in Y order so lower players draw in front.
    std::array<int, sim::kMaxPlayers> order{};
    int n = 0;
    for (int i = 0; i < sim::kMaxPlayers; ++i)
        if (s.players[i].present && s.players[i].alive) order[n++] = i;
    std::sort(order.begin(), order.begin() + n,
              [&s](int a, int b) { return s.players[a].y < s.players[b].y; });
    for (int k = 0; k < n; ++k) {
        int i = order[k];
        const sim::Player& p = s.players[i];
        int dir = static_cast<int>(p.facing);
        float sx = kFieldOriginX + p.x / static_cast<float>(sim::kScale);
        float sy =
            kFieldOriginY + p.y / static_cast<float>(sim::kScale) + sim::kTileH / 2.0f - 1.0f;
        // Shadow ellipse centered on the ground point (half peeks below the feet).
        draw_anim(q.shadow, 0, sx, sy + 8.0f);
        int pv = i < kLocalPlayers ? i : 0;
        const Anim& a = moving_[i] ? q.walk[pv][dir] : q.stand[pv][dir];
        std::size_t ph = moving_[i] ? walk_phase_[i] : 0;
        // A diseased player's sprite strobes through random colours — the
        // original redraws it with rand()%10 while a disease-age bit is set
        // (sub_41F29B). Tint on alternating sim ticks so it visibly flashes.
        if (p.disease_timer > 0 && (s.tick & 1)) {
            draw_anim(a, ph, sx, sy, disease_flash_channel(), disease_flash_channel(),
                      disease_flash_channel());
        } else {
            draw_anim(a, ph, sx, sy);
        }
        if (p.carrying) {  // held bomb rides above the head
            int bo = p.carried_owner < kLocalPlayers ? p.carried_owner : 0;
            draw_anim(q.bomb[bo], pulse, sx, sy - 78.0f);
        }
    }

    // Death animations (cosmetic, play once, advance at sim tick rate).
    for (std::size_t di = 0; di < deaths_.size();) {
        auto& fx = deaths_[di];
        const std::vector<Anim>& pool = assets_->deaths_for(fx.player % kLocalPlayers);
        if (pool.empty()) {
            deaths_.erase(deaths_.begin() + static_cast<std::ptrdiff_t>(di));
            continue;
        }
        const Anim& a = pool[fx.anim % pool.size()];
        std::uint64_t step = s.tick - fx.start;
        if (step >= a.steps.size()) {
            deaths_.erase(deaths_.begin() + static_cast<std::ptrdiff_t>(di));
            continue;
        }
        draw_sprite(a.steps[step], fx.x, fx.y);
        ++di;
    }
}

void Renderer::draw_hud(const sim::State& s) {
    // HURRY! banner flashes center-screen (every-4-ticks blink).
    if (s.tick < hurry_until_ && ((s.tick >> 2) & 1) == 0)
        draw_anim(seqs_->hurry, 0, kScreenW / 2.0f, kScreenH / 2.0f);

    // Match clock at the original HUD position (VALUELST ids 110/111/112).
    const Anim& d = seqs_->digits;
    if (d.steps.size() < 11) return;
    int total = (s.ticks_left + sim::kTicksPerSecond - 1) / sim::kTicksPerSecond;
    int minutes = total / 60, seconds = total % 60;
    char text[16];
    std::snprintf(text, sizeof(text), "%d:%02d", minutes, seconds);
    float x = 525.0f;
    for (const char* ch = text; *ch; ++ch) {
        const Sprite& g = d.steps[*ch == ':' ? 10 : static_cast<std::size_t>(*ch - '0')];
        draw_sprite(g, x + g.hx, 36.0f);
        x += g.w + 4.0f;  // VALUELST 112: extra spacing between digits
    }
}

void Renderer::draw_frame(const sim::State& s) {
    SDL_SetRenderDrawColor(ren_, 0, 0, 0, 255);
    SDL_RenderClear(ren_);
    SDL_RenderTexture(ren_, assets_->field(), nullptr, nullptr);
    draw_powerups(s);
    draw_world(s);
    draw_hud(s);
}

}  // namespace bomber::game
