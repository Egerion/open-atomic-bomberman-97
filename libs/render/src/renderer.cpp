#include "bomber/render/renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "bomber/game_util/death_anim.hpp"
#include "bomber/game_util/anim_pace.hpp"
#include "bomber/game_util/carry_pose.hpp"
#include "bomber/game_util/hud_format.hpp"
#include "bomber/match/team_colour.hpp"

namespace bomber::game {

// Trampoline hop height per elapsed frame. CONFIRMED VALUELST id 681 = 35
// ("how many pixels vertically do you move each frame?"), read by sub_41F29B
// state 5. Presentation-only — the integer sim omits it; docs/re/stage-actors.md §4.
constexpr int kHopPixelsPerFrame = 35;

// Total warp duration in sim ticks (9 warp-out + 9 warp-in). Mirrors
// StageActorSystem::kWarpTicks (a private sim header); the "spin" animation
// (WALK.ANI, sub_41F29B states 6/7) advances by elapsed = kWarpTicks - warp.
constexpr int kWarpTicks = 18;

// HURRY! banner duration — DERIVED from the id-101 window, not chosen. The HUD
// (sub_42A191) draws the banner while `remaining < getvalue(101)` AND
// `remaining > getvalue(101) - 5`, both STRICT, so over floored seconds that
// open interval is exactly four and the walls arm on the tick it closes
// (docs/re/enclosure.md §2). A literal `+ 60` turned it off a second early.
constexpr int kHurryWallArmLeadSeconds = 5;                        // the `- 5` in getvalue(101) - 5
constexpr int kHurryBannerSeconds = kHurryWallArmLeadSeconds - 1;  // strict `>` drops one
constexpr std::uint64_t kHurryBannerTicks =
    static_cast<std::uint64_t>(kHurryBannerSeconds) * sim::kTicksPerSecond;

// Inter-tick interpolation snap threshold (docs/render-notes.md §1). Anything a
// moving entity legitimately covers in ONE 20 Hz tick stays well under this;
// warps, trampoline landings and the flying-bomb field wrap move a full tile or
// more and must SNAP rather than smear across the screen.
constexpr sim::Fixed kInterpSnapDelta = 32 * sim::kScale;

// Marker sizes for the rover/ghost fallback when ALIENS1.ANI is absent.
constexpr float kMarkerW = 24.0f;
constexpr float kMarkerH = 24.0f;

void Renderer::capture_interp(const sim::State& s) {
    if (s.tick == interp_tick_) return;
    // A tick we have never seen makes the previous snapshot meaningless only
    // when there IS none; stale cross-round data is caught by reset_match and,
    // failing that, by the snap threshold.
    interp_valid_ = interp_tick_ != ~0ull;
    prev_px_ = seen_px_;
    prev_py_ = seen_py_;
    prev_bombs_.swap(seen_bombs_);
    prev_rovers_.swap(seen_rovers_);
    for (int i = 0; i < sim::kMaxPlayers; ++i) {
        seen_px_[i] = s.players[i].x;
        seen_py_[i] = s.players[i].y;
    }
    trace_ = s.sub_trace;  // this tick's per-sub-frame motion, for player_interp
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
    // Segment f runs from sample f-1 (or the previous tick's endpoint for
    // f == 0) to sample f. Sample kSubFrames-1 is pinned to the tick's true
    // endpoint by the sim (run_tick step 12), so alpha -> 1 converges on
    // exactly the position the old endpoint lerp used.
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
    // Same both-axes snap rule as interp_pos, applied per SEGMENT.
    if (std::abs(seg_end.x - x0) > kInterpSnapDelta || std::abs(seg_end.y - y0) > kInterpSnapDelta)
        return {ex, ey};
    const float sx = static_cast<float>(x0) / static_cast<float>(sim::kScale);
    const float sy = static_cast<float>(y0) / static_cast<float>(sim::kScale);
    return {sx + (ex - sx) * local, sy + (ey - sy) * local};
}

Renderer::Posf Renderer::interp_pos(PrevPos prev, sim::Fixed x, sim::Fixed y) const {
    // Bombs/rovers step on the 20 Hz systems grid in BOTH modes, so on the F9
    // path (where players are drawn direct) they glide on the systems
    // accumulator fraction instead — otherwise a flying bomb stutters at 20 Hz
    // against a per-frame world. docs/render-notes.md §2.
    const float a = native_cadence_ ? entity_alpha_ : interp_alpha_;
    const float fx = static_cast<float>(x) / static_cast<float>(sim::kScale);
    const float fy = static_cast<float>(y) / static_cast<float>(sim::kScale);
    if (!interp_valid_ || !prev.ok || a >= 1.0f) return {fx, fy};
    // Snap both axes together: lerping the small axis of a mostly-teleport move
    // would draw one frame at a position the entity never occupied.
    if (std::abs(x - prev.x) > kInterpSnapDelta || std::abs(y - prev.y) > kInterpSnapDelta)
        return {fx, fy};
    const float pfx = static_cast<float>(prev.x) / static_cast<float>(sim::kScale);
    const float pfy = static_cast<float>(prev.y) / static_cast<float>(sim::kScale);
    return {pfx + (fx - pfx) * a, pfy + (fy - pfy) * a};
}

void Renderer::draw_sprite(const Sprite& sp, Posf at, Tint tint) {
    // HD override when enabled and authored for this frame; the classic
    // w/h/hx/hy are kept, so the higher-res texture is sampled into the same
    // logical dst rect (the front-end HD PCX trick, asset_store.cpp).
    SDL_Texture* tex = (assets_->hd_enabled() && sp.tex_hd) ? sp.tex_hd : sp.tex;
    if (!tex) return;
    SDL_FRect dst{at.x - sp.hx, at.y - sp.hy, static_cast<float>(sp.w), static_cast<float>(sp.h)};
    const bool tinted = tint.r != 255 || tint.g != 255 || tint.b != 255;
    if (tinted) SDL_SetTextureColorMod(tex, tint.r, tint.g, tint.b);
    SDL_RenderTexture(ren_, tex, nullptr, &dst);
    if (tinted) SDL_SetTextureColorMod(tex, 255, 255, 255);
}

void Renderer::draw_anim(const Anim& a, std::size_t step, Posf at, Tint tint) {
    if (a.steps.empty()) return;
    // frame = counter % statecnt — the original ANI player (sub_41DAA7). The
    // STAT HEAD timing field is inert; see anim_pace.hpp / docs/re/facts.md.
    draw_sprite(a.steps[anim_step_index(step, a.steps.size())], at, tint);
}

int Renderer::disease_flash_colour() {
    flash_lcg_ = flash_lcg_ * 1664525u + 1013904223u;
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

void Renderer::spawn_gold_sparkle(const sim::Player& p) {
    // sub_420D4E: scan for the FIRST empty slot; exactly one attempt (spawn or
    // not) per matching player per tick, never a fresh scan per particle.
    const auto it = std::find_if(gold_sparkles_.begin(), gold_sparkles_.end(),
                                 [](const GoldSparkle& sp) { return !sp.active; });
    if (it == gold_sparkles_.end()) return;
    if (gold_roll() % 6 == 0) return;  // 5-in-6 chance to actually place it
    const float px = kFieldOriginX + p.x / static_cast<float>(sim::kScale);
    const float py =
        kFieldOriginY + p.y / static_cast<float>(sim::kScale) + sim::kTileH / 2.0f - 1.0f;
    it->active = true;
    it->age = 0;
    // rand()%40 + player_x - 20, rand()%50 + player_y - 48 — fixed at spawn;
    // the particle does not track the player afterwards.
    it->x = px + static_cast<float>(gold_roll() % 40) - 20.0f;
    it->y = py + static_cast<float>(gold_roll() % 50) - 48.0f;
}

// Gold Bomberman "twinkle" SPAWN (sub_420D4E), once per sim tick. Ageing and
// retirement is NOT here — the original ages each particle once per ENGINE
// FRAME (sub_420E39), so it lives in draw_gold_sparkles instead; ageing at
// 20 Hz made the sparkles ~9x too slow. docs/render-notes.md §5.
void Renderer::update_gold_sparkles(const sim::State& s) {
    if (gold_player_ < 0) return;
    // getvalue(1010): twinkle duration in seconds, 0 = indefinitely (the file's
    // own legend). A fresh Simulation is built per round, so s.tick already IS
    // the round-elapsed clock.
    const std::int64_t duration = values_ ? values_->at_or(1010, 5) : 5;
    if (duration != 0 && static_cast<std::int64_t>(s.tick) / sim::kTicksPerSecond >= duration)
        return;
    for (int i = 0; i < sim::kMaxPlayers; ++i) {
        const sim::Player& p = s.players[i];
        if (!p.present || !p.alive) continue;
        // dword_46492C holds the clinching player's TEAM id in team play and
        // the player SLOT in solo play (assign_gold_player's doc comment).
        const bool is_gold = gold_team_mode_ ? (p.team == gold_player_) : (i == gold_player_);
        if (is_gold) spawn_gold_sparkle(p);
    }
}

int Renderer::render_colour(const sim::State& s, int slot) {
    if (slot < 0 || slot >= sim::kMaxPlayers) return 0;
    return match::team_render_colour(s.players[slot].team, slot);
}

bool Renderer::boxed_in(const sim::State& s, int tx, int ty) {
    // "Blocked" = not walkable: outside the grid, a solid/brick/burning tile, or
    // a resting (non-flying) bomb on it. Mirrors sub_41F29B's neighbour scan.
    static constexpr int dx[4] = {0, 0, -1, 1};
    static constexpr int dy[4] = {-1, 1, 0, 0};
    for (int k = 0; k < 4; ++k) {
        const int nx = tx + dx[k], ny = ty + dy[k];
        if (nx < 0 || nx >= sim::kGridWidth || ny < 0 || ny >= sim::kGridHeight) continue;
        if (s.cells[ny][nx] == sim::Cell::Solid || s.cells[ny][nx] == sim::Cell::Brick ||
            s.burning[ny][nx] > 0)
            continue;  // blocked by terrain
        const bool bomb_here = std::any_of(s.bombs.begin(), s.bombs.end(), [&](const auto& b) {
            return b.active && !b.flying && b.tile_x() == nx && b.tile_y() == ny;
        });
        if (!bomb_here) return false;  // an open neighbour => not boxed in
    }
    return true;
}

const Anim& Renderer::flame_piece(const FlameSet& fset, sim::FlameKind kind) {
    // 1:1 with FlameKind's own off_45BEA0-mirroring order (types.hpp) — the sim
    // decides the piece once at ignition (FlameSystem::spread_to), so this is a
    // plain lookup, not a live neighbour scan.
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
    body_phase_.fill(0);
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

void Renderer::set_action_pose(int slot, ActionPose which, int frames) {
    // The +78 action-state word is ONE word, so entering a state ends whichever
    // was running: the kick dispatch writes `+78 = 1` outright (sub_41EC84
    // 22617-22624), clobbering an in-progress punch or pickup. Our three
    // countdowns are independent timers, so reproduce the clobber explicitly.
    kick_pose_[slot] = which == ActionPose::Kick ? frames : 0;
    punch_pose_[slot] = which == ActionPose::Punch ? frames : 0;
    pickup_pose_[slot] = which == ActionPose::Pickup ? frames : 0;
}

void Renderer::start_action_pose(const sim::State& s, const sim::Event& ev, ActionPose which) {
    // Every real emitter sets `player` from a valid loop index; range-check it
    // anyway rather than trust that.
    if (ev.player < 0 || ev.player >= sim::kMaxPlayers) return;
    // Each pose plays for its own sequence's frame count, not a fixed tick
    // budget: sub_41F29B states 1/2/4 exit when the elapsed frame passes the
    // sequence's statecnt (sub_41DA5C, pseudo.c ~23119-23145/~23396-23407).
    const int colour = render_colour(s, ev.player);
    const int dir = static_cast<int>(s.players[ev.player].facing);
    const Anim& seq = which == ActionPose::Kick    ? seqs_->kick[colour][dir]
                      : which == ActionPose::Punch ? seqs_->punch[colour][dir]
                                                   : seqs_->pickup[colour][dir];
    set_action_pose(ev.player, which, static_cast<int>(seq.steps.size()));
}

void Renderer::on_player_died(const sim::State& s, const sim::Event& ev) {
    if (ev.player < 0 || ev.player >= sim::kMaxPlayers) return;
    // render_colour resolves Team Play's white/red override, so a
    // disease-strobe-free death still shows the on-screen team colour.
    const int colour = render_colour(s, ev.player);
    const auto& pool = assets_->deaths_for(colour);
    if (pool.empty()) return;
    DeathFx fx;
    fx.player = colour;
    // WHICH death animation, shared with the audio side: the original has one
    // `actor[+4]` that both the `die green %d` sprite name and
    // `sub_4278F2(340 + actor[+4])` read (docs/re/sound-engine.md §10). The port
    // keeps that one-value property without a hashed field by deriving it here
    // and in SoundDirector from the same pure function of (tick, victim slot).
    fx.anim = death_anim_slot(s.tick, ev.player, pool.size());
    fx.x = kFieldOriginX + s.players[ev.player].x / static_cast<float>(sim::kScale);
    fx.y = kFieldOriginY + s.players[ev.player].y / static_cast<float>(sim::kScale) +
           sim::kTileH / 2.0f - 1.0f;
    fx.start = s.tick;
    deaths_.push_back(fx);
}

void Renderer::on_events(const sim::State& s, bool tick_advanced) {
    // Age the action poses once per SIM TICK. on_events runs once per tick on
    // the deterministic path but once per DISPLAYED FRAME on F9, where
    // `tick_advanced` is only true on the frame that crossed a 50 ms tick — so
    // the countdowns do not play ~9x too fast. docs/render-notes.md §2.
    if (tick_advanced) {
        for (int i = 0; i < sim::kMaxPlayers; ++i) {
            if (kick_pose_[i] > 0) --kick_pose_[i];
            if (punch_pose_[i] > 0) --punch_pose_[i];
            if (pickup_pose_[i] > 0) --pickup_pose_[i];
        }
    }
    for (const auto& ev : s.events) {
        switch (ev.type) {
            case sim::Event::Type::Hurry: hurry_until_ = s.tick + kHurryBannerTicks; break;
            case sim::Event::Type::BombKicked: start_action_pose(s, ev, ActionPose::Kick); break;
            case sim::Event::Type::BombPunched: start_action_pose(s, ev, ActionPose::Punch); break;
            case sim::Event::Type::BombGrabbed: start_action_pose(s, ev, ActionPose::Pickup); break;
            case sim::Event::Type::PlayerDied: on_player_died(s, ev); break;
            default: break;
        }
    }
}

void Renderer::update_fidget(const sim::State& s, int slot, bool displaced) {
    // sub_41F29B (~23011 entry, ~23236-23246 exit) rolls a VARIANT once —
    // `rand() % getvalue(330) + 20`, where id 330 is the variant COUNT, never a
    // duration — and holds it until that variant's own ANI plays one full
    // cycle, then re-rolls. So the re-roll cadence is the chosen art's own
    // length, not a fixed `20 + rand()%13` spread.
    const sim::Player& p = s.players[slot];
    const bool panic = p.present && p.alive && !displaced && boxed_in(s, p.tile_x(), p.tile_y());
    if (!panic) {
        panic_active_[slot] = false;
        panic_elapsed_[slot] = 0;
        return;
    }
    if (!panic_active_[slot]) {
        panic_variant_[slot] = static_cast<int>(panic_roll() % kCornerheadVariants);
        panic_elapsed_[slot] = 0;
        panic_active_[slot] = true;
        return;
    }
    ++panic_elapsed_[slot];
    const int len = static_cast<int>(
        seqs_->cornerhead[render_colour(s, slot)][panic_variant_[slot]].steps.size());
    if (len > 0 && panic_elapsed_[slot] < len) return;
    panic_variant_[slot] = static_cast<int>(panic_roll() % kCornerheadVariants);
    panic_elapsed_[slot] = 0;
}

void Renderer::update_carry(const sim::Player& p, int slot, std::int8_t walk_px) {
    // Ticks elapsed since carrying started, 0 on the grab tick — the original's
    // player+80 "elapsed since state entry" counter (docs/re/id-audit.md item
    // 4). Clamped at 4 because carry_arc_index saturates at 3 = 4 - 1.
    const bool carrying_now = p.present && p.alive && p.carrying;
    if (carrying_now)
        carry_ticks_[slot] = carrying_prev_[slot] ? std::min(carry_ticks_[slot] + 1, 4) : 0;
    // The +48 body anim counter (carry_pose.hpp): held at 0 for the whole carry
    // and the release tick, then advanced by this tick's walk budget or, idle,
    // by one per displayed frame — the idle branch's `++[+48]` is per FRAME, so
    // a tick is worth kSubFrames of it at the canonical cadence.
    body_phase_[slot] = body_phase_next(body_phase_[slot], carrying_now, carrying_prev_[slot],
                                        walk_px, sim::kSubFrames);
    carrying_prev_[slot] = carrying_now;
}

void Renderer::sample_movement(const sim::State& s) {
    // Once per sim tick normally; EVERY displayed frame under F9, where only
    // the walk leg phase advances per frame and the rest stays on the 20 Hz
    // grid via `new_tick`. docs/render-notes.md §2.
    const bool new_tick = s.tick != last_tick_;
    if (!native_cadence_ && !new_tick) return;
    // Walk state comes from the sim's PlayerWalking events, NOT the position
    // delta: the original burns the tick's speed budget even when a wall blocks
    // every pixel step (sub_41EC84), so a blocked walker pedals in place, while
    // a conveyor sliding an IDLE player emits no event and glides in the stand
    // pose. The event's data is the disease-scaled budget in px.
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
        if (!new_tick) continue;  // F9 mid-tick frame: leg phase only
        // Displacement is tracked separately: the fidget below cares about
        // standing STILL while fully enclosed (its entry, sub_41F29B
        // 23006-23013, checks only enclosure + state 0 — held keys don't cancel
        // it, and boxed in they can't displace you anyway).
        const bool displaced =
            last_tick_ != ~0ull && p.present && p.alive && (p.x != last_x_[i] || p.y != last_y_[i]);
        last_x_[i] = p.x;
        last_y_[i] = p.y;
        update_fidget(s, i, displaced);
        update_carry(p, i, walk_px[i]);
    }
    if (!new_tick) return;
    update_gold_sparkles(s);
    last_tick_ = s.tick;
}

std::size_t Renderer::tramp_frame(const sim::State& s, FieldCell c) const {
    // The bounce counts down from tuning.trampoline_bounce_frames (VALUELST
    // 680); frame 0 = the resting mat. The "extra trampoline" ANI is 12 frames,
    // so draw_anim's `% statecnt` maps the 30-tick bounce onto the 12 cels.
    for (const auto& p : s.players) {
        if (!p.present || !p.alive || p.bounce <= 0) continue;
        if (p.tile_x() == c.x && p.tile_y() == c.y)
            return static_cast<std::size_t>(s.tuning.trampoline_bounce_frames - p.bounce);
    }
    return 0;  // no one bouncing here: the resting frame
}

void Renderer::draw_actor_cell(const sim::State& s, FieldCell c) {
    const sim::ActorType at = s.actor_type[c.y][c.x];
    if (at == sim::ActorType::None) return;
    // sub_4056CA draws over the SAME background surface sub_425D22 stamps tiles
    // into, so it needs a solid gate — and the gate is PER TYPE, not uniform:
    // dirarrow/conveyor/trampoline are gated on `!sub_425FB9(x,y)` (== our
    // `cells == Cell::Blank`), the warphole has no solid test at all.
    // docs/re/stage-actors.md §3-5.
    if (at != sim::ActorType::Warphole && s.cells[c.y][c.x] != sim::Cell::Blank) return;
    // Each actor's own frame counter (+48) advances once PER DRAW: dirarrows
    // and warpholes animate at the full per-frame rate, the conveyor at
    // counter/3 (a slow belt), the trampoline only while bounced. Drawing them
    // at a fixed frame 0 froze the dormant "closed" cel.
    const SequenceSet& q = *seqs_;
    const std::size_t belt_step = static_cast<std::size_t>(s.tick / 3);
    const std::size_t full_step = static_cast<std::size_t>(s.tick);
    const Posf anchor = tile_anchor(c);
    const int g = s.actor_dir[c.y][c.x] & 3;
    switch (at) {
        case sim::ActorType::Conveyor: draw_anim(q.conveyor[g], belt_step, anchor); break;
        case sim::ActorType::DirArrow: draw_anim(q.dirarrow[g], full_step, anchor); break;
        case sim::ActorType::Warphole: draw_anim(q.warphole, full_step, anchor); break;
        case sim::ActorType::Trampoline: draw_anim(q.trampoline, tramp_frame(s, c), anchor); break;
        default: break;
    }
}

// Conveyor / dirarrow / warphole / trampoline floor tiles, drawn UNDER powerups
// and entities (the lowest floor layer above the field).
void Renderer::draw_actors(const sim::State& s) {
    for (const FieldCell c : field_cells) draw_actor_cell(s, c);
}

void Renderer::draw_powerup_cell(const sim::State& s, FieldCell c) {
    if (s.floor[c.y][c.x] == sim::PowerupType::None) return;
    // A brick's hidden token flips to "visible" in sim state the instant the
    // brick ignites (sub_425107's unconditional reveal), but the original's
    // floor-powerup drawer (sub_424F89, pseudo.c ~26247-26261) has its own gate
    // — the cell must also read blank — so the token stays hidden under the
    // crumble animation and only pops into view once the tile actually opens.
    if (s.cells[c.y][c.x] != sim::Cell::Blank) return;
    const int kind = static_cast<int>(s.floor[c.y][c.x]);
    // The animated "power <name>" sequence (POWERS.ANI), frame = counter %
    // statecnt (sub_4250DE / sub_41DAA7).
    const SequenceSet& q = *seqs_;
    if (kind >= 0 && kind < sim::kPowerupKinds && !q.powerup_anim[kind].steps.empty()) {
        draw_anim(q.powerup_anim[kind], static_cast<std::size_t>(s.tick), tile_anchor(c));
        return;
    }
    // Fallback: the static POW*.PCX tile-fill (top-left anchor) when the
    // animated sequence is unavailable.
    const Sprite& p = assets_->powerup(kind);
    SDL_Texture* ptex = (assets_->hd_enabled() && p.tex_hd) ? p.tex_hd : p.tex;
    if (!ptex) return;
    SDL_FRect dst{tile_screen_x(c.x), tile_screen_y(c.y), static_cast<float>(p.w),
                  static_cast<float>(p.h)};
    SDL_RenderTexture(ren_, ptex, nullptr, &dst);
}

void Renderer::draw_powerups(const sim::State& s) {
    for (const FieldCell c : field_cells) draw_powerup_cell(s, c);
}

void Renderer::draw_bomb(const sim::State& s, std::size_t index) {
    const auto& b = s.bombs[index];
    if (!b.active) return;
    // Inter-tick smoothing for kicked slides and flight legs.
    const Posf ip = interp_pos(prev_entity(prev_bombs_, index), b.x, b.y);
    float bx = ip.x;
    float by = ip.y;
    float lift = 0.0f;
    if (b.flying && b.fly_total > 0) {
        const float t = 1.0f - static_cast<float>(b.fly_ticks) / static_cast<float>(b.fly_total);
        // A HALF SINE over the leg, not a parabola: sub_42331C's draw tail
        // (pseudo.c ~25710-25726) blits at y - sin(pi * travelled / leg) * arc,
        // the literals read out of DGROUP at 0x45A2C9/D1/D9. `travelled / leg`
        // is exactly `t`. The parabola it replaces peaked at the same height
        // but bulged ~4% of it early in the throw.
        lift = static_cast<float>(b.fly_arc) * std::sin(3.14159265f * std::clamp(t, 0.0f, 1.0f));
        const float fw = static_cast<float>(sim::kGridWidth * sim::kTileW);
        const float fh = static_cast<float>(sim::kGridHeight * sim::kTileH);
        bx = std::fmod(std::fmod(bx, fw) + fw, fw);
        by = std::fmod(std::fmod(by, fh) + fh, fh);
    }
    const Posf at{kFieldOriginX + bx, kFieldOriginY + by + sim::kTileH / 2.0f - 1.0f - lift};
    // Colour byte, not the (chain-transferable) owner word — see Bomb::colour /
    // facts.md "Bomb/flame colour is not the owner".
    const int bo = render_colour(s, b.colour);
    draw_anim(bomb_anim(b, bo), static_cast<std::size_t>(s.tick), at);
}

// The "bomb %s green" pick, mirroring the original's EXCLUSIVE kind set at
// creation (trigger overrides jelly, sub_41EB13) plus the dud state suffix
// (sub_42331C). Each special case falls back to the plain pulse when its
// sequence is missing, so a bomb never blanks out.
const Anim& Renderer::bomb_anim(const sim::Bomb& b, int colour) const {
    const SequenceSet& q = *seqs_;
    if (b.dud_left > 0 && !q.bomb_dud[colour].steps.empty()) return q.bomb_dud[colour];
    if (b.trigger && !q.bomb_trigger[colour].steps.empty()) return q.bomb_trigger[colour];
    if (b.jelly && !b.trigger && !q.bomb_jelly[colour].steps.empty()) return q.bomb_jelly[colour];
    return q.bomb[colour];
}

// Bombs are their OWN pass, before powerups/flame/burn/players — sub_42A191's
// per-frame order (pseudo.c ~29488-29556). docs/render-notes.md §4.
void Renderer::draw_bombs(const sim::State& s) {
    for (std::size_t i = 0; i < s.bombs.size(); ++i) draw_bomb(s, i);
}

void Renderer::draw_cell_tile(const sim::State& s, FieldCell c) {
    const SequenceSet& q = *seqs_;
    if (s.cells[c.y][c.x] == sim::Cell::Solid) {
        draw_anim(q.solid, 0, tile_anchor(c));
        return;
    }
    // A burning brick draws NO static brick: the ignition stamp (sub_425EFC's
    // blank-then-revert dance) erases it from the background, leaving bare floor
    // for the crumble frames to composite over, even though the CELL stays Brick
    // (blocking) until `burning` expires.
    if (s.cells[c.y][c.x] == sim::Cell::Brick && s.burning[c.y][c.x] == 0)
        draw_anim(q.brick, 0, tile_anchor(c));
}

// The BACKGROUND layer: the original never draws these per frame at all —
// sub_425D22 STAMPS them into the background surface whenever a cell changes,
// so every sprite pass composites OVER them. docs/render-notes.md §4.
void Renderer::draw_cells(const sim::State& s) {
    for (const FieldCell c : field_cells) draw_cell_tile(s, c);
}

void Renderer::draw_burning_cell(const sim::State& s, FieldCell c) {
    if (s.burning[c.y][c.x] == 0) return;
    // sub_426D06's per-cell counter (+48) is zeroed at ignition and advanced by
    // exactly 1 per tick — NOT rescaled — so elapsed = brick_burn_frames -
    // remaining reproduces it (facts.md "Flame/burn frame pacing"). CLAMP to
    // the last cel rather than wrapping: the kind-9 branch holds the final cel,
    // and most XBRICKs have fewer cels than brick_burn_frames (9 vs 10), where
    // a wrap would flash the FULL brick for one frame. docs/render-notes.md §9.
    const SequenceSet& q = *seqs_;
    const std::size_t elapsed =
        static_cast<std::size_t>(s.tuning.brick_burn_frames - s.burning[c.y][c.x]);
    const std::size_t last = q.burn.steps.empty() ? 0 : q.burn.steps.size() - 1;
    draw_anim(q.burn, std::min(elapsed, last), tile_anchor(c));
}

// Brick-crumble frames (sub_426D06's kind-9 branch), over the bare floor the
// ignition stamp left behind — see draw_cell_tile.
void Renderer::draw_burning_bricks(const sim::State& s) {
    for (const FieldCell c : field_cells) draw_burning_cell(s, c);
}

void Renderer::draw_flame_cell(const sim::State& s, FieldCell c) {
    if (s.flame[c.y][c.x] == 0) return;
    // Colour from flame_colour, NOT flame_owner: the owner word is kill credit
    // and moves to the chainer on a chain hit, while the drawn colour (the
    // flame record's +60 byte) never transfers — overlapping explosions from
    // different players keep their own colours.
    const SequenceSet& q = *seqs_;
    const FlameSet& fset = q.flames[render_colour(s, s.flame_colour[c.y][c.x])];
    const Anim* a = &flame_piece(fset, s.flame_kind[c.y][c.x]);
    if (a->steps.empty()) return;
    // Same free-running, ignition-zeroed per-cell counter as the brick-burn
    // draw above (sub_426D06 drives both kinds off one +48 field).
    const std::size_t idx = anim_step_index(
        static_cast<std::size_t>(s.tuning.flame_frames - s.flame[c.y][c.x]), a->steps.size());
    const Sprite& sp = a->steps[idx];
    const Posf base = tile_anchor(c);
    // The ONE draw site that folds the per-STAT dx/dy in; everything else in
    // the game ignores them (docs/formats/ani.md "Rendering a step"). From the
    // disassembly at 0x426ee7-0x426f46: X = X_base + dx, Y = Y_base - tileH/2
    // + dy. The `- tileH/2` re-anchors flames to tile CENTRE while bricks keep
    // tile-bottom; omitting it drew flames ~kTileH/2 px too low.
    // docs/re/facts.md "Flame draw offset".
    const int dy_centre = sp.dy - sim::kTileH / 2;  // integer semantics intended
    draw_sprite(sp, {base.x + static_cast<float>(sp.dx), base.y + static_cast<float>(dy_centre)});
}

// Arm-piece selection reads the sim's `flame_kind`, decided once at ignition
// (sub_42331C's arm loop, pseudo.c ~25625/25673-25677). A previous live scan of
// neighbouring flame cells checkerboarded the mid-piece choice; it is gone.
void Renderer::draw_flames(const sim::State& s) {
    for (const FieldCell c : field_cells) draw_flame_cell(s, c);
}

// ONE attempt at the pose choice: the family select_player_pose lands on, its
// frame, and the flag to clear if that ANI turns out to be absent (null for the
// walk/stand base case, which has nothing left to fall back to). A faithful
// port of sub_41F29B's name-build dispatch, so the switch stays a switch
// (coding-standards §8); the per-family frame sources are tabulated in
// docs/render-notes.md §3.
Renderer::PosedAnim Renderer::pose_attempt(const sim::State& s, PoseView& v, bool*& drop) const {
    const SequenceSet& q = *seqs_;
    const sim::Player& p = s.players[v.slot];
    switch (select_player_pose(v.flags)) {
        case PlayerPose::Spin:
            drop = &v.flags.warping;
            return {&q.spin[v.colour], static_cast<std::size_t>(kWarpTicks - p.warp)};
        case PlayerPose::Kick:
        case PlayerPose::Punch: {
            const bool kicking = v.flags.kick;
            const Anim& a = kicking ? q.kick[v.colour][v.dir] : q.punch[v.colour][v.dir];
            const int left = kicking ? kick_pose_[v.slot] : punch_pose_[v.slot];
            drop = kicking ? &v.flags.kick : &v.flags.punch;
            return {&a, static_cast<std::size_t>(static_cast<int>(a.steps.size()) - left)};
        }
        case PlayerPose::Pickup:
            drop = &v.flags.pickup;
            return {&q.pickup[v.colour][v.dir], v.body_phase};
        case PlayerPose::Cornerhead:
            drop = &v.flags.cornerhead;
            return {&q.cornerhead[v.colour][panic_variant_[v.slot]],
                    static_cast<std::size_t>(panic_elapsed_[v.slot])};
        case PlayerPose::WalkBomb:
        case PlayerPose::StandBomb:
            drop = &v.flags.carrying;
            return {v.flags.moving ? &q.walkbomb[v.colour][v.dir] : &q.standbomb[v.colour][v.dir],
                    v.body_phase};
        case PlayerPose::Walk:
        case PlayerPose::Stand: break;
    }
    // The IDLE half takes its direction from the head-stun spin when one is
    // running (sub_41F29B's idle branch formats `stand %s` from `+80 & 3` while
    // +58 is set); the walking half is untouched.
    return {
        v.flags.moving
            ? &q.walk[v.colour][v.dir]
            : &q.stand[v.colour][stunned_stand_facing(v.dir, p.stun, s.tuning.head_stun_frames)],
        v.walk_phase};
}

Renderer::PoseView Renderer::pose_view(const sim::State& s, int slot, int dir) {
    const sim::Player& p = s.players[slot];
    // The diseased-body strobe — CONFIRMED sub_41F29B ~23252. The `& 8` gate is
    // the load-bearing part: it PULSES the strobe 8-on/8-off rather than running
    // it uniformly, and that clustered pulse is what makes it read as "I am
    // diseased" — the sole ongoing cue for no-bomb Constipation. A prior port
    // gated on `s.tick & 1`, a ~10 Hz shimmer dismissible as a render artifact.
    // Reads hashed state, never writes it. docs/render-notes.md §9.
    const bool disease_flash = (p.disease_timer & 8) != 0;
    // Then the round-start colour REVEAL (batch_0x41F29B.cpp:623-630). The
    // if/else-if order is the native's: the strobe wins, else the reveal, else
    // the team colour. Branches 2 and 3 live in match::round_start_body_colour
    // so the headless suite can pin them (tests/match/test_team.cpp); branch 1
    // stays here because it draws on a presentation-side RNG.
    const std::int64_t reveal_ticks = values_ ? values_->at_or(32, 40) : 40;
    PoseView v;
    v.slot = slot;
    v.dir = dir;
    v.colour = disease_flash ? disease_flash_colour()
                             : match::round_start_body_colour(s.tick, reveal_ticks, p.team, slot);
    v.flags.moving = moving_[slot];
    v.flags.carrying = p.carrying;
    v.flags.kick = kick_pose_[slot] > 0;
    v.flags.punch = punch_pose_[slot] > 0;
    // The pickup pose is NOT gated on `carrying`: the original's state 4
    // outlives the bomb, since the release clears +148 and never touches +78.
    v.flags.pickup = pickup_pose_[slot] > 0;
    v.flags.cornerhead = panic_active_[slot];
    v.flags.warping = p.warp > 0;
    // ONE anim frame per THREE pixels walked: the pose frame is
    // `(u16)player[+48] / 3 % statecnt` (sub_41F29B 23410) and +48 advances once
    // per PIXEL step inside sub_41EC84's loop, the same budget walk_phase_
    // accumulates. Without the /3 the cycle ran 3x fast AND froze whenever the
    // per-tick pixel count hit a multiple of the sequence length.
    // docs/re/facts.md "Walk leg-cycle pacing".
    v.walk_phase = moving_[slot] ? walk_phase_[slot] / (native_cadence_ ? 48u : 3u) : 0;
    v.body_phase = body_anim_step(body_phase_[slot]);
    return v;
}

Renderer::PosedAnim Renderer::pick_pose(const sim::State& s, PoseView v) const {
    // At most one flag is dropped per pass, so the loop always terminates well
    // inside its guard. docs/render-notes.md §3 for the fallback chain.
    PosedAnim out{};
    for (int pass = 0; pass < 8; ++pass) {
        bool* drop = nullptr;  // flag to clear and retry if this ANI is absent
        out = pose_attempt(s, v, drop);
        if (drop == nullptr || !out.anim->steps.empty()) break;
        *drop = false;
    }
    return out;
}

void Renderer::draw_carried_bomb(const sim::State& s, const PoseView& v, Posf at) {
    const sim::Player& p = s.players[v.slot];
    const int bo = render_colour(s, p.carried_colour);
    // Bomb-pickup carry arc (docs/re/id-audit.md item 4; VALUELST 500/502/504/
    // 506, read live so a modified install's curve changes the arc). Pinned
    // consumer: sub_42331C's "carried" state-3 branch (pseudo.c ~25488-25497),
    // which applies the curve ONLY while +78 == 4 (the pickup animation) and
    // otherwise sits the bomb straight above the head. docs/render-notes.md §9.
    // dx/dy mirror our Direction enum order (grid::dir_dx/dy are sim-internal).
    static constexpr int kCarryArcIds[4] = {500, 502, 504, 506};
    static constexpr int kCarryArcXDefault[4] = {12, 25, 25, 12};
    static constexpr int kCarryArcYDefault[4] = {10, 20, 30, 40};
    static constexpr float kCarryDirDx[4] = {0, 0, -1, 1};  // Up,Down,Left,Right
    static constexpr float kCarryDirDy[4] = {-1, 1, 0, 0};
    const int t = carry_arc_index(carry_ticks_[v.slot]);
    const int cx =
        values_ ? static_cast<int>(values_->column_or(kCarryArcIds[t], 0, kCarryArcXDefault[t]))
                : kCarryArcXDefault[t];
    const int cy =
        values_ ? static_cast<int>(values_->column_or(kCarryArcIds[t], 1, kCarryArcYDefault[t]))
                : kCarryArcYDefault[t];
    const CarryOffset off = carried_bomb_offset(pickup_pose_[v.slot] > 0, cx, cy);
    draw_anim(seqs_->bomb[bo], static_cast<std::size_t>(s.tick),
              {at.x + kCarryDirDx[v.dir] * static_cast<float>(off.forward),
               at.y + kCarryDirDy[v.dir] * 10.0f - static_cast<float>(off.lift)});
}

void Renderer::draw_player(const sim::State& s, int slot) {
    const sim::Player& p = s.players[slot];
    if (!p.present || !p.alive) return;
    const SequenceSet& q = *seqs_;
    // Sub-frame trace playback: position AND facing come from the active
    // intra-tick segment, so the original's per-frame micro-motion (the AI's
    // stutter-step flips, the human wall-vibrate) reaches the screen instead of
    // being lerped away between the two 20 Hz endpoints. The shadow, body and
    // carried bomb all anchor off this one interpolated position.
    int dir = static_cast<int>(p.facing);
    const Posf ip = player_interp(s, slot, dir);
    const float sx = kFieldOriginX + ip.x;
    float sy = kFieldOriginY + ip.y + sim::kTileH / 2.0f - 1.0f;
    // Trampoline flight lift. CONFIRMED from sub_41F29B state 5 (raw disasm
    // 0x4204b3..0x420517): y minus getvalue(681) * min(c, len - c), a linear
    // tent peaking at the apex, c = elapsed frames (the +80 up-counter; our
    // Player::bounce is the equivalent down-counter). Peak 35*15 = 525 px, so
    // the player rockets off the top of the field — the game's "fly".
    float lift = 0.0f;
    if (p.bounce > 0) {
        const int len = s.tuning.trampoline_bounce_frames;
        const int c = len - p.bounce;                // 0 at launch .. len-1
        const int tent = c < len - c ? c : len - c;  // min(c, len-c)
        lift = static_cast<float>(kHopPixelsPerFrame * tent);
    }
    // The shadow is blitted at the SAME (x,y) as the player, its own hotspot
    // doing the centring. EXCEPTION: the trampoline flight (state 5) skips it
    // entirely (the block jmps to LABEL_246 at 0x420870, past the shadow blit)
    // — the player is high in the air. The warp (states 6/7) DOES draw it, so
    // only a bounce suppresses it.
    if (p.bounce <= 0) draw_anim(q.shadow, 0, {sx, sy});
    sy -= lift;  // raise the body (and anything anchored to it) by the hop arc
    const PoseView v = pose_view(s, slot, dir);
    const PosedAnim pose = pick_pose(s, v);
    draw_anim(*pose.anim, pose.step, {sx, sy});
    if (p.carrying) draw_carried_bomb(s, v, {sx, sy});  // held bomb rides above the head
}

// FIXED SLOT ORDER 0..9: sub_420F07 has no Y-sort, depth buffer or re-ordering,
// so the higher SLOT always wins an overlap regardless of screen position. A
// prior pseudo-3D Y-sort was removed here — it silently changed which sprite
// won an overlap on every multiplayer round. facts.md "Draw order".
void Renderer::draw_players(const sim::State& s) {
    for (int i = 0; i < sim::kMaxPlayers; ++i) draw_player(s, i);
}

void Renderer::draw_rover(const sim::State& s, std::size_t index) {
    const auto& r = s.rovers[index];
    if (!r.alive) return;
    const SequenceSet& q = *seqs_;
    const Posf ip = interp_pos(prev_entity(prev_rovers_, index), r.x, r.y);
    const Posf at{kFieldOriginX + ip.x, kFieldOriginY + ip.y + sim::kTileH / 2.0f - 1.0f};
    const Anim& a = (r.kind == sim::RoverKind::Rover) ? q.rover[r.dir & 3] : q.ghost[r.dir & 3];
    if (!a.steps.empty()) {
        draw_anim(a, r.anim_step, at);
        return;
    }
    // Fallback marker so a partial install still shows something.
    SDL_FRect dst{at.x - kMarkerW / 2.0f, at.y - kMarkerH - 4.0f, kMarkerW, kMarkerH};
    const SDL_Color fill = r.kind == sim::RoverKind::Rover
                               ? SDL_Color{170, 90, 30, 255}     // rover: brown/orange
                               : SDL_Color{210, 225, 255, 220};  // ghost: pale blue-white
    SDL_SetRenderDrawColor(ren_, fill.r, fill.g, fill.b, fill.a);
    SDL_RenderFillRect(ren_, &dst);
    SDL_SetRenderDrawColor(ren_, 20, 20, 20, 255);
    SDL_RenderRect(ren_, &dst);  // outline so it reads against similar floor colours
}

// Campaign rover/ghost hazards (docs/re/campaign.md "Per-tick mover").
void Renderer::draw_rovers(const sim::State& s) {
    for (std::size_t i = 0; i < s.rovers.size(); ++i) draw_rover(s, i);
}

// Death animations (cosmetic, play once, advance at sim tick rate).
void Renderer::draw_death_fx(const sim::State& s) {
    for (std::size_t di = 0; di < deaths_.size();) {
        auto& fx = deaths_[di];
        const std::vector<Anim>& pool = assets_->deaths_for(fx.player % kLocalPlayers);
        const std::uint64_t step = s.tick - fx.start;
        const bool done = pool.empty() || step >= pool[fx.anim % pool.size()].steps.size();
        if (done) {
            deaths_.erase(deaths_.begin() + static_cast<std::ptrdiff_t>(di));
            continue;
        }
        draw_sprite(pool[fx.anim % pool.size()].steps[step], {fx.x, fx.y});
        ++di;
    }
}

// The gold twinkle post-pass (sub_420E39, after sub_420F07's per-player loop).
// Each particle ages once per REAL ENGINE FRAME, NOT per sim tick, which is why
// this half lives here and the spawn half does not. docs/render-notes.md §5.
void Renderer::draw_gold_sparkles() {
    const int lifetime = static_cast<int>(goldman_anim_.steps.size());
    for (auto& sp : gold_sparkles_) {
        if (!sp.active) continue;
        if (lifetime <= 0 || sp.age >= lifetime) {
            sp.active = false;
            continue;
        }
        draw_anim(goldman_anim_, static_cast<std::size_t>(sp.age), {sp.x, sp.y});
        ++sp.age;
    }
}

void Renderer::draw_world(const sim::State& s) {
    draw_burning_bricks(s);
    draw_flames(s);
    sample_movement(s);
    draw_players(s);
    draw_rovers(s);
    draw_death_fx(s);
    draw_gold_sparkles();
}

void Renderer::draw_hurry_banner(const sim::State& s) {
    // The sim's enclosure system reconciles the getvalue(101) window into a
    // single Hurry event on the edge (enclosure.cpp); on_events latches it here
    // for the flash duration and SoundDirector fires SFX 2700..2799 off the same
    // event — one source of truth, no duplicated threshold math.
    if (s.tick < hurry_until_ && ((s.tick >> 2) & 1) == 0)
        draw_anim(seqs_->hurry, 0, {kScreenW / 2.0f, kScreenH / 2.0f});
}

void Renderer::draw_clock(const sim::State& s) {
    // Match clock position — CONFIRMED VALUELST ids 110/111/112 (x/y of the
    // first digit and the extra per-digit spacing, docs/valuelst-map.md). Read
    // live so a modified VALUELST still repositions the HUD.
    const float x0 = static_cast<float>(values_ ? values_->column_or(110, 0, 525) : 525);
    const float y = static_cast<float>(values_ ? values_->column_or(111, 0, 36) : 36);
    const float spacing = static_cast<float>(values_ ? values_->column_or(112, 0, 4) : 4);

    if (untimed_) {
        // Untimed round (playtime_seconds == 1001, dword_4601A8 == 1001): the
        // KFONT "infinity" glyph in place of the digit string, same anchor.
        const Anim& inf = seqs_->infinity;
        if (!inf.steps.empty()) draw_sprite(inf.steps[0], {x0 + inf.steps[0].hx, y});
        return;
    }

    const Anim& d = seqs_->digits;
    if (d.steps.size() < 11) return;
    const int seconds_left = (s.ticks_left + sim::kTicksPerSecond - 1) / sim::kTicksPerSecond;
    // MESSAGES.TXT id 281 = "%u:%02u" (sub_4105D2 splits whole seconds by 60);
    // getstring falls back to the literal when the install's file lacks the id.
    const std::string text = format_clock(assets_->getstring(281, "%u:%02u"), seconds_left);
    // <=30 s remaining swaps the digit ink to a warning colour (byte_49D38F ->
    // byte_49A390, docs/re/in-match-shell.md §3 point 4). The exact palette
    // index is not resolvable without the original's LUT, so a clear red stands
    // in; layout and timing are the faithful part.
    const bool warn = clock_warning(seconds_left);
    const Tint ink{255, warn ? Uint8{40} : Uint8{255}, warn ? Uint8{40} : Uint8{255}};

    float x = x0;
    for (char ch : text) {
        const std::size_t idx = ch == ':' ? 10 : static_cast<std::size_t>(ch - '0');
        if (idx > 10) continue;  // ignore anything format_clock couldn't map to a glyph
        const Sprite& sp = d.steps[idx];
        draw_sprite(sp, {x + sp.hx, y}, ink);
        x += sp.w + spacing;  // VALUELST 112: extra spacing between digits
    }
}

void Renderer::draw_hud(const sim::State& s) {
    draw_hurry_banner(s);
    draw_clock(s);
}

// The pass order IS sub_42A191's per-frame call sequence, and tests/visual pins
// it. facts.md "Draw order"; docs/render-notes.md §4.
void Renderer::draw_frame(const sim::State& s, float alpha) {
    capture_interp(s);
    interp_alpha_ = alpha;
    SDL_SetRenderDrawColor(ren_, 0, 0, 0, 255);
    SDL_RenderClear(ren_);
    SDL_RenderTexture(ren_, assets_->field(), nullptr, nullptr);
    draw_cells(s);
    draw_actors(s);
    draw_bombs(s);
    draw_powerups(s);
    draw_world(s);
    draw_hud(s);
}

}  // namespace bomber::game
