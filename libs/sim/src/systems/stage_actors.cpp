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

// The trampoline "solid" test, sub_425FB9(x,y): reads the collision grid
// dword_46222C (filled from the board tile type; 0 = floor, non-zero = wall/
// brick) and returns 1 (blocked) out of bounds. So !sub_425FB9 == in-grid AND a
// walkable floor tile — exactly grid::tile_open (Blank, not burning). Confirmed
// by disasm (sub_425FB9 @0x425FB9, grid fill @sub_425E36 callers ~0x4260F5).
bool tramp_solid(const State& s, int tx, int ty) { return !grid::tile_open(s, tx, ty); }

// The trampoline "bomb" test, sub_422E48(x,y): scans the bomb registry for a
// grounded bomb on (x,y) whose sub-mode (+46) is not 2 or 3 (airborne/thrown).
// Maps to grid::bomb_at, which already excludes flying bombs. Confirmed by
// disasm (sub_422E48 @0x422E48).
bool tramp_bomb(const State& s, int tx, int ty) { return grid::bomb_at(s, tx, ty) != nullptr; }

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
    // "fly" the trampoline is named for (NOT an in-place bounce). Byte-for-byte
    // port of the 0x4203a7 loop: up to 100 attempts, each draws rand()%5 TWICE
    // (both ALWAYS, before any test), offsets the current tile by [-2,+2] on each
    // axis, and accepts the first candidate that differs from the origin on BOTH
    // axes and is neither solid (!sub_425FB9) nor occupied by a bomb
    // (!sub_422E48). No trampolines on a board ⇒ never reached ⇒ zero RNG draws
    // ⇒ the golden RNG stream is untouched.
    if (c == len / 2) {
        const int cx = p.tile_x(), cy = p.tile_y();
        for (int m = 0; m < 100; ++m) {
            const int nx = cx + static_cast<int>(random_below(s_, 5)) - 2;  // FIRST draw
            const int ny = cy + static_cast<int>(random_below(s_, 5)) - 2;  // SECOND draw
            if (nx != cx && ny != cy && !tramp_solid(s_, nx, ny) && !tramp_bomb(s_, nx, ny)) {
                p.x = grid::tile_center_x(nx);
                p.y = grid::tile_center_y(ny);
                break;
            }
        }
        // The extra `++c` at 0x4204af, right after the relocation loop. A prior
        // note dismissed it as a re-fire guard our down-counter does not need —
        // true, but it is ALSO a frame of the flight: the counter skips 15
        // outright (…14, 16, …), so the original reaches getvalue(680) one tick
        // sooner and the whole hop is 29 counter steps of wall clock, not 30.
        // Dropping it made our hop a tick longer than the original's.
        --p.bounce;
    }
}

bool StageActorSystem::start_warp(Player& p, int player_index, int tx, int ty) {
    if (p.warp > 0) return false;  // already warping
    if (actor_at(s_, tx, ty) != ActorType::Warphole) return false;

    // START the two-phase warp (sub_41EC84 sets player state 6 and STORES the
    // destination in +20/+24). NO RNG: the exit was resolved at setup by the
    // idno/linkto scan (sub_405A81). Capture the dest tile NOW — the walk that
    // triggered this can slide the player off the warphole later this tick, so
    // tick_warp must relocate to the stored dest, not a midpoint tile lookup.
    // A warphole with no partner has dest == its own tile, so the warp is a
    // harmless in-place hop. Sound 1330 fires here.
    //
    // NO re-entry latch, and the original has none either: the guard against
    // warping straight back out of the exit is GEOMETRIC. tick_warp drops the
    // player exactly on the destination tile centre (sub_426524/sub_42655F), so
    // its along-axis offset there is 0, and the trigger needs -1 — which is only
    // reachable by approaching a centre from outside it. The `warp_latch` this
    // port used to carry existed solely to protect the post-tick "safety net"
    // trigger (removed with it); the actor's own +146 byte is the one-shot
    // LOAD-TIME knockout latch, a different thing, and lives in match_factory.
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

    // Bounce length = VALUELST id 680 (=30), the bounce-state frame count read
    // by sub_41F29B (~23160). A confirmed VALUELST value, not a guess. See §4.
    //
    // No latch, same argument as start_warp: the apex always relocates the
    // player to a tile centre (and one that differs from the trampoline's on
    // BOTH axes), where the along offset is 0, so the -1 trigger cannot re-fire
    // without the player walking in again. The `tramp_latch` this port used to
    // carry was a consequence of the removed post-tick trigger — and on level 9
    // (eight trampolines) that trigger could land a player on a SECOND
    // trampoline's centre and re-launch them off it, which the original cannot
    // do. The actor's own +48 "triggered" word is the bounce ANIMATION flag, not
    // a re-entry guard; the renderer derives it from Player::bounce.
    p.bounce = s_.tuning.trampoline_bounce_frames;
    s_.events.push_back({Event::Type::TrampolineBounce,
                         static_cast<std::int8_t>(player_index),
                         static_cast<std::int8_t>(tx), static_cast<std::int8_t>(ty), 0});
    return true;
}

