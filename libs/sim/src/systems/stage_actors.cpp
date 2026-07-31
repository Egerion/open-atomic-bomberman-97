// Conveyor, trampoline and warphole player mechanics. Faithful port of the
// actor branches in sub_41F29B (conveyor move-budget) and sub_41EC84 (step-on
// trampoline / warphole). Dirarrows are bomb-only (bombs.cpp). See
// docs/re/stage-actors.md §3-5.

#include "systems/stage_actors.hpp"

#include "bomber/sim/rng.hpp"
#include "grid.hpp"

namespace bomber::sim {

namespace {

// The actor under a tile, or None if empty/out of grid. Mirrors sub_405654.
ActorType actor_at(const State& s, int tx, int ty) {
    if (!grid::in_grid(tx, ty)) return ActorType::None;
    return s.actor_type[ty][tx];
}

// The apex-relocation accept test, as the two probes the original uses.
// sub_425FB9 reads the collision grid dword_46222C (0 = floor) and returns 1 out
// of bounds, so !sub_425FB9 is exactly grid::tile_open; sub_422E48 finds a
// grounded bomb (sub-mode +46 not 2 or 3), which grid::bomb_at already matches.
bool tramp_landable(const State& s, int tx, int ty) {
    return grid::tile_open(s, tx, ty) && grid::bomb_at(s, tx, ty) == nullptr;
}

}  // namespace

void StageActorSystem::tick_bounce(Player& p) {
    if (p.bounce <= 0) return;

    // Mirror sub_41F29B state 5 (raw disasm 0x420280..0x42053f). The original
    // holds an UP counter c (player word +80) advanced once per tick; the bounce
    // ends at c == getvalue(680) (=30) and the apex is c == getvalue(680)/2 (=15).
    // Our Player::bounce is the equivalent DOWN countdown (starts at 30), so the
    // elapsed frame count is c = trampoline_bounce_frames - bounce. We decrement
    // first (matching the original's leading ++c), then act on the new c.
    const std::int32_t len = s_.tuning.trampoline_bounce_frames;
    --p.bounce;
    const std::int32_t c = len - p.bounce;  // 1..len this tick

    // At the apex the player is teleported to a RANDOM nearby open tile — the
    // "fly" the trampoline is named for, NOT an in-place bounce. Byte-for-byte
    // port of the 0x4203a7 loop: up to 100 attempts, each drawing rand()%5 TWICE
    // (both ALWAYS, before any test) to offset the current tile by [-2,+2] per
    // axis, accepting the first candidate that differs from the origin on BOTH
    // axes and is landable.
    if (c != len / 2) return;
    const int cx = p.tile_x(), cy = p.tile_y();
    for (int m = 0; m < 100; ++m) {
        const int nx = cx + static_cast<int>(random_below(s_, 5)) - 2;  // FIRST draw
        const int ny = cy + static_cast<int>(random_below(s_, 5)) - 2;  // SECOND draw
        if (nx == cx || ny == cy || !tramp_landable(s_, nx, ny)) continue;
        p.x = grid::tile_center_x(nx);
        p.y = grid::tile_center_y(ny);
        break;
    }
    // The extra `++c` at 0x4204af, right after the relocation loop, is ALSO a
    // frame of the flight, not just the re-fire guard an earlier note dismissed
    // it as: the counter skips 15 outright (…14, 16, …), so the original reaches
    // getvalue(680) one tick sooner and the hop is 29 counter steps of wall
    // clock, not 30. Dropping it made our hop a tick longer than the original's.
    --p.bounce;
}

bool StageActorSystem::start_warp(Player& p, int player_index, int tx, int ty) {
    if (p.warp > 0) return false;  // already warping
    if (actor_at(s_, tx, ty) != ActorType::Warphole) return false;

    // START the two-phase warp (sub_41EC84 sets state 6 and STORES the
    // destination in +20/+24). NO RNG: the exit was resolved at setup by the
    // idno/linkto scan sub_405A81. Capture the dest NOW — the walk that triggered
    // this can slide the player off the warphole later in the tick, so tick_warp
    // must use the stored dest and not a midpoint lookup. A warphole with no
    // partner has dest == its own tile, making the warp a harmless in-place hop.
    //
    // NO re-entry latch, and the original has none either: the guard against
    // warping straight back out of the exit is GEOMETRIC. tick_warp drops the
    // player exactly on the destination tile centre, so its along-axis offset
    // there is 0, and the trigger needs -1 — reachable only by approaching a
    // centre from outside. The `warp_latch` this port carried existed solely to
    // protect the post-tick "safety net" trigger, removed with it; the actor's own
    // +146 byte is the one-shot LOAD-TIME knockout latch, a different thing.
    p.warp = kWarpTicks;
    p.warp_to_x = s_.warp_dest_x[ty][tx];
    p.warp_to_y = s_.warp_dest_y[ty][tx];
    s_.events.push_back({Event::Type::WarpUsed, static_cast<std::int8_t>(player_index),
                         static_cast<std::int8_t>(p.warp_to_x),
                         static_cast<std::int8_t>(p.warp_to_y), 0});
    return true;
}

bool StageActorSystem::start_bounce(Player& p, int player_index, int tx, int ty) {
    if (p.bounce > 0) return false;  // already bouncing
    if (actor_at(s_, tx, ty) != ActorType::Trampoline) return false;

    // No latch, same geometric argument as start_warp: the apex always relocates
    // the player to a tile centre, and one differing from the trampoline's on
    // BOTH axes, where the along offset is 0 — so the -1 trigger cannot re-fire
    // without the player walking in again. The `tramp_latch` this port carried
    // was a consequence of the removed post-tick trigger, which on level 9 (eight
    // trampolines) could land a player on a SECOND trampoline's centre and
    // re-launch them, something the original cannot do. The actor's own +48
    // "triggered" word is the bounce ANIMATION flag, not a re-entry guard.
    p.bounce = s_.tuning.trampoline_bounce_frames;
    s_.events.push_back({Event::Type::TrampolineBounce,
                         static_cast<std::int8_t>(player_index),
                         static_cast<std::int8_t>(tx), static_cast<std::int8_t>(ty), 0});
    return true;
}

// Warphole first, then trampoline. The original's -1 block tests
// `actor.type == 1` and `actor.type == 3` as two independent ifs on the SAME
// lookup, in that order; a tile carries at most one actor, so sequencing them is
// equivalent, and neither carries an extra condition.
void StageActorSystem::on_step_center(void* ctx, Player& p, int tx, int ty) {
    auto* c = static_cast<StepOnCtx*>(ctx);
    if (!c->self->start_warp(p, c->player_index, tx, ty))
        c->self->start_bounce(p, c->player_index, tx, ty);
}

bool StageActorSystem::move_on_actor(Player& p, int want_godir, bool moving,
                                     std::int32_t delta_ms, MovementSystem::PixelFn on_pixel,
                                     void* pixel_ctx) {
    const Fixed bx = p.x, by = p.y;
    const int tx = p.tile_x(), ty = p.tile_y();
    const ActorType act = actor_at(s_, tx, ty);
    const bool conveyor = (act == ActorType::Conveyor);
    const int belt_dir = conveyor ? s_.actor_dir[ty][tx] : -1;
    const std::int32_t belt = s_.tuning.conveyor_speed();

    // The player index is recovered from the array offset — players live in a
    // contiguous std::array in State.
    StepOnCtx sctx{this, static_cast<int>(&p - &s_.players[0])};

    // Dirarrows (type 0) do NOT steer walking players: sub_41F29B calls
    // sub_405654 only for warpholes (~23354) and conveyors (~23417/23443), with
    // no type-0 branch. They re-steer SLIDING BOMBS only (bombs.cpp).
    MovementSystem::MoveRequest req;
    req.delta_ms = delta_ms;
    req.on_center = &on_step_center;
    req.center_ctx = &sctx;
    req.on_pixel = on_pixel;
    req.pixel_ctx = pixel_ctx;

    if (moving) {
        // Case (b) — the player has an input direction. The belt only nudges the
        // budget; it never overrides the chosen direction. Perpendicular to the
        // belt, nothing changes.
        req.dir = grid::from_godir(want_godir);
        if (conveyor && want_godir == belt_dir) req.extra_budget = belt;
        if (conveyor && want_godir == ((belt_dir + 2) & 3)) req.extra_budget = -belt;
        movement_.move(p, req);
        return p.x != bx || p.y != by;
    }
    if (!conveyor) return false;

    // Case (a) — no input. The belt FORCES its direction and pushes. If the step
    // is fully blocked the original reverts godir to -1 (no facing change, no
    // visible push); our stepper simply produces no shift. The budget is EXACTLY
    // conveyor_speed: sub_41F29B's case (a) never reads the player's own speed
    // (docs/re/stage-actors.md §3).
    req.dir = grid::from_godir(belt_dir);
    req.extra_budget = belt;
    req.use_player_speed = false;
    const Direction saved_facing = p.facing;
    movement_.move(p, req);
    // stage_actors.md finding 1 (sub_41F29B 779-796): after a non-fatal
    // belt-forced push the original ALWAYS reverts both the requested dir (+46)
    // and the facing (+44) to the pre-push value. The revert is gated on
    // sub_41EC84 reporting no kill, which holds on every tick except the rare one
    // where a flamed belt kills the player mid-step — and simulation.cpp returns
    // on !p.alive before facing is read again. So an idle player parked on a belt
    // keeps FACING whatever direction they last actively chose, not the belt
    // direction, which also drives their punch/kick direction if they act with no
    // directional input. Reverting only on zero net displacement left facing ==
    // belt_dir whenever the belt actually moved them; facing is hashed -> golden.
    p.facing = saved_facing;
    return p.x != bx || p.y != by;
}

void StageActorSystem::tick_warp(Player& p) const {
    if (p.warp <= 0) return;
    --p.warp;
    // At the warp-out→warp-in boundary the original relocates the player to the
    // pending exit (state 6 sets +28/+32 = the stored dest +20/+24). We captured
    // that dest at step-on into warp_to_*, so the relocation is exact even if the
    // trigger tick's walk slid the player off the warphole. No RNG.
    if (p.warp == kWarpMid) {
        if (grid::in_grid(p.warp_to_x, p.warp_to_y)) {
            p.x = grid::tile_center_x(p.warp_to_x);
            p.y = grid::tile_center_y(p.warp_to_y);
        }
    }
}

}  // namespace bomber::sim
