#include "bomber/game/renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "bomber/game/anim_pace.hpp"
#include "bomber/game/hud_format.hpp"

namespace bomber::game {

// How long a kick/punch pose stays up before returning to walk/stand. The
// KICK/PUNCH sequences are short, so ~8 ticks (~0.4 s) reads cleanly.
constexpr int kActionPoseTicks = 8;

// Trampoline hop height per elapsed frame. CONFIRMED VALUELST id 681 = 35
// ("how many pixels vertically do you move each frame?"), read by sub_41F29B
// state 5 to blit the flying body at y - 35*min(c, len-c). Presentation-only —
// the integer sim omits it; see docs/re/stage-actors.md §4.
constexpr int kHopPixelsPerFrame = 35;

// Total warp duration in sim ticks (9 warp-out + 9 warp-in). Mirrors
// StageActorSystem::kWarpTicks (a private sim header); the "spin" warp animation
// (WALK.ANI, sub_41F29B states 6/7) advances by elapsed = kWarpTicks - warp.
constexpr int kWarpTicks = 18;

// Spread of the idle-fidget duration: the boxed-in "cornerhead" fidget rolls
// `20 + rand()%getvalue(330)` ticks per fidget (sub_41F29B ~23011, guarded so
// the modulus is >= 1). VALUELST id 330 = 13 (CONFIRMED — the file labels it
// "how many cornerhead animations there are"; it is BOTH the fidget-duration
// spread and the number of cornerhead sequences, so it equals
// kCornerheadVariants above, not a coincidence). Presentation-only: the roll
// comes off panic_lcg_, never State::rng, so there is no golden impact.
constexpr int kPanicSpread = 13;  // getvalue(330)

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

void Renderer::reset_match(bool untimed) {
    deaths_.clear();
    last_tick_ = ~0ull;
    hurry_until_ = 0;
    untimed_ = untimed;
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
    // Conveyor / dirarrow / warphole / trampoline floor tiles, drawn UNDER
    // powerups and entities (the lowest floor layer above the field). Anchored
    // bottom-centre like every other tile. The original's actor animator
    // (sub_4056CA) advances each actor's own frame counter (+48) once PER DRAW
    // and blits sub_41DAA7(seq, counter): dirarrows (case 0) and warpholes
    // (case 1) animate at the full per-frame rate, the conveyor (case 2) at
    // counter/3 (a slow belt), and the trampoline (case 3) only while a player
    // is bouncing on it (else frame 0 — the resting mat). Drawing a warphole/
    // arrow at a fixed frame 0 froze its dormant "closed" frame — the bug Ege
    // caught. See docs/re/stage-actors.md §3-5.
    const SequenceSet& q = *seqs_;
    const std::size_t belt_step = static_cast<std::size_t>(s.tick / 3);
    const std::size_t full_step = static_cast<std::size_t>(s.tick);

    // How far into its hop the trampoline at (x,y) is — driven by the hashed
    // Player::bounce of whoever is centred on it, so the mat rests until (and
    // only while) it is actually bounced. The bounce counts down from
    // tuning.trampoline_bounce_frames (VALUELST 680); frame 0 = resting mat. The
    // "extra trampoline" ANI is 12 frames, so draw_anim's `% statecnt` maps the
    // 30-tick bounce onto the 12 art frames.
    const std::int32_t bounce_len = s.tuning.trampoline_bounce_frames;
    auto tramp_frame = [&](int x, int y) -> std::size_t {
        for (const auto& p : s.players) {
            if (!p.present || !p.alive || p.bounce <= 0) continue;
            if (p.tile_x() == x && p.tile_y() == y)
                return static_cast<std::size_t>(bounce_len - p.bounce);
        }
        return 0;  // no one bouncing here: the resting frame
    };

    for (int y = 0; y < sim::kGridHeight; ++y) {
        for (int x = 0; x < sim::kGridWidth; ++x) {
            sim::ActorType at = s.actor_type[y][x];
            if (at == sim::ActorType::None) continue;
            float sx = tile_screen_x(x) + sim::kTileW / 2.0f;
            float sy = tile_screen_y(y) + sim::kTileH - 1.0f;
            const int g = s.actor_dir[y][x] & 3;
            switch (at) {
                case sim::ActorType::Conveyor:
                    if (!q.conveyor[g].steps.empty()) draw_anim(q.conveyor[g], belt_step, sx, sy);
                    break;
                case sim::ActorType::DirArrow:
                    if (!q.dirarrow[g].steps.empty()) draw_anim(q.dirarrow[g], full_step, sx, sy);
                    break;
                case sim::ActorType::Warphole:
                    if (!q.warphole.steps.empty()) draw_anim(q.warphole, full_step, sx, sy);
                    break;
                case sim::ActorType::Trampoline:
                    if (!q.trampoline.steps.empty())
                        draw_anim(q.trampoline, tramp_frame(x, y), sx, sy);
                    break;
                default: break;
            }
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
        // Trampoline flight lift. CONFIRMED arc from sub_41F29B state 5 (raw
        // disasm 0x4204b3..0x420517): the body is blitted at y - v80 where
        //   v80 = getvalue(681) * (c < len/2 ? c : len - c)
        // i.e. a linear tent peaking at the apex (c == len/2). c = elapsed frames
        // (the original's +80 up-counter); our Player::bounce is the equivalent
        // down-counter, already decremented for this tick, so c = len - bounce.
        // getvalue(681) = 35 px/frame (VALUELST id 681, presentation-only — the
        // integer sim omits it). len = getvalue(680) = 30, peak = 35*15 = 525 px:
        // the player rockets high off the top of the field and lands on the random
        // apex tile, exactly the game's "fly". (The shadow is suppressed entirely
        // during the flight — see below.)
        float lift = 0.0f;
        if (p.bounce > 0) {
            const int len = s.tuning.trampoline_bounce_frames;
            const int c = len - p.bounce;  // elapsed frames: 0 at launch .. len-1
            const int tent = c < len - c ? c : len - c;  // min(c, len-c)
            lift = static_cast<float>(kHopPixelsPerFrame * tent);
        }
        // Shadow ellipse at the player's ground anchor. The original blits it
        // at the SAME (x,y) as the player (sub_41F29B: shadow then body at
        // v111+28/+32), its own hotspot doing the centring — no extra offset.
        // EXCEPTION: the trampoline flight (state 5) skips the shadow entirely
        // (the state-5 block jmps to LABEL_246 at 0x420870, past the LABEL_239
        // shadow blit) — the player is high in the air with no ground contact.
        // The warp (states 6/7) DOES draw the shadow (it lands at 0x42071e, which
        // blits "shadow" @0x45a242), so only a bounce suppresses it.
        if (p.bounce <= 0) draw_anim(q.shadow, 0, sx, sy);
        sy -= lift;  // raise the body (and anything anchored to it) by the hop arc
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
        // Warp/teleport pose wins over everything: while warping the player is
        // fully state-gated (states 6/7, no walk/kick/carry), and the original
        // draws the "spin" sequence (sub_41F29B strcpy'd @0x45a213) both phases,
        // advancing its frame by the per-phase counter (+80). Player::warp counts
        // 18→0, so elapsed = kWarpTicks - warp drives the frame; draw_anim's
        // `% statecnt` cycles the spin art across the 9-out + 9-in ticks. Falls
        // back to the pose already selected if WALK.ANI has no "spin".
        if (p.warp > 0 && !q.spin[pv].steps.empty()) {
            a = &q.spin[pv];
            ph = static_cast<std::size_t>(kWarpTicks - p.warp);
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

    // Campaign rover/ghost hazards (docs/re/campaign.md "Per-tick mover").
    // No known install ships GHOST.ANI/ROVER.ANI (confirmed cut content —
    // see sequences.hpp's rover/ghost comment), so there is no Anim to draw;
    // a small filled marker keeps the actor visible instead of invisible,
    // distinct per kind (rover = brown/orange, ghost = pale blue-white) and
    // per-tile bottom-anchored like every other world entity here.
    for (const auto& r : s.rovers) {
        if (!r.alive) continue;
        float sx = kFieldOriginX + r.x / static_cast<float>(sim::kScale);
        float sy = kFieldOriginY + r.y / static_cast<float>(sim::kScale) +
                  sim::kTileH / 2.0f - 1.0f;
        constexpr float kMarkerW = 24.0f, kMarkerH = 24.0f;
        SDL_FRect dst{sx - kMarkerW / 2.0f, sy - kMarkerH - 4.0f, kMarkerW, kMarkerH};
        if (r.kind == sim::RoverKind::Rover)
            SDL_SetRenderDrawColor(ren_, 170, 90, 30, 255);   // rover: brown/orange
        else
            SDL_SetRenderDrawColor(ren_, 210, 225, 255, 220);  // ghost: pale blue-white
        SDL_RenderFillRect(ren_, &dst);
        SDL_SetRenderDrawColor(ren_, 20, 20, 20, 255);
        SDL_RenderRect(ren_, &dst);  // outline so it reads against similar floor colours
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
    // HURRY! banner flashes center-screen (every-4-ticks blink). The sim's
    // enclosure system already reconciles the "60s-remaining, fires once"
    // predicate (docs/re/in-match-shell.md §3's getvalue(101) window) into a
    // single Hurry event on the edge (enclosure.cpp); on_events just latches
    // it here for the flash duration, and SoundDirector fires SFX 2700..2799
    // off the same event (sound_director.cpp) — one source of truth, no
    // duplicated threshold math on the presentation side.
    if (s.tick < hurry_until_ && ((s.tick >> 2) & 1) == 0)
        draw_anim(seqs_->hurry, 0, kScreenW / 2.0f, kScreenH / 2.0f);

    // Match clock at the original HUD position — CONFIRMED VALUELST ids
    // 110/111/112 (docs/re/in-match-shell.md §3, docs/valuelst-map.md):
    // x/y of the first digit and the extra per-digit spacing. Read live so a
    // modified VALUELST still repositions the HUD; fallbacks are the
    // confirmed 525/36/4.
    const float x0 = static_cast<float>(values_ ? values_->column_or(110, 0, 525) : 525);
    const float y = static_cast<float>(values_ ? values_->column_or(111, 0, 36) : 36);
    const float spacing = static_cast<float>(values_ ? values_->column_or(112, 0, 4) : 4);

    if (untimed_) {
        // Untimed round (options_.playtime_seconds == 1001, the port's
        // presentation-side stand-in for dword_4601A8 == 1001 — see
        // Renderer::reset_match's doc comment): draw the KFONT "infinity"
        // glyph in place of the digit string, same anchor as the clock.
        const Anim& inf = seqs_->infinity;
        if (!inf.steps.empty()) draw_sprite(inf.steps[0], x0 + inf.steps[0].hx, y);
        return;
    }

    const Anim& d = seqs_->digits;
    if (d.steps.size() < 11) return;
    int seconds_left = (s.ticks_left + sim::kTicksPerSecond - 1) / sim::kTicksPerSecond;
    // MESSAGES.TXT id 281 = "%u:%02u" (sub_4105D2's v13/60, v13%60 split);
    // getstring falls back to the literal format when the install's own
    // MESSAGES.TXT lacks the id (asset_store.hpp's getstring convention).
    std::string text = format_clock(assets_->getstring(281, "%u:%02u"), seconds_left);

    // <=30s remaining: swap the digit ink from the normal colour to a
    // warning one (byte_49D38F -> byte_49A390 in the original, docs/re/
    // in-match-shell.md §3 point 4). The exact palette index isn't resolvable
    // without the original's LUT (out of scope, matching frontend-flow.md's
    // byte_49D38F precedent) — a clear red stand-in reads as "warning" the
    // same way; layout/timing are the faithful part.
    bool warn = clock_warning(seconds_left);
    Uint8 r = 255, g = warn ? 40 : 255, b = warn ? 40 : 255;

    float x = x0;
    for (char ch : text) {
        std::size_t idx = ch == ':' ? 10 : static_cast<std::size_t>(ch - '0');
        if (idx > 10) continue;  // ignore anything format_clock couldn't map to a glyph
        const Sprite& sp = d.steps[idx];
        draw_sprite(sp, x + sp.hx, y, r, g, b);
        x += sp.w + spacing;  // VALUELST 112: extra spacing between digits
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
