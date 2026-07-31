#include "systems/movement.hpp"

#include <algorithm>

#include "grid.hpp"

namespace bomber::sim {

// Faithful port of sub_41EC84: each pixel step advances along the facing axis
// when free, glides one pixel toward the lane centreline when off-lane, rounds a
// corner when blocked head-on beside a clear perpendicular L, and settles back
// onto the tile centre when blocked past it. There is NO distance threshold —
// the assist is governed purely by which side of the tile centre the player is
// on, exactly like the original.
//
// req.on_center is sub_41EC84's in-loop step-on test EXACTLY: at the TOP of each
// pixel iteration the original rotates the two offset-to-tile-centre values
// (sub_426599 for x, sub_4265EB for y) by the requested direction and, when the
// resulting ALONG component reads exactly -1, looks the stage actor up at the
// player's CURRENT tile. Three properties fall out of that literal reading and
// all three matter:
//
//   * -1, not 0 — the trigger is the APPROACH to the centre, one pixel short,
//     and from -1 the step is always +1 along the axis (the "free to advance"
//     branch short-circuits on `along < 0`). So the only way to hit it is to walk
//     INTO the centre from outside: a player already parked on the centre (along
//     == 0), one shoved back onto it by the blocked settle-back (along > 0 -> 0),
//     and one whose step is fully blocked all read something else and are left
//     alone. Testing the POST-step position for "landed on the centre" — what
//     this used to do — collapses those three cases into a trigger, and is why
//     the port grabbed players the original does not.
//   * only the travel axis — the perpendicular component is never consulted, so
//     an off-lane walker still triggers on its own row/column, and a player
//     crossing the NEIGHBOURING row/column cannot, because the actor lookup uses
//     the player's own tile.
//   * pre-step tile — the actor is read where the offsets were taken from.
//
// Because every axis step is exactly ±1px, every genuine centre approach is
// caught, so a walking player still warps mid-walk. (The original "stuck" fix: a
// 9px/tick stride steps OVER the centre, so a post-walk-only test at the end of
// the tick almost never fired.) See docs/re/stage-actors.md §5.
void MovementSystem::move(Player& p, const MoveRequest& req) {
    State& s = s_;
    p.facing = req.dir;

    const auto passable = [&](int tx, int ty) {
        return grid::tile_open(s, tx, ty) && !grid::bomb_at(s, tx, ty);
    };

    // grid::kDx/kDy are in the original's godir order; our Direction enum orders
    // differently, so map through to_godir explicitly. The (g±1)&3 rotations
    // below depend on that exact ordering.
    const int g = grid::to_godir(req.dir);
    const int dxg = grid::kDx[g], dyg = grid::kDy[g];

    // Per-frame budget accrual (sub_41F29B 23432-23440). The disease factors
    // scale the SPEED first — molasses divides by 3, then hyper/super multiplies
    // by 3/2 — and only THEN the delta division runs. The original's single speed
    // temporary is seeded with base + skates·gv(90) − clogs·gv(91), divided by 3
    // if molasses, replaced by 3/2 of itself if hyper or super, and finally
    // rescaled to delta·speed/50. That ordering was a facts.md [VERIFY] until the
    // 2026-07-16 movement audit resolved it: factors FIRST, delta scaling SECOND
    // — identical at delta 50, ±1 budget unit per sub-frame otherwise, diseased
    // players only.
    std::int32_t eff = 0;
    if (req.use_player_speed) {
        std::int32_t sp = p.speed;
        if (p.sick(Disease::Slow)) sp /= 3;
        if (p.sick(Disease::Fast) || p.sick(Disease::Super)) sp = 3 * sp / 2;
        eff = frame_budget(sp, req.delta_ms);
    }
    // The belt term is added AFTER the disease factors, exactly as sub_41F29B
    // adds its getvalue(190+idx) term: the belt is not slowed by disease, but it
    // IS delta-scaled like every per-frame accrual.
    p.move_budget += eff + frame_budget(req.extra_budget, req.delta_ms);

    while (p.move_budget > 0) {
        p.move_budget -= 100;

        const int px = p.x / kScale, py = p.y / kScale;
        const int tx = px / kTileW, ty = py / kTileH;
        const int sx = ((px % kTileW) + kTileW) % kTileW - kTileW / 2;  // offset from centre
        const int sy = ((py % kTileH) + kTileH) % kTileH - kTileH / 2;

        const int along = sx * dxg + sy * dyg;  // signed distance along the facing axis
        const int perp = sy * dxg - sx * dyg;   // signed offset from the lane centreline

        // The step-on test, pre-step, at the player's CURRENT tile — see the
        // header comment. The callee filters by actor type, so crossings of
        // ordinary tiles are no-ops.
        if (req.on_center && along == -1) req.on_center(req.center_ctx, p, tx, ty);

        int mdx = 0, mdy = 0;
        if (along < 0 || passable(tx + dxg, ty + dyg)) {
            // Free to advance; if off-lane, also step one pixel toward the
            // centreline (a diagonal glide into the lane).
            mdx = dxg;
            mdy = dyg;
            if (perp != 0) {
                const int pd = (g + (perp < 0 ? 1 : 3)) & 3;
                mdx += grid::kDx[pd];
                mdy += grid::kDy[pd];
            }
        } else if (perp != 0) {
            // Blocked ahead, leaning one way: round the corner if the L is clear.
            // The rotation is the MIRROR of the glide above (3 where the glide
            // takes 1), which is why the two cannot share one expression.
            const int pd = (g + (perp < 0 ? 3 : 1)) & 3;
            if (passable(tx + grid::kDx[pd], ty + grid::kDy[pd]) &&
                passable(tx + grid::kDx[pd] + dxg, ty + grid::kDy[pd] + dyg)) {
                mdx = grid::kDx[pd];
                mdy = grid::kDy[pd];
            }
        } else if (along > 0) {
            // Blocked, centred on the lane, past the tile centre: settle back.
            const int opp = (g + 2) & 3;
            mdx = grid::kDx[opp] * along;
            mdy = grid::kDy[opp] * along;
        }

        p.x = (px + mdx) * kScale;
        p.y = (py + mdy) * kScale;

        // The post-commit tail (pseudo.c 22699-22717) runs EVERY iteration, even
        // a blocked one (a zero shift re-checks the same tile, a harmless no-op).
        // A kill aborts the walk: the original returns 1 mid-loop and the rest of
        // the budget dies with the player.
        if (req.on_pixel && req.on_pixel(req.pixel_ctx, p)) return;
    }
}

int MovementSystem::ice_delay(Player& p, int want_godir) const {
    // AI is exempt: sub_41F29B gates the whole buffer push+resolve block on the
    // player-type byte (+16 != 1), so an AI's desired direction reaches the mover
    // unlagged even on Hockey Rink.
    if (p.ai) return want_godir;

    const int level = std::clamp(s_.tuning.level_index, 0, 10);
    const int delay_ms = s_.tuning.ice_delay_ms[level];
    if (delay_ms <= 0) return want_godir;  // inert off Hockey Rink; buffer untouched

    // Age + shift + insert (sub_41F29B ~23060-23077): the original ages every
    // slot by the frame delta, shifts down one and inserts the fresh sample at
    // slot 0, once per DISPLAYED frame. This is called once per canonical
    // sub-frame, so it stays a plain FIFO push and slot k's age is k sub-frame
    // deltas (~5.56 ms each at kSubFrames = 9) — the 30-slot buffer spans ~167 ms.
    //
    // FRAMERATE-COUPLED BY DECISION (2026-07-22): the nominal lag is
    // getvalue(450+level) = 250 ms for Hockey Rink, but the original's buffer is
    // also 30 FRAMES, so at the canonical ~184 fps free-run this port targets it
    // caps at slot 29 ≈ 161 ms on BOTH sides — the port matches the native at
    // that rate. On slower or vsync-locked native hardware 30 frames span
    // 500-1000 ms and the full 250 ms is delivered; the port deliberately does
    // NOT chase that. Honouring the nominal 250 ms would mean growing
    // kIceHistoryLen to >= 46, a hashed-state and golden change, explicitly not
    // done. facts.md "Ice / input-lag".
    for (int k = Player::kIceHistoryLen - 1; k > 0; --k) p.ice_history[k] = p.ice_history[k - 1];
    p.ice_history[0] = static_cast<std::int8_t>(want_godir);

    // Resolve (~23071-23077): walk from the freshest sample toward the oldest and
    // use the first whose age has reached delay_ms, clamped to the buffer's own
    // capacity — the original's behaviour when a delay exceeds its 30-slot
    // history is that the loop runs out and the LAST sample it read stays in
    // effect.
    int k = (delay_ms * kSubFrames + kMsPerTick - 1) / kMsPerTick;
    if (k >= Player::kIceHistoryLen) k = Player::kIceHistoryLen - 1;
    return p.ice_history[k];
}

}  // namespace bomber::sim
