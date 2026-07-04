#include "bomber/game/renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "bomber/game/anim_pace.hpp"

namespace bomber::game {

// How long a kick/punch pose stays up before returning to walk/stand. The
// KICK/PUNCH sequences are short, so ~8 ticks (~0.4 s) reads cleanly.
constexpr int kActionPoseTicks = 8;

// Spread of the idle-fidget duration: the original rolls 20 + rand()%getvalue(330)
// ticks per fidget (sub_41F29B). getvalue id 330 is not yet read from VALUELST —
// this stands in for it and stays tunable. See docs/valuelst-map.md id 330.
constexpr int kPanicSpread = 40;

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
    // frame = counter % statecnt — the original ANI player (sub_41DAA7). The
    // STAT HEAD timing field is inert; see anim_pace.hpp / docs/re/facts.md.
    draw_sprite(a.steps[anim_step_index(step, a.steps.size())], x, y, r, g, b);
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

std::uint32_t Renderer::panic_roll() {
    panic_lcg_ = panic_lcg_ * 1664525u + 1013904223u;
    return panic_lcg_ >> 16;
}

bool Renderer::boxed_in(const sim::State& s, int tx, int ty) {
    // "Blocked" = not walkable: outside the grid, a solid/brick/burning tile,
    // or a resting (non-flying) bomb sitting on it. Mirrors the original's
    // neighbour scan in sub_41F29B.
    static constexpr int dx[4] = {0, 0, -1, 1};
    static constexpr int dy[4] = {-1, 1, 0, 0};
    for (int k = 0; k < 4; ++k) {
        int nx = tx + dx[k], ny = ty + dy[k];
        if (nx < 0 || nx >= sim::kGridWidth || ny < 0 || ny >= sim::kGridHeight) continue;
        bool blocked = s.cells[ny][nx] == sim::Cell::Solid ||
                       s.cells[ny][nx] == sim::Cell::Brick || s.burning[ny][nx] > 0;
        if (!blocked) {
            for (const auto& b : s.bombs) {
                if (b.active && !b.flying && b.tile_x() == nx && b.tile_y() == ny) {
                    blocked = true;
                    break;
                }
            }
        }
        if (!blocked) return false;  // an open neighbour => not boxed in
    }
    return true;
}

void Renderer::reset_match() {
    deaths_.clear();
    last_tick_ = ~0ull;
    hurry_until_ = 0;
    kick_pose_.fill(0);
    punch_pose_.fill(0);
    panic_ticks_.fill(0);
}

void Renderer::on_events(const sim::State& s) {
    // Age the action poses once per tick (on_events is called exactly once per
    // sim tick, before the frame is drawn).
    for (int i = 0; i < sim::kMaxPlayers; ++i) {
        if (kick_pose_[i] > 0) --kick_pose_[i];
        if (punch_pose_[i] > 0) --punch_pose_[i];
    }
    for (const auto& ev : s.events) {
        switch (ev.type) {
            case sim::Event::Type::Hurry:
                hurry_until_ = s.tick + 60;  // ~3 s of flashing banner
                break;
            case sim::Event::Type::BombKicked:
                if (ev.player >= 0 && ev.player < sim::kMaxPlayers)
                    kick_pose_[ev.player] = kActionPoseTicks;
                break;
            case sim::Event::Type::BombPunched:
                if (ev.player >= 0 && ev.player < sim::kMaxPlayers)
                    punch_pose_[ev.player] = kActionPoseTicks;
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
        // Advance the leg cycle by DISTANCE travelled, not once per tick: the
        // original holds a 16.16 walk phase that increments with the character's
        // speed and indexes the frame via phase >> 16 (sub_41F29B); its rover
        // mover (sub_401B5C) likewise bumps the anim counter once per movement
        // step. So one walk frame ~ one pixel of travel. We approximate the
        // engine's per-tick speed with the actual Manhattan pixel delta this
        // tick (sim pos is pixels*kScale) and add it to walk_phase_; draw_anim's
        // `% statecnt` then picks the frame. Purely render-side.
        if (m) {
            std::uint32_t dpx =
                static_cast<std::uint32_t>((std::abs(p.x - last_x_[i]) +
                                            std::abs(p.y - last_y_[i])) /
                                           sim::kScale);
            if (dpx == 0) dpx = 1;  // a sub-pixel step still nudges the cycle
            walk_phase_[i] += dpx;
        }
        last_x_[i] = p.x;
        last_y_[i] = p.y;

        // Idle-fidget bookkeeping (once per sim tick). While a live player is
        // standing still and fully boxed in, keep re-rolling "cornerhead"
        // fidgets; the instant it can move again (or moves), cancel. Cosmetic —
        // the rolls come off panic_lcg_, never State::rng (determinism intact).
        bool panic = p.present && p.alive && !m && boxed_in(s, p.tile_x(), p.tile_y());
        if (panic) {
            if (--panic_ticks_[i] <= 0) {
                panic_variant_[i] = static_cast<int>(panic_roll() % kCornerheadVariants);
                panic_ticks_[i] = 20 + static_cast<int>(panic_roll() % kPanicSpread);
            }
        } else {
            panic_ticks_[i] = 0;
        }
    }
    last_tick_ = s.tick;
}

void Renderer::draw_actors(const sim::State& s) {
    // Conveyor + trampoline floor tiles, drawn UNDER powerups and entities (the
    // lowest floor layer above the field). Anchored bottom-centre like every
    // other tile. The conveyor scroll advances at counter/3 (a slow belt, per
    // the original's actor animator sub_4056CA); the trampoline rides the tick
    // pulse. Missing sequences simply draw nothing. See docs/re/stage-actors.md.
    const SequenceSet& q = *seqs_;
    const std::size_t belt_step = static_cast<std::size_t>(s.tick / 3);
    const std::size_t pulse = static_cast<std::size_t>(s.tick);
    for (int y = 0; y < sim::kGridHeight; ++y) {
        for (int x = 0; x < sim::kGridWidth; ++x) {
            sim::ActorType at = s.actor_type[y][x];
            if (at == sim::ActorType::None) continue;
            float sx = tile_screen_x(x) + sim::kTileW / 2.0f;
            float sy = tile_screen_y(y) + sim::kTileH - 1.0f;
            if (at == sim::ActorType::Conveyor) {
                int g = s.actor_dir[y][x] & 3;
                if (!q.conveyor[g].steps.empty()) draw_anim(q.conveyor[g], belt_step, sx, sy);
            } else if (at == sim::ActorType::Trampoline) {
                if (!q.trampoline.steps.empty()) draw_anim(q.trampoline, pulse, sx, sy);
            }
            // DirArrow (0) / Warphole (1) floor art is deferred with their
            // mechanics (stage-actors.md §5); their sequences ("extra arrow
            // <dir>", "extra warp 1") are already resolvable when wired.
        }
    }
}

void Renderer::draw_powerups(const sim::State& s) {
    const SequenceSet& q = *seqs_;
    for (int y = 0; y < sim::kGridHeight; ++y) {
        for (int x = 0; x < sim::kGridWidth; ++x) {
            if (s.floor[y][x] == sim::PowerupType::None) continue;
            int kind = static_cast<int>(s.floor[y][x]);
            // The original draws floor powerups with the ANIMATED "power <name>"
            // sequence (POWERS.ANI) at (tile-centre-x, tile-bottom-y) minus the
            // sprite hotspot — the same bottom-centre anchor as tiles/bricks —
            // advancing frame = counter % statecnt (sub_4250DE / sub_41DAA7).
            // See docs/re/facts.md "Screen geometry". A shared per-tick pulse is
            // used for the counter (matches "advanced by a per-item counter").
            if (kind >= 0 && kind < sim::kPowerupKinds &&
                !q.powerup_anim[kind].steps.empty()) {
                float sx = tile_screen_x(x) + sim::kTileW / 2.0f;
                float sy = tile_screen_y(y) + sim::kTileH - 1.0f;
                draw_anim(q.powerup_anim[kind], static_cast<std::size_t>(s.tick), sx, sy);
                continue;
            }
            // Fallback: the static POW*.PCX tile-fill (top-left anchor) when the
            // animated sequence is unavailable.
            const Sprite& p = assets_->powerup(kind);
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
        int bo = b.owner < kLocalPlayers ? b.owner : 0;
        // Bomb sprite selection (mirrors the original's per-state pick):
        //   fizzling dud  -> DUDS.ANI "bomb regular green dud"
        //   armed trigger -> TRIGBOMB.ANI "bomb trigger green"
        //   otherwise     -> the normal owner-coloured pulse
        // Each special-case falls back to the pulse if its ANI/sequence is
        // missing so a bomb never blanks out.
        const Anim* ba = &q.bomb[bo];
        if (b.dud_left > 0 && !q.bomb_dud[bo].steps.empty())
            ba = &q.bomb_dud[bo];
        else if (b.trigger && !q.bomb_trigger[bo].steps.empty())
            ba = &q.bomb_trigger[bo];
        draw_anim(*ba, pulse, sx, sy);
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
        // Shadow ellipse at the player's ground anchor. The original blits it
        // at the SAME (x,y) as the player (sub_41F29B: shadow then body at
        // v111+28/+32), its own hotspot doing the centring — no extra offset.
        draw_anim(q.shadow, 0, sx, sy);
        int pv = i < kLocalPlayers ? i : 0;
        const Anim* a = moving_[i] ? &q.walk[pv][dir] : &q.stand[pv][dir];
        std::size_t ph = moving_[i] ? walk_phase_[i] : 0;
        // Boxed-in idle: replace the stand pose with the current "cornerhead"
        // fidget (direction-independent). sample_movement already rolled the
        // variant/duration this tick; the phase just rides the sim tick so the
        // frames advance. Falls back to stand if the CORNER ANI is missing.
        if (!moving_[i] && panic_ticks_[i] > 0 &&
            !q.cornerhead[pv][panic_variant_[i]].steps.empty()) {
            a = &q.cornerhead[pv][panic_variant_[i]];
            ph = static_cast<std::size_t>(s.tick);
        }
        // A recent kick/punch overrides walk/stand with the action pose, played
        // once over its lifetime (falls back to walk/stand if the ANI is
        // missing so nothing ever blanks out).
        if (kick_pose_[i] > 0 && !q.kick[pv][dir].steps.empty()) {
            a = &q.kick[pv][dir];
            ph = static_cast<std::size_t>(kActionPoseTicks - kick_pose_[i]);
        } else if (punch_pose_[i] > 0 && !q.punch[pv][dir].steps.empty()) {
            a = &q.punch[pv][dir];
            ph = static_cast<std::size_t>(kActionPoseTicks - punch_pose_[i]);
        }
        // Carrying a grabbed bomb wins over the idle fidget and the kick/punch
        // poses (a carrying player can't kick/punch): show the "holding a bomb"
        // walk/stand pose (BWALK*.ANI), same phase convention as walk/stand.
        // Falls back to the plain walk/stand already selected if the ANI is
        // missing so nothing ever blanks out.
        if (p.carrying) {
            const Anim* c = moving_[i] ? &q.walkbomb[pv][dir] : &q.standbomb[pv][dir];
            if (!c->steps.empty()) {
                a = c;
                ph = moving_[i] ? walk_phase_[i] : 0;
            }
        }
        // A diseased player's sprite strobes through random colours — the
        // original redraws it with rand()%10 while a disease-age bit is set
        // (sub_41F29B). Tint on alternating sim ticks so it visibly flashes.
        if (p.disease_timer > 0 && (s.tick & 1)) {
            draw_anim(*a, ph, sx, sy, disease_flash_channel(), disease_flash_channel(),
                      disease_flash_channel());
        } else {
            draw_anim(*a, ph, sx, sy);
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
    draw_actors(s);  // conveyor/trampoline floor tiles, under powerups + entities
    draw_powerups(s);
    draw_world(s);
    draw_hud(s);
}

}  // namespace bomber::game