// Movement step-on trampoline for MovementSystem: fires the warphole/trampoline
// trigger the instant a per-pixel step centres the player on a tile — the port
// of sub_41EC84's in-loop check for an offset-to-tile-centre of -1. ctx is a
// StepOnCtx.
void StageActorSystem::on_step_center(void* ctx, Player& p, int tx, int ty) {
    auto* c = static_cast<StepOnCtx*>(ctx);
    // Warphole first, then trampoline. The original's -1 block tests
    // `actor.type == 1` and `actor.type == 3` as two independent ifs on the SAME
    // lookup, in that order; a tile carries at most one actor, so sequencing
    // them is equivalent. The two are gated identically — there is no extra
    // condition on either — so a change to this hook is a change to both.
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

    // The per-pixel stepper fires the warphole/trampoline step-on mid-walk
    // (sub_41EC84's offset-to-centre == -1 check), so a player WALKING onto a
    // warp triggers it — the
    // old post-walk-only test almost never landed on the exact centre pixel and
    // left the player unable to warp ("stuck"). Player index is recovered from
    // the array offset (players live in a contiguous std::array in State).
    StepOnCtx sctx{this, static_cast<int>(&p - &s_.players[0])};

    // NOTE: dirarrows (type 0) do NOT steer walking players. The player mover
    // sub_41F29B calls sub_405654 only for warpholes (~23354) and conveyors
    // (~23417/23443) — there is no type-0 branch. Dirarrows re-steer SLIDING
    // BOMBS only (bombs.cpp / sub_42331C ~25532). See docs/re/stage-actors.md §5.
    if (moving) {
        // Case (b) — the player has an input direction. The belt only nudges
        // the budget; it never overrides the chosen direction.
        std::int32_t extra = 0;
        if (conveyor) {
            if (want_godir == belt_dir)
                extra += belt;  // moving WITH the belt: speed bonus
            else if (want_godir == ((belt_dir + 2) & 3))
                extra -= belt;  // moving AGAINST the belt: speed penalty
            // perpendicular: no change
        }
        movement_.move(p, grid::from_godir(want_godir), extra, &on_step_center, &sctx,
                       /*use_player_speed=*/true, on_pixel, pixel_ctx, delta_ms);
    } else if (conveyor) {
        // Case (a) — no input. The belt FORCES its direction and pushes. If the
        // step is fully blocked (wall ahead) the original reverts godir to -1
        // (no facing change, no visible push); our stepper simply produces no
        // shift, and we leave facing untouched to match that revert. The budget
        // is EXACTLY conveyor_speed here — sub_41F29B's case (a) never reads the
        // player's own speed (that only happens in case (b), on player input),
        // so use_player_speed=false (docs/re/stage-actors.md §3).
        const Direction saved_facing = p.facing;
        movement_.move(p, grid::from_godir(belt_dir), belt, &on_step_center, &sctx,
                       /*use_player_speed=*/false, on_pixel, pixel_ctx, delta_ms);
        // stage_actors.md finding 1 (sub_41F29B 779-796, batch_0x41F29B.cpp:
        // 789-795): after a non-fatal belt-forced push the original ALWAYS
        // reverts both the requested dir (+46) and the facing (+44) to the
        // pre-push value — the revert is gated on sub_41EC84 reporting no kill,
        // which holds on every tick except the
        // rare one a flamed belt kills the player mid-step (and simulation.cpp
        // returns on !p.alive before facing is read again). So an idle player
        // parked on a belt keeps FACING (and posing walking in) whatever
        // direction they last actively chose, not the belt direction — which
        // also drives their punch/kick direction if they act with no
        // directional input that tick. The prior port only reverted on zero
        // net displacement (the blocked case), leaving facing == belt_dir
        // whenever the belt actually moved them. facing is hashed -> golden.
        p.facing = saved_facing;
    }
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
