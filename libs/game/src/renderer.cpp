#include "bomber/game/renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "bomber/game/anim_pace.hpp"
#include "bomber/game/hud_format.hpp"
#include "bomber/match/team_colour.hpp"

namespace bomber::game {

// Trampoline hop height per elapsed frame. CONFIRMED VALUELST id 681 = 35
// ("how many pixels vertically do you move each frame?"), read by sub_41F29B
// state 5 to blit the flying body at y - 35*min(c, len-c). Presentation-only —
// the integer sim omits it; see docs/re/stage-actors.md §4.
constexpr int kHopPixelsPerFrame = 35;

// Total warp duration in sim ticks (9 warp-out + 9 warp-in). Mirrors
// StageActorSystem::kWarpTicks (a private sim header); the "spin" warp animation
// (WALK.ANI, sub_41F29B states 6/7) advances by elapsed = kWarpTicks - warp.
constexpr int kWarpTicks = 18;

// Inter-tick interpolation snap threshold (see draw_frame's doc comment in
// renderer.hpp). Anything a moving entity legitimately covers in ONE 20 Hz
// tick stays well under this: a max-skate hyper walker ~30 px, a kicked slide
// getvalue(300)/100 = 10 px, a punched/thrown bomb's flight leg under a tile.
// Warps, trampoline landings, and the flying-bomb field wrap all move a full
// tile (36/40 px) or more in one tick — those must SNAP, not smear across
// the screen.
constexpr sim::Fixed kInterpSnapDelta = 32 * sim::kScale;

void Renderer::capture_interp(const sim::State& s) {
    if (s.tick == interp_tick_) return;
    // A tick we've never seen (fresh match without reset_match, or a resumed
    // demo) makes the previous snapshot meaningless only when there IS none;
    // stale cross-round data is caught by reset_match and, failing that, by
    // the snap threshold.
    interp_valid_ = interp_tick_ != ~0ull;
    prev_px_ = seen_px_;
    prev_py_ = seen_py_;
    prev_bombs_.swap(seen_bombs_);
    prev_rovers_.swap(seen_rovers_);
    for (int i = 0; i < sim::kMaxPlayers; ++i) {
        seen_px_[i] = s.players[i].x;
        seen_py_[i] = s.players[i].y;
    }
    // This tick's per-sub-frame player motion (prev endpoints -> the seen
    // endpoints captured above) for player_interp's playback.
    trace_ = s.sub_trace;
    seen_bombs_.assign(s.bombs.size(), EntSnap{});
    for (std::size_t i = 0; i < s.bombs.size(); ++i)
        seen_bombs_[i] = {s.bombs[i].active, s.bombs[i].x, s.bombs[i].y};
    seen_rovers_.assign(s.rovers.size(), EntSnap{});
    for (std::size_t i = 0; i < s.rovers.size(); ++i)
        seen_rovers_[i] = {s.rovers[i].alive, s.rovers[i].x, s.rovers[i].y};
    interp_tick_ = s.tick;
}

Renderer::Posf Renderer::player_interp(const sim::State& s, int i, int& out_dir) const {
    const sim::Player& p = s.players[i];
    out_dir = static_cast<int>(p.facing);
    const float fx = static_cast<float>(p.x) / static_cast<float>(sim::kScale);
    const float fy = static_cast<float>(p.y) / static_cast<float>(sim::kScale);
    if (!interp_valid_ || interp_alpha_ >= 1.0f) return {fx, fy};
    // Map alpha onto the tick's kSubFrames playback segments: segment f runs
    // from sample f-1 (or the previous tick's endpoint for f == 0) to sample
    // f. Sample kSubFrames-1 is pinned to the tick's true endpoint by the
    // sim (run_tick step 12), so alpha -> 1 converges on exactly the
    // position the old endpoint lerp used.
    const float t = interp_alpha_ * static_cast<float>(sim::kSubFrames);
    int f = static_cast<int>(t);
    if (f >= sim::kSubFrames) f = sim::kSubFrames - 1;
    const float local = t - static_cast<float>(f);
    const auto& seg_end = trace_[i][f];
    const sim::Fixed x0 = f == 0 ? prev_px_[i] : trace_[i][f - 1].x;
    const sim::Fixed y0 = f == 0 ? prev_py_[i] : trace_[i][f - 1].y;
    out_dir = static_cast<int>(seg_end.facing);
    const float ex = static_cast<float>(seg_end.x) / static_cast<float>(sim::kScale);
    const float ey = static_cast<float>(seg_end.y) / static_cast<float>(sim::kScale);
    // Same both-axes snap rule as interp_pos, applied per SEGMENT: a warp/
    // trampoline/scatter relocation lands entirely in one segment and snaps
    // there instead of smearing across the field.
    if (std::abs(seg_end.x - x0) > kInterpSnapDelta || std::abs(seg_end.y - y0) > kInterpSnapDelta)
        return {ex, ey};
    const float sx = static_cast<float>(x0) / static_cast<float>(sim::kScale);
    const float sy = static_cast<float>(y0) / static_cast<float>(sim::kScale);
    return {sx + (ex - sx) * local, sy + (ey - sy) * local};
}

Renderer::Posf Renderer::interp_pos(sim::Fixed prev_x, sim::Fixed prev_y, sim::Fixed x,
                                    sim::Fixed y, bool prev_ok) const {
    const float fx = static_cast<float>(x) / static_cast<float>(sim::kScale);
    const float fy = static_cast<float>(y) / static_cast<float>(sim::kScale);
    if (!interp_valid_ || !prev_ok || interp_alpha_ >= 1.0f) return {fx, fy};
    // Snap both axes together: lerping the small axis of a mostly-teleport
    // move would draw one frame at a position the entity never occupied.
    if (std::abs(x - prev_x) > kInterpSnapDelta || std::abs(y - prev_y) > kInterpSnapDelta)
        return {fx, fy};
    const float pfx = static_cast<float>(prev_x) / static_cast<float>(sim::kScale);
    const float pfy = static_cast<float>(prev_y) / static_cast<float>(sim::kScale);
    return {pfx + (fx - pfx) * interp_alpha_, pfy + (fy - pfy) * interp_alpha_};
}

void Renderer::draw_sprite(const Sprite& sp, float x, float y, Uint8 r, Uint8 g, Uint8 b) {
    // HD override when enabled and authored for this frame; the classic w/h/hx/hy
    // are kept, so the higher-res texture is just sampled into the same logical
    // dst rect (the front-end HD PCX trick, asset_store.cpp frontend_pcx).
    SDL_Texture* tex = (assets_->hd_enabled() && sp.tex_hd) ? sp.tex_hd : sp.tex;
    if (!tex) return;
    SDL_FRect dst{x - sp.hx, y - sp.hy, static_cast<float>(sp.w), static_cast<float>(sp.h)};
    bool tinted = r != 255 || g != 255 || b != 255;
    if (tinted) SDL_SetTextureColorMod(tex, r, g, b);
    SDL_RenderTexture(ren_, tex, nullptr, &dst);
    if (tinted) SDL_SetTextureColorMod(tex, 255, 255, 255);
}

void Renderer::draw_anim(const Anim& a, std::size_t step, float x, float y, Uint8 r, Uint8 g,
                         Uint8 b) {
    if (a.steps.empty()) return;
    // frame = counter % statecnt — the original ANI player (sub_41DAA7). The
    // STAT HEAD timing field is inert; see anim_pace.hpp / docs/re/facts.md.
    draw_sprite(a.steps[anim_step_index(step, a.steps.size())], x, y, r, g, b);
}

int Renderer::disease_flash_colour() {
    flash_lcg_ = flash_lcg_ * 1664525u + 1013904223u;
    // rand() % 10 in the original (sub_41F29B ~23252) — one of the ten real
    // player-colour sprite sets, not an arbitrary tint. See draw_world's
    // disease_flash comment for the full citation.
    return static_cast<int>((flash_lcg_ >> 16) % kLocalPlayers);
}

std::uint32_t Renderer::panic_roll() {
    panic_lcg_ = panic_lcg_ * 1664525u + 1013904223u;
    return panic_lcg_ >> 16;
}

std::uint32_t Renderer::gold_roll() {
    gold_lcg_ = gold_lcg_ * 1664525u + 1013904223u;
    return gold_lcg_ >> 16;
}

// Gold Bomberman "twinkle" (docs/re/goldman-roulette.md §6, VALUELST id 1010,
// SOUNDLST-adjacent id collision noted in docs/re/id-audit.md — this is the
// VALUELST sense, "how many seconds the twinkling of goldman lasts"). Ported
// from `sub_420D4E` (spawn, pseudo.c 23549-23583) and `sub_420E39` (age/draw,
// pseudo.c 23591-23625), called once per player per tick by `sub_420F07`'s
// main per-tick loop while `!dword_464938 && dword_4648BC` (goldman option on,
// not the attract/no-match state) and a gold player is pending.
void Renderer::update_gold_sparkles(const sim::State& s) {
    // SPAWN only (sub_420D4E) — runs once per new sim tick via sample_movement.
    // Aging/retirement + frame advance is NOT here: the original's sub_420E39
    // ages each particle once per ENGINE FRAME (batch_0x420D4E.cpp:136), not
    // per sim tick, so it lives in draw_world's per-frame sparkle pass instead
    // (aging here at 20 Hz made the sparkles ~9x too slow and lingering).
    if (gold_player_ < 0) return;
    // getvalue(1010): twinkle duration in seconds; the file's own legend says
    // 0 = indefinitely. A fresh bomber::sim::Simulation is built per round
    // (GameApp::start_match), so s.tick already IS the round-elapsed clock —
    // no separate "round start" bookkeeping needed.
    const std::int64_t duration = values_ ? values_->at_or(1010, 5) : 5;
    if (duration != 0 && static_cast<std::int64_t>(s.tick) / sim::kTicksPerSecond >= duration)
        return;
    for (int i = 0; i < sim::kMaxPlayers; ++i) {
        const sim::Player& p = s.players[i];
        if (!p.present || !p.alive) continue;
        // Team play: dword_46492C holds the clinching player's TEAM id (see
        // bomber::game::assign_gold_player's doc comment), and the original
        // sparkles every teammate (`byte_461C18[152*i] ? 2 : 0 == dword_46492C`).
        // Solo play: dword_46492C is the player slot index directly.
        const bool is_gold = gold_team_mode_ ? (p.team == gold_player_) : (i == gold_player_);
        if (!is_gold) continue;
        // sub_420D4E: scan for the first empty slot; exactly one attempt (spawn
        // or not) per matching player per tick, never a fresh scan per particle.
        for (auto& sp : gold_sparkles_) {
            if (sp.active) continue;
            if (gold_roll() % 6 != 0) {  // 5-in-6 chance to actually place it
                const float px = kFieldOriginX + p.x / static_cast<float>(sim::kScale);
                const float py = kFieldOriginY + p.y / static_cast<float>(sim::kScale) +
                                 sim::kTileH / 2.0f - 1.0f;
                sp.active = true;
                sp.age = 0;
                // rand()%40 + player_x - 20, rand()%50 + player_y - 48 — fixed
                // at spawn, the particle does not track the player afterward.
                sp.x = px + static_cast<float>(gold_roll() % 40) - 20.0f;
                sp.y = py + static_cast<float>(gold_roll() % 50) - 48.0f;
            }
            break;
        }
    }
}

int Renderer::render_colour(const sim::State& s, int slot) {
    if (slot < 0 || slot >= sim::kMaxPlayers) return 0;
    return match::team_render_colour(s.players[slot].team, slot);
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
        bool blocked = s.cells[ny][nx] == sim::Cell::Solid || s.cells[ny][nx] == sim::Cell::Brick ||
                       s.burning[ny][nx] > 0;
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

const Anim& Renderer::flame_piece(const FlameSet& fset, sim::FlameKind kind) {
    // 1:1 with FlameKind's own off_45BEA0-mirroring order (types.hpp) — the
    // sim decides the piece once at ignition (FlameSystem::spread_to), so
    // this is a plain lookup, not a live neighbour scan.
    switch (kind) {
        case sim::FlameKind::TipNorth: return fset.tip_n;
        case sim::FlameKind::TipEast: return fset.tip_e;
        case sim::FlameKind::TipSouth: return fset.tip_s;
        case sim::FlameKind::TipWest: return fset.tip_w;
        case sim::FlameKind::MidNorth: return fset.mid_v[0];  // "flame midnorth green"
        case sim::FlameKind::MidEast: return fset.mid_h[1];   // "flame mideast green"
        case sim::FlameKind::MidSouth: return fset.mid_v[1];  // "flame midsouth green"
        case sim::FlameKind::MidWest: return fset.mid_h[0];   // "flame midwest green"
        case sim::FlameKind::Center: return fset.center;
    }
    return fset.center;
}

void Renderer::reset_match(bool untimed) {
    deaths_.clear();
    last_tick_ = ~0ull;
    hurry_until_ = 0;
    untimed_ = untimed;
    kick_pose_.fill(0);
    punch_pose_.fill(0);
    pickup_pose_.fill(0);
    panic_active_.fill(false);
    panic_elapsed_.fill(0);
    // Drop the inter-tick snapshots: a new round restarts s.tick and reuses
    // slots, so lerping from the previous match's positions would be garbage.
    interp_tick_ = ~0ull;
    interp_valid_ = false;
    prev_bombs_.clear();
    seen_bombs_.clear();
    prev_rovers_.clear();
    seen_rovers_.clear();
}

void Renderer::on_events(const sim::State& s) {
    // Age the action poses once per tick (on_events is called exactly once per
    // sim tick, before the frame is drawn).
    for (int i = 0; i < sim::kMaxPlayers; ++i) {
        if (kick_pose_[i] > 0) --kick_pose_[i];
        if (punch_pose_[i] > 0) --punch_pose_[i];
        if (pickup_pose_[i] > 0) --pickup_pose_[i];
    }
    for (const auto& ev : s.events) {
        switch (ev.type) {
            case sim::Event::Type::Hurry:
                hurry_until_ = s.tick + 60;  // ~3 s of flashing banner
                break;
            case sim::Event::Type::BombKicked:
                // The kick pose plays for exactly the KICK.ANI sequence's own
                // frame count, like the pickup pose below — sub_41F29B state 1
                // exits when its elapsed frame passes the sequence's statecnt
                // (sub_41DA5C, pseudo.c ~23119-23129/native batch_0x41F29B), NOT
                // a fixed tick budget. dir = the player's facing at the event.
                if (ev.player >= 0 && ev.player < sim::kMaxPlayers) {
                    const auto& seq =
                        seqs_->kick[render_colour(s, ev.player)]
                                   [static_cast<int>(s.players[ev.player].facing)];
                    kick_pose_[ev.player] = static_cast<int>(seq.steps.size());
                }
                break;
            case sim::Event::Type::BombPunched:
                // Same as BombKicked: the punch pose runs for PUNCH.ANI's own
                // length (sub_41F29B state 2, pseudo.c ~23135-23145), not a
                // guessed constant.
                if (ev.player >= 0 && ev.player < sim::kMaxPlayers) {
                    const auto& seq =
                        seqs_->punch[render_colour(s, ev.player)]
                                    [static_cast<int>(s.players[ev.player].facing)];
                    punch_pose_[ev.player] = static_cast<int>(seq.steps.size());
                }
                break;
            case sim::Event::Type::BombGrabbed:
                // "Picking up a bomb" transitional pose (PUP*.ANI "pickup
                // <dir>", sub_41F29B action-state 4 — the state exits when
                // the anim counter passes the sequence's own statecnt,
                // pseudo.c ~23396-23407, so the countdown is the sequence
                // length; the shipped PUP sequences are 10 steps).
                if (ev.player >= 0 && ev.player < sim::kMaxPlayers) {
                    const auto& seq = seqs_->pickup[render_colour(s, ev.player)]
                                                   [static_cast<int>(s.players[ev.player].facing)];
                    pickup_pose_[ev.player] = static_cast<int>(seq.steps.size());
                }
                break;
            case sim::Event::Type::PlayerDied: {
                // Every real emitter (simulation.cpp/enclosure.cpp/rovers.cpp)
                // sets `player` from a valid loop index, but guard it the same
                // way BombKicked/BombPunched do above rather than trust that.
                if (ev.player < 0 || ev.player >= sim::kMaxPlayers) break;
                // render_colour resolves Team Play's white/red override (see its
                // doc comment) so a diseased-strobe-free death still shows the
                // player's on-screen team colour, not their raw slot colour.
                const int colour = render_colour(s, ev.player);
                const auto& pool = assets_->deaths_for(colour);
                if (pool.empty()) break;
                DeathFx fx;
                fx.player = colour;  // NOLINT(bugprone-signed-char-misuse) — range-checked above
                fx.anim =
                    static_cast<std::size_t>(s.tick + static_cast<std::uint64_t>(ev.player) * 7u) %
                    pool.size();
                fx.x = kFieldOriginX + s.players[ev.player].x / static_cast<float>(sim::kScale);
                fx.y = kFieldOriginY + s.players[ev.player].y / static_cast<float>(sim::kScale) +
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
    // Walk state comes from the sim's PlayerWalking events, NOT from the
    // position delta: the original picks walk-vs-stand off the dispatched
    // godir (+46) and advances the 16.16 leg phase by the tick's speed budget
    // (sub_41F29B), which is burned even when a wall blocks every pixel step
    // (sub_41EC84's loop spends 100 per iteration regardless) — a blocked
    // walker pedals in place. Conversely a conveyor sliding an IDLE player is
    // the +46 == -1 branch: no event, stand pose gliding along. The previous
    // position-delta approximation got both wrong (froze the first, pedalled
    // the second). The event's data is the disease-scaled budget in px, so
    // one walk frame ~ one pixel of ATTEMPTED travel, molasses/hyper included.
    std::array<std::int8_t, sim::kMaxPlayers> walk_px{};
    for (const auto& ev : s.events) {
        if (ev.type == sim::Event::Type::PlayerWalking && ev.player >= 0 &&
            ev.player < sim::kMaxPlayers)
            walk_px[ev.player] = ev.data;  // clamped 1..127 by the sim, so >0 == walking
    }
    for (int i = 0; i < sim::kMaxPlayers; ++i) {
        const sim::Player& p = s.players[i];
        moving_[i] = p.present && p.alive && walk_px[i] > 0;
        if (moving_[i]) walk_phase_[i] += static_cast<std::uint32_t>(walk_px[i]);
        // Displacement is still tracked separately: the cornerhead fidget
        // below cares about standing STILL while fully enclosed (its entry,
        // sub_41F29B 23006-23013, checks only enclosure + state 0 — held keys
        // don't cancel it, and boxed in they can't displace you anyway).
        const bool displaced =
            last_tick_ != ~0ull && p.present && p.alive && (p.x != last_x_[i] || p.y != last_y_[i]);
        last_x_[i] = p.x;
        last_y_[i] = p.y;

        // Idle-fidget bookkeeping (once per sim tick). While a live player is
        // standing still and fully boxed in, cycle "cornerhead" fidgets; the
        // instant it moves (or the box opens), cancel. The original
        // (sub_41F29B, pseudo.c ~23011 entry / ~23236-23246 exit; native
        // batch_0x41F29B) rolls a VARIANT once — `rand() % getvalue(330) + 20`,
        // where id 330 is the variant COUNT (=13=kCornerheadVariants), never a
        // duration — and holds it until that variant's own ANI plays through
        // one full cycle (elapsed frame >= its sub_41DA5C statecnt), at which
        // point, still boxed in, it re-rolls a fresh variant on the next tick.
        // So the re-roll cadence is the chosen art's own length, not a fixed
        // `20 + rand()%13` spread; and the displayed frame is an elapsed-since-
        // entry counter (panic_elapsed_), not a raw global-tick phase. Cosmetic
        // — rolls come off panic_lcg_, never State::rng (determinism intact).
        bool panic = p.present && p.alive && !displaced && boxed_in(s, p.tile_x(), p.tile_y());
        if (panic) {
            if (!panic_active_[i]) {
                panic_variant_[i] = static_cast<int>(panic_roll() % kCornerheadVariants);
                panic_elapsed_[i] = 0;
                panic_active_[i] = true;
            } else {
                ++panic_elapsed_[i];
                const int len = static_cast<int>(
                    seqs_->cornerhead[render_colour(s, i)][panic_variant_[i]].steps.size());
                if (len <= 0 || panic_elapsed_[i] >= len) {
                    panic_variant_[i] = static_cast<int>(panic_roll() % kCornerheadVariants);
                    panic_elapsed_[i] = 0;
                }
            }
        } else {
            panic_active_[i] = false;
            panic_elapsed_[i] = 0;
        }

        // Bomb-pickup carry arc bookkeeping (docs/re/id-audit.md item 4):
        // ticks elapsed since carrying started, clamped 0..3, mirroring the
        // original's player+80 "elapsed since state entry" counter (see the
        // carried-bomb draw in draw_world for the full citation).
        const bool carrying_now = p.present && p.alive && p.carrying;
        if (carrying_now)
            carry_ticks_[i] = carrying_prev_[i] ? std::min(carry_ticks_[i] + 1, 3) : 0;
        carrying_prev_[i] = carrying_now;
    }
    update_gold_sparkles(s);
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
            // A brick's hidden token flips to "visible" in sim state the
            // instant the brick ignites (docs/re/facts.md "Brick crumble
            // timing" — sub_425107's unconditional reveal), but the ORIGINAL's
            // floor-powerup drawer (sub_424F89, pseudo.c ~26247-26261) has its
            // own separate gate, `*(_DWORD*)v9==2 && !sub_425FB9(j,i)`: it only
            // actually blits the token sprite once the CELL reads blank
            // (sub_425FB9==0). While the brick is still crumbling (cells[y][x]
            // still Brick — it only flips at burning==0) the token is eligible
            // but not drawn, so it stays hidden under the crumble animation
            // and only pops into view once the tile actually opens. Without
            // this gate the token appeared immediately at ignition, visible
            // through/under the still-standing brick's crumble frames.
            if (s.cells[y][x] != sim::Cell::Blank) continue;
            int kind = static_cast<int>(s.floor[y][x]);
            // The original draws floor powerups with the ANIMATED "power <name>"
            // sequence (POWERS.ANI) at (tile-centre-x, tile-bottom-y) minus the
            // sprite hotspot — the same bottom-centre anchor as tiles/bricks —
            // advancing frame = counter % statecnt (sub_4250DE / sub_41DAA7).
            // See docs/re/facts.md "Screen geometry". A shared per-tick pulse is
            // used for the counter (matches "advanced by a per-item counter").
            if (kind >= 0 && kind < sim::kPowerupKinds && !q.powerup_anim[kind].steps.empty()) {
                float sx = tile_screen_x(x) + sim::kTileW / 2.0f;
                float sy = tile_screen_y(y) + sim::kTileH - 1.0f;
                draw_anim(q.powerup_anim[kind], static_cast<std::size_t>(s.tick), sx, sy);
                continue;
            }
            // Fallback: the static POW*.PCX tile-fill (top-left anchor) when the
            // animated sequence is unavailable.
            const Sprite& p = assets_->powerup(kind);
            SDL_Texture* ptex = (assets_->hd_enabled() && p.tex_hd) ? p.tex_hd : p.tex;
            if (!ptex) continue;
            SDL_FRect dst{tile_screen_x(x), tile_screen_y(y), static_cast<float>(p.w),
                          static_cast<float>(p.h)};
            SDL_RenderTexture(ren_, ptex, nullptr, &dst);
        }
    }
}

void Renderer::draw_bombs(const sim::State& s) {
    const SequenceSet& q = *seqs_;
    std::size_t pulse = static_cast<std::size_t>(s.tick);
    // Bombs (pulse at tick rate, owner-colored; airborne ones arc and wrap).
    // Drawn as their OWN pass, before powerups/flame/burn/players — matching
    // the original's per-frame order (`sub_42A191`, pseudo.c ~29488-29556):
    // the bomb updater `sub_4245B9`->`sub_42331C` (which draws inline as it
    // ticks) runs before the powerup drawer `sub_424F89` and the flame/burn
    // animator `sub_426D06`, both of which in turn run before the player
    // drawer `sub_420F07`. A bomb sitting on a powerup tile or in a
    // just-ignited flame tile (the one-tick window before a chain-reaction
    // detonates it, docs/re/facts.md "Chain-reaction timing") must be drawn
    // UNDER those, not over them; our previous single `draw_world` pass drew
    // bombs after cells/flames (and after `draw_powerups`), compositing the
    // opposite way. See docs/re/facts.md "Draw order".
    for (std::size_t bi = 0; bi < s.bombs.size(); ++bi) {
        const auto& b = s.bombs[bi];
        if (!b.active) continue;
        // Inter-tick smoothing for kicked slides and flight legs; a slot that
        // wasn't an active bomb last tick draws unsmoothed (prev_ok false).
        const bool prev_ok = bi < prev_bombs_.size() && prev_bombs_[bi].active;
        const Posf ip = interp_pos(prev_ok ? prev_bombs_[bi].x : 0, prev_ok ? prev_bombs_[bi].y : 0,
                                   b.x, b.y, prev_ok);
        float bx = ip.x;
        float by = ip.y;
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
        // Colour byte, not the (chain-transferable) owner word — see
        // Bomb::colour / facts.md "Bomb/flame colour is not the owner".
        int bo = render_colour(s, b.colour);
        // Bomb sprite selection (mirrors the original's "bomb %s green" pick
        // from the EXCLUSIVE kind set at creation — trigger overrides jelly,
        // sub_41EB13 — plus the dud state suffix, sub_42331C):
        //   fizzling dud  -> DUDS.ANI "bomb regular green dud"
        //   armed trigger -> TRIGANIM.ANI "bomb trigger green"
        //   jelly         -> BOMBS.ANI "bomb jelly green"
        //   otherwise     -> the normal owner-coloured pulse
        // Each special-case falls back to the pulse if its ANI/sequence is
        // missing so a bomb never blanks out.
        const Anim* ba = &q.bomb[bo];
        if (b.dud_left > 0 && !q.bomb_dud[bo].steps.empty())
            ba = &q.bomb_dud[bo];
        else if (b.trigger && !q.bomb_trigger[bo].steps.empty())
            ba = &q.bomb_trigger[bo];
        else if (b.jelly && !b.trigger && !q.bomb_jelly[bo].steps.empty())
            ba = &q.bomb_jelly[bo];
        draw_anim(*ba, pulse, sx, sy);
    }
}

void Renderer::draw_cells(const sim::State& s) {
    const SequenceSet& q = *seqs_;
    // Static solid/brick tiles, anchored at bottom-center via their hotspots.
    // These belong to the BACKGROUND layer, drawn before every sprite pass:
    // the original never draws them per frame at all — sub_425D22 STAMPS
    // "tile %u solid"/"tile %u brick" into the background surface whenever a
    // cell changes (sub_425EFC/sub_425E9B), so bombs (including a flying
    // bomb's whole arc), powerups, flames and players all composite OVER
    // them. Our previous single draw_world pass painted these statics after
    // draw_bombs, which buried an airborne bomb behind any brick/solid tile
    // it crossed. docs/re/facts.md "Draw order" (tile layer addendum).
    //
    // A burning brick draws NO static brick here: the ignition stamp
    // (sub_425EFC's blank-then-revert dance) erases the brick from the
    // background — sub_425D22 runs while the cell type is momentarily 0 —
    // leaving bare floor for the crumble frames (drawn later, in
    // draw_world's sub_426D06-equivalent pass) to composite over, even
    // though the CELL stays Brick (blocking) until `burning` expires.
    for (int y = 0; y < sim::kGridHeight; ++y) {
        for (int x = 0; x < sim::kGridWidth; ++x) {
            float sx = tile_screen_x(x) + sim::kTileW / 2.0f;
            float sy = tile_screen_y(y) + sim::kTileH - 1.0f;
            if (s.cells[y][x] == sim::Cell::Solid)
                draw_anim(q.solid, 0, sx, sy);
            else if (s.cells[y][x] == sim::Cell::Brick && s.burning[y][x] == 0)
                draw_anim(q.brick, 0, sx, sy);
        }
    }
}

void Renderer::draw_world(const sim::State& s) {
    const SequenceSet& q = *seqs_;
    std::size_t pulse = static_cast<std::size_t>(s.tick);

    // Brick-crumble frames ("brick %s" burn art, the kind-9 branch of the
    // original's flame/burn animator sub_426D06). Drawn HERE — after bombs
    // and powerups, matching sub_426D06's slot in the per-frame sequence —
    // over the bare floor the ignition stamp left behind (see draw_cells).
    for (int y = 0; y < sim::kGridHeight; ++y) {
        for (int x = 0; x < sim::kGridWidth; ++x) {
            float sx = tile_screen_x(x) + sim::kTileW / 2.0f;
            float sy = tile_screen_y(y) + sim::kTileH - 1.0f;
            if (s.burning[y][x] > 0)
                // Frame pacing: sub_426D06's per-cell counter (+48) is a
                // monotonic tick counter reset to 0 at ignition (sub_426FCC's
                // `*(_WORD*)(v8+48)=0`) and advanced by exactly 1 per tick
                // (dword_464958==dword_46494C at the locked 20 Hz rate, so the
                // +50 pacing accumulator fires every call) — NOT rescaled to
                // fit brick_burn_frames; sub_41DAA7 just wraps it `%
                // statecnt` like every other ANI playback. elapsed =
                // brick_burn_frames - remaining reproduces that counter
                // exactly (both start at 0, +1/tick). See docs/re/facts.md
                // "Flame/burn frame pacing".
                // CLAMP to the last cel, do NOT wrap: sub_426D06's kind-9
                // (brick crumble) draw holds the final disintegration frame
                // once the counter passes statecnt-1, instead of the generic
                // `% statecnt` wrap draw_anim otherwise applies. Most tilesets'
                // XBRICK has FEWER cels than brick_burn_frames (9 vs 10 for
                // FIELD0/1/5/... — Green Acres etc.), so on the LAST burn tick
                // (burning==1, elapsed==brick_burn_frames-1==9) a wrap gives
                // 9 % 9 == 0 = the FULL/fresh brick cel — a one-frame DARK
                // "brick re-forms" flash the user reported on those maps. FIELD10
                // (10-cel XBRICK) never wraps, which is why the visual golden
                // (its demo field) never caught this. Clamp fixes the rest and
                // leaves FIELD10 byte-identical.
                {
                    const std::size_t elapsed =
                        static_cast<std::size_t>(s.tuning.brick_burn_frames - s.burning[y][x]);
                    const std::size_t last =
                        q.burn.steps.empty() ? 0 : q.burn.steps.size() - 1;
                    draw_anim(q.burn, std::min(elapsed, last), sx, sy);
                }
        }
    }

    // Flames. Arm-piece (center/mid/tip) selection reads the sim's
    // `flame_kind` — decided once at ignition from the casting arm's own
    // direction and position-within-reach, exactly mirroring sub_42331C's
    // arm loop (pseudo.c ~25625/25673-25677); see FlameKind's doc comment
    // and docs/re/facts.md "Flame arm-shape selection". A previous live scan
    // of neighbouring flame cells (checkerboarding the mid-piece choice, and
    // mis-tipping arms cut short by an obstacle) has been removed.
    for (int y = 0; y < sim::kGridHeight; ++y) {
        for (int x = 0; x < sim::kGridWidth; ++x) {
            if (s.flame[y][x] == 0) continue;
            // Colour from flame_colour, NOT flame_owner: the owner word is
            // kill credit and moves to the chainer on a chain hit, while the
            // drawn colour (the flame record's +60 byte, from the igniting
            // bomb's creation-time colour) never transfers — overlapping
            // explosions from different players keep their own colours.
            // docs/re/facts.md "Bomb/flame colour is not the owner".
            const FlameSet& fset = q.flames[render_colour(s, s.flame_colour[y][x])];
            const Anim* a = &flame_piece(fset, s.flame_kind[y][x]);
            if (a->steps.empty()) continue;
            // Frame pacing: same free-running, ignition-zeroed per-cell
            // counter as the brick-burn draw above (sub_426D06 drives both
            // kinds off one +48 field) — see that draw's comment and
            // docs/re/facts.md "Flame/burn frame pacing".
            std::size_t idx = anim_step_index(
                static_cast<std::size_t>(s.tuning.flame_frames - s.flame[y][x]), a->steps.size());
            const Sprite& sp = a->steps[idx];
            float sx = tile_screen_x(x) + sim::kTileW / 2.0f;
            float sy = tile_screen_y(y) + sim::kTileH - 1.0f;
            // sub_426D06's real-flame branch (kind != 9, off_45BEA0 index
            // 0-8) is the ONE draw site that calls the offset getter
            // sub_41DB41 and folds the per-STAT dx/dy into the blit position —
            // every other sequence in the game, INCLUDING this same function's
            // brick-burn kind-9 branch, ignores dx/dy per the general rule
            // (docs/formats/ani.md "Rendering a step"). Apply it here, and only
            // here. See docs/re/facts.md "Flame draw offset".
            //
            // Coordinate math resolved by disassembly (BM95.EXE 0x426ee7-
            // 0x426f46; the whole Y block was lost as Hex-Rays' "v6 possibly
            // undefined"). Before the blit's own hotspot subtraction the branch
            // computes:
            //   X = sub_426524(j) + dx            (= X_base + dx)
            //   Y = sub_42655F(i) - tileH/2 + dy  (= Y_base - tileH/2 + dy)
            // i.e. dx/dy are ADDED (the Y sign was never negative — the earlier
            // "symmetry inference" caveat had the sign right but MISSED this
            // `- tileH/2` anchor shift). `sub_42655F` returns tile-BOTTOM
            // (Y_base == our sy); `- tileH/2` (dword_4648A0/2, the same Y stride
            // our kTileH is) re-anchors flames to tile CENTRE before the per-
            // STAT dy nudge, while bricks (kind 9) keep the raw tile-bottom
            // anchor. Omitting it drew flames ~kTileH/2 px too low.
            const int dy_centre = sp.dy - sim::kTileH / 2;  // integer semantics intended
            draw_sprite(sp, sx + static_cast<float>(sp.dx), sy + static_cast<float>(dy_centre));
        }
    }

    sample_movement(s);

    // Players, bottom-anchored, in FIXED SLOT ORDER 0..9 every frame. The
    // original's per-player loop (sub_420F07, native batch_0x420D4E.cpp:176
    // `for (i = 0; i < 10; ++i) sub_41F29B(&dword_461BC4[38*i])`, pseudo.c
    // ~23639; facts.md "Draw order" — "slots 0..9 ascending") has no Y-sort,
    // depth buffer, or re-ordering: the higher SLOT always wins an overlap
    // regardless of screen position. A prior pseudo-3D Y-sort (lower player
    // drawn in front) was removed here — it silently changed which sprite won
    // an overlap on every multiplayer round.
    for (int i = 0; i < sim::kMaxPlayers; ++i) {
        const sim::Player& p = s.players[i];
        if (!p.present || !p.alive) continue;
        // Sub-frame trace playback (player_interp): position AND facing come
        // from the active intra-tick segment, so the original's per-frame
        // micro-motion — the AI's stutter-step direction flips, the human
        // wall-vibrate — reaches the screen instead of being lerped away
        // between the two 20 Hz endpoints. The shadow, body, and the carried
        // bomb below all anchor off this one interpolated position.
        int dir = static_cast<int>(p.facing);
        const Posf ip = player_interp(s, i, dir);
        float sx = kFieldOriginX + ip.x;
        float sy = kFieldOriginY + ip.y + sim::kTileH / 2.0f - 1.0f;
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
            const int c = len - p.bounce;                // elapsed frames: 0 at launch .. len-1
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
        int pv = render_colour(s, i);
        // A diseased player's body sprite strobes — CONFIRMED exact mechanism
        // (sub_41F29B ~23252, traced 2026-07-09): after the shadow blit, the
        // per-player draw-colour byte (+0x3C) that normally selects the FRAME
        // within the current pose sequence (one frame per player colour, 0-9)
        // is replaced by `rand() % 10` whenever the disease-timer word's bit 3
        // is set (`v111[60] & 8`, the +120 countdown WORD, distinct from
        // +0x3C): the body is redrawn in a RANDOM one of the ten real player
        // colours, not an arbitrary tint. `body_colour` reproduces that by
        // swapping in a presentation-RNG colour index for every pose branch
        // below (walk/stand/cornerhead/kick/punch/carry/spin all key off it).
        //
        // The `& 8` gate is the load-bearing part: it PULSES the strobe
        // 8-tick-on / 8-tick-off (0.4 s buzz, 0.4 s calm at 20 Hz) rather than
        // running it uniformly. That clustered pulse is what makes the strobe
        // read as a distinct "I am diseased" state — the SOLE ongoing cue for
        // the no-bomb Constipation disease, whose placement block otherwise
        // looks like "I can't place bombs for no reason." A prior port gated on
        // `s.tick & 1` instead — a documented deviation that produced a ~10 Hz
        // shimmer easily dismissed as a render artifact; restored here to the
        // confirmed timer-bit mechanism so the pulse is legible. disease_timer
        // is hashed sim state but this only READS it — presentation only, no
        // golden impact (State::rng is untouched; flash colour uses flash_lcg_).
        bool disease_flash = (p.disease_timer & 8) != 0;
        int body_colour = disease_flash ? disease_flash_colour() : pv;
        const Anim* a = moving_[i] ? &q.walk[body_colour][dir] : &q.stand[body_colour][dir];
        // Leg-cycle pacing: ONE anim frame per THREE pixels walked. The
        // original's pose frame is `(u16)player[+48] / 3 % statecnt`
        // (sub_41F29B pseudo.c 23410), and +48 advances once per PIXEL step
        // inside the mover's per-pixel loop (sub_41EC84, 22718). walk_phase_
        // accumulates the same per-tick pixel budget (PlayerWalking event),
        // so /3 here reproduces the original exactly. Without it the cycle
        // ran 3x fast AND froze outright whenever the per-tick pixel count
        // hit a multiple of the sequence length (the reported "walk anim
        // stops after some skates" — e.g. 10 px/tick vs a 10-frame WALK.ANI).
        // docs/re/facts.md "Walk leg-cycle pacing".
        std::size_t ph = moving_[i] ? walk_phase_[i] / 3 : 0;
        // Boxed-in idle: replace the pose with the current "cornerhead" fidget
        // (direction-independent). sample_movement already rolled the variant/
        // duration this tick; the phase just rides the sim tick so the frames
        // advance. NOT gated on moving_: the original's fidget states 20-39
        // are entered off enclosure alone (sub_41F29B 23006-23013) and held
        // keys don't exit them — a boxed-in player mashing into the walls
        // still fidgets, it doesn't pedal. Falls back to stand if the CORNER
        // ANI is missing.
        if (panic_active_[i] &&
            !q.cornerhead[body_colour][panic_variant_[i]].steps.empty()) {
            a = &q.cornerhead[body_colour][panic_variant_[i]];
            // Elapsed-since-entry phase (sub_41F29B draws the fidget at its own
            // up-counter, pseudo.c ~23238), so each rolled variant plays a clean
            // 0..statecnt-1 cycle before sample_movement re-rolls the next one —
            // not a raw global-tick phase that could start mid-animation.
            ph = static_cast<std::size_t>(panic_elapsed_[i]);
        }
        // A recent kick/punch overrides walk/stand with the action pose, played
        // once over its lifetime (falls back to walk/stand if the ANI is
        // missing so nothing ever blanks out).
        // elapsed = the sequence's own length - the remaining countdown (which
        // on_events seeded FROM that same length, F3) — the original's
        // elapsed-from-0 frame, ticking up to statecnt.
        if (kick_pose_[i] > 0 && !q.kick[body_colour][dir].steps.empty()) {
            a = &q.kick[body_colour][dir];
            ph = static_cast<std::size_t>(static_cast<int>(a->steps.size()) - kick_pose_[i]);
        } else if (punch_pose_[i] > 0 && !q.punch[body_colour][dir].steps.empty()) {
            a = &q.punch[body_colour][dir];
            ph = static_cast<std::size_t>(static_cast<int>(a->steps.size()) - punch_pose_[i]);
        }
        // Carrying a grabbed bomb wins over the idle fidget and the kick/punch
        // poses (a carrying player can't kick/punch): show the "holding a bomb"
        // walk/stand pose (BWALK*.ANI), same phase convention as walk/stand.
        // Falls back to the plain walk/stand already selected if the ANI is
        // missing so nothing ever blanks out.
        if (p.carrying) {
            const Anim* c =
                moving_[i] ? &q.walkbomb[body_colour][dir] : &q.standbomb[body_colour][dir];
            if (!c->steps.empty()) {
                a = c;
                // Same +48/3 pacing as walk/stand above — the original's
                // carry poses share the one counter and the one /3 site.
                ph = moving_[i] ? walk_phase_[i] / 3 : 0;
            }
            // The "picking up" transitional pose (PUP*.ANI, sub_41F29B
            // action-state 4) wins over the steady carry pose while its
            // countdown runs — the original enters state 4 on the grab and
            // only then settles into the carry poses. Played once, front to
            // back (its length was set from this very sequence's step count).
            const Anim& up = q.pickup[body_colour][dir];
            if (pickup_pose_[i] > 0 && !up.steps.empty()) {
                a = &up;
                // The DISPLAYED frame is walk-phase-driven, NOT elapsed-since-
                // grab: the original unconditionally recomputes v110 =
                // sub_41DAA7(seq, (u16)player[+0x30] / 3) at the shared draw
                // tail (pseudo.c 23410; disasm-confirmed 0x420350-0x420379,
                // `idiv ebx` with ebx=3), discarding the elapsed-based frame the
                // pickup block computed. Only pickup_pose_'s countdown (the
                // state's exit timer, set from the sequence length) survives.
                // Same +48/3 walk leg-cycle as the walk/stand/carry poses above.
                ph = moving_[i] ? walk_phase_[i] / 3 : 0;
            }
        }
        // Warp/teleport pose wins over everything: while warping the player is
        // fully state-gated (states 6/7, no walk/kick/carry), and the original
        // draws the "spin" sequence (sub_41F29B strcpy'd @0x45a213) both phases,
        // advancing its frame by the per-phase counter (+80). Player::warp counts
        // 18→0, so elapsed = kWarpTicks - warp drives the frame; draw_anim's
        // `% statecnt` cycles the spin art across the 9-out + 9-in ticks. Falls
        // back to the pose already selected if WALK.ANI has no "spin".
        if (p.warp > 0 && !q.spin[body_colour].steps.empty()) {
            a = &q.spin[body_colour];
            ph = static_cast<std::size_t>(kWarpTicks - p.warp);
        }
        draw_anim(*a, ph, sx, sy);
        if (p.carrying) {  // held bomb rides above the head
            int bo = render_colour(s, p.carried_colour);
            // Bomb-pickup carry arc (docs/re/id-audit.md item 4; VALUELST
            // 500/502/504/506, "the curve (upwards) of a bomb being picked
            // up"). Pinned consumer: the BOMB tick function `sub_42331C`'s
            // "carried" state-3 branch (pseudo.c ~25488-25497), gated on the
            // CARRIER's player-state field +78 == 4 ("picking up"):
            //   v60 = clamp((carrier.+80 elapsed-frames) - 1, 0, 3);
            //   x = 10*dx[dir] + carrier.x;  bomb.x = x + getvalue(2*v60+500)*dx[dir];
            //   y = 10*dy[dir] + carrier.y;  bomb.y = y - getvalue(2*v60+501);
            // i.e. a small forward nudge (+10px) plus the curve's own forward
            // reach in the facing direction, and a vertical lift that grows
            // from the curve's Y column (10/20/30/40 px) as the carry ages;
            // v60 clamps at 3 so the bomb settles at the LAST curve point
            // (12,40) for the remainder of the carry, not just a 4-frame pop.
            // carry_ticks_ (sample_movement) mirrors the +80 elapsed-frames
            // counter, already clamped 0..3. Read live off VALUELST so a
            // modified install's curve/columns change the arc; falls back to
            // the shipped values. dx/dy match our Direction enum order
            // (Up,Down,Left,Right — grid::dir_dx/dir_dy are sim-internal, so
            // this mirrors them locally for the renderer).
            static constexpr int kCarryArcIds[4] = {500, 502, 504, 506};
            static constexpr float kCarryArcXDefault[4] = {12, 25, 25, 12};
            static constexpr float kCarryArcYDefault[4] = {10, 20, 30, 40};
            static constexpr float kCarryDirDx[4] = {0, 0, -1, 1};  // Up,Down,Left,Right
            static constexpr float kCarryDirDy[4] = {-1, 1, 0, 0};
            const int t = carry_ticks_[i];
            const float cx =
                values_ ? static_cast<float>(values_->column_or(
                              kCarryArcIds[t], 0, static_cast<std::int64_t>(kCarryArcXDefault[t])))
                        : kCarryArcXDefault[t];
            const float cy =
                values_ ? static_cast<float>(values_->column_or(
                              kCarryArcIds[t], 1, static_cast<std::int64_t>(kCarryArcYDefault[t])))
                        : kCarryArcYDefault[t];
            const float bx = sx + kCarryDirDx[dir] * (cx + 10.0f);
            const float by = sy + kCarryDirDy[dir] * 10.0f - cy;
            draw_anim(q.bomb[bo], pulse, bx, by);
        }
    }

    // Campaign rover/ghost hazards (docs/re/campaign.md "Per-tick mover").
    // ALIENS1.ANI "ghost <dir>"/"rover <dir>" (CORRECTED 2026-07-09,
    // sequences.hpp's rover/ghost comment — was previously believed cut
    // content under the wrong filename). Bottom-anchored like every other
    // world entity here. Falls back to a plain filled marker (rover =
    // brown/orange, ghost = pale blue-white) when the sequence is missing,
    // so a partial install still shows something instead of nothing.
    for (std::size_t ri = 0; ri < s.rovers.size(); ++ri) {
        const auto& r = s.rovers[ri];
        if (!r.alive) continue;
        const bool prev_ok = ri < prev_rovers_.size() && prev_rovers_[ri].active;
        const Posf ip = interp_pos(prev_ok ? prev_rovers_[ri].x : 0,
                                   prev_ok ? prev_rovers_[ri].y : 0, r.x, r.y, prev_ok);
        float sx = kFieldOriginX + ip.x;
        float sy = kFieldOriginY + ip.y + sim::kTileH / 2.0f - 1.0f;
        const Anim& a = (r.kind == sim::RoverKind::Rover) ? q.rover[r.dir & 3] : q.ghost[r.dir & 3];
        if (!a.steps.empty()) {
            draw_anim(a, r.anim_step, sx, sy);
            continue;
        }
        constexpr float kMarkerW = 24.0f, kMarkerH = 24.0f;
        SDL_FRect dst{sx - kMarkerW / 2.0f, sy - kMarkerH - 4.0f, kMarkerW, kMarkerH};
        if (r.kind == sim::RoverKind::Rover)
            SDL_SetRenderDrawColor(ren_, 170, 90, 30, 255);  // rover: brown/orange
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

    // Gold Bomberman twinkle overlay (docs/re/goldman-roulette.md §6): drawn
    // last so the sparkles read on top of the player sprite, matching the
    // original's dedicated post-pass (`sub_420E39`, called after the main
    // per-player loop in `sub_420F07`). Each particle's frame advances once
    // per REAL ENGINE FRAME here (batch_0x420D4E.cpp:136), NOT per sim tick —
    // draw_world runs on the render clock, so `sp.age` doubles as the frame
    // index and the retirement counter, bounded by the sequence length.
    const int sparkle_lifetime = static_cast<int>(goldman_anim_.steps.size());
    for (auto& sp : gold_sparkles_) {
        if (!sp.active) continue;
        if (sparkle_lifetime <= 0 || sp.age >= sparkle_lifetime) {
            sp.active = false;
            continue;
        }
        draw_anim(goldman_anim_, static_cast<std::size_t>(sp.age), sp.x, sp.y);
        ++sp.age;
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

void Renderer::draw_frame(const sim::State& s, float alpha) {
    capture_interp(s);
    interp_alpha_ = alpha;
    SDL_SetRenderDrawColor(ren_, 0, 0, 0, 255);
    SDL_RenderClear(ren_);
    SDL_RenderTexture(ren_, assets_->field(), nullptr, nullptr);
    // Static solid/brick tiles first: they are part of the original's
    // BACKGROUND surface (sub_425D22 stamps them into it), so every sprite
    // pass below — including a flying bomb mid-arc — composites over them.
    draw_cells(s);
    draw_actors(s);  // conveyor/trampoline floor tiles, under powerups + entities
    // Order matches sub_42A191's per-frame call sequence (docs/re/facts.md
    // "Draw order"): actors, then bombs, then powerups, then burn/flame
    // (inside draw_world), then players (also draw_world) last.
    draw_bombs(s);
    draw_powerups(s);
    draw_world(s);
    draw_hud(s);
}

}  // namespace bomber::game
