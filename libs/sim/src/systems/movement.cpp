#include "systems/movement.hpp"

#include "grid.hpp"

namespace bomber::sim {

// Faithful port of sub_41EC84: each pixel step advances along the facing
// axis when free, glides one pixel toward the lane centreline when off-lane,
// rounds a corner when blocked head-on beside a clear perpendicular L, and
// settles back onto the tile centre when blocked past it. There is NO
// distance threshold — the assist is governed purely by which side of the
// tile centre the player is on, exactly like the original.
//
// on_center (if non-null) fires the instant a per-pixel step lands the player
// exactly on a tile centre — the port of the original's in-loop `v35 == -1`
// step-on check. The original tests this at the START of each pixel iteration
// (position at centre-1 about to become centre); measured post-step it is the
// same physical event (arrival at the centre pixel), and because steps are
// exactly ±1px along the axis, every centre crossing is caught. This fixes the
// warp/trampoline "stuck": a walking player's budget steps OVER the exact
// centre pixel, so a post-walk-only test almost never fired. See §5.
void MovementSystem::move(Player& p, Direction d, std::int32_t extra_budget, StepOnFn on_center,
                          void* ctx, bool use_player_speed) {
    State& s = s_;
    p.facing = d;

    // Unit vectors in the original's godir order: 0=Up, 1=Right, 2=Down, 3=Left.
    // (Our Direction enum orders differently, so map through godir explicitly —
    // the (dir±1)&3 rotations below depend on this specific ordering.)
    static constexpr int DX[4] = {0, 1, 0, -1};
    static constexpr int DY[4] = {-1, 0, 1, 0};
    auto godir = [](Direction dd) -> int {
        switch (dd) {
            case Direction::Up: return 0;
            case Direction::Right: return 1;
            case Direction::Down: return 2;
            case Direction::Left: return 3;
        }
        return 1;
    };
    auto passable = [&](int tx, int ty) {
        return grid::tile_open(s, tx, ty) && !grid::bomb_at(s, tx, ty);
    };

    const int g = godir(d);
    const int dxg = DX[g], dyg = DY[g];

    // Disease speed factors, applied in the original's order (sub_41F29B):
    // molasses divides by 3 first, then hyper/super multiplies by 3/2.
    // Only folded in when the player actually supplied the move (case (b) in
    // sub_41F29B); a conveyor forcing an idle player (case (a)) contributes
    // ONLY its own term — see the use_player_speed doc comment in the header.
    std::int32_t eff = 0;
    if (use_player_speed) {
        eff = p.speed;
        if (p.sick(Disease::Slow)) eff /= 3;
        if (p.sick(Disease::Fast) || p.sick(Disease::Super)) eff = 3 * eff / 2;
    }

    // The conveyor budget (extra_budget) is added AFTER the disease factors,
    // exactly as sub_41F29B adds its getvalue(190+idx) term after molasses/
    // hyper scaling — the belt is not slowed by disease. See stage-actors.md.
    p.move_budget += eff + extra_budget;
    while (p.move_budget > 0) {
        p.move_budget -= 100;

        const int px = p.x / kScale, py = p.y / kScale;
        const int tx = px / kTileW, ty = py / kTileH;
        const int sx = ((px % kTileW) + kTileW) % kTileW - kTileW / 2;  // offset from centre
        const int sy = ((py % kTileH) + kTileH) % kTileH - kTileH / 2;

        const int along = sx * dxg + sy * dyg;  // signed distance along the facing axis
        const int perp = sy * dxg - sx * dyg;   // signed offset from the lane centreline

        int mdx = 0, mdy = 0;
        if (along < 0 || passable(tx + dxg, ty + dyg)) {
            // Free to advance: step forward; if off-lane, also step one pixel
            // toward the centreline (a diagonal glide into the lane).
            mdx = dxg;
            mdy = dyg;
            if (perp != 0) {
                const int pd = perp < 0 ? (g + 1) & 3 : (g + 3) & 3;
                mdx += DX[pd];
                mdy += DY[pd];
            }
        } else if (perp < 0) {
            // Blocked ahead, leaning one way: round the corner if the L is clear.
            const int pd = (g + 3) & 3;
            if (passable(tx + DX[pd], ty + DY[pd]) &&
                passable(tx + DX[pd] + dxg, ty + DY[pd] + dyg)) {
                mdx = DX[pd];
                mdy = DY[pd];
            }
        } else if (perp > 0) {
            const int pd = (g + 1) & 3;
            if (passable(tx + DX[pd], ty + DY[pd]) &&
                passable(tx + DX[pd] + dxg, ty + DY[pd] + dyg)) {
                mdx = DX[pd];
                mdy = DY[pd];
            }
        } else if (along > 0) {
            // Blocked, centred on the lane, past the tile centre: settle back to it.
            const int opp = (g + 2) & 3;
            mdx = DX[opp] * along;
            mdy = DY[opp] * along;
        }

        p.x = (px + mdx) * kScale;
        p.y = (py + mdy) * kScale;

        // sub_41EC84 `v35 == -1`: fire the step-on the moment this pixel step
        // brings the player to the tile centre ALONG THE TRAVEL AXIS. The
        // original rotates the offset by the facing (v35 = along-axis offset)
        // and tests only that axis — the perpendicular (v36) is not required to
        // be centred — and looks the actor up at the player's CURRENT tile. So
        // match that: horizontal travel fires at the x-centre (any row),
        // vertical at the y-centre (any column). Whole-pixel positions make the
        // centre exactly representable (20 px in x, 18 px in y within a tile);
        // 1-px axis steps guarantee every centre crossing is caught. The callee
        // filters by actor type + latch, so crossings of ordinary tiles are
        // harmless no-ops.
        if (on_center) {
            const int nx = p.x / kScale, ny = p.y / kScale;
            const bool at_x_centre = ((nx % kTileW) + kTileW) % kTileW == kTileW / 2;
            const bool at_y_centre = ((ny % kTileH) + kTileH) % kTileH == kTileH / 2;
            if ((dxg != 0 && at_x_centre) || (dyg != 0 && at_y_centre))
                on_center(ctx, p, nx / kTileW, ny / kTileH);
        }
    }
}

}  // namespace bomber::sim
