#include "systems/movement.hpp"

#include <algorithm>

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
                          void* ctx, bool use_player_speed, PixelFn on_pixel, void* pixel_ctx,
                          std::int32_t delta_ms) {
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

    // Per-frame budget accrual (sub_41F29B 23432-23440): the disease factors
    // scale the SPEED first — molasses divides by 3, then hyper/super
    // multiplies by 3/2 — and only THEN the delta division runs (`v91 =
    // base + skates·gv(90) − clogs·gv(91); if (molasses) v91 /= 3;
    // if (hyper/super) v91 = 3*v91/2; v91 = delta*v91/50`). Resolves the
    // former facts.md [VERIFY] on this ordering (2026-07-16 movement audit):
    // factors FIRST, delta scaling SECOND — identical at delta 50, ±1 budget
    // unit per sub-frame versus the old scale-after order, diseased players
    // only. Only folded in when the player actually supplied the move (case
    // (b) in sub_41F29B); a conveyor forcing an idle player (case (a))
    // contributes ONLY its own term — see the use_player_speed doc comment
    // in the header.
    std::int32_t eff = 0;
    if (use_player_speed) {
        std::int32_t sp = p.speed;
        if (p.sick(Disease::Slow)) sp /= 3;
        if (p.sick(Disease::Fast) || p.sick(Disease::Super)) sp = 3 * sp / 2;
        eff = frame_budget(sp, delta_ms);
    }

    // The conveyor budget (extra_budget) is added AFTER the disease factors,
    // exactly as sub_41F29B adds its getvalue(190+idx) term after molasses/
    // hyper scaling — the belt is not slowed by disease, but it IS delta-
    // scaled like every per-frame accrual. See stage-actors.md.
    p.move_budget += eff + frame_budget(extra_budget, delta_ms);
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

        // sub_41EC84's post-commit tail (pseudo.c 22699-22717): flame death,
        // then pickup, at the tile of the CURRENT pixel position — every
        // iteration, even a blocked one (mdx/mdy zero re-checks the same tile,
        // a harmless no-op). A kill aborts the walk: the original returns 1
        // mid-loop and the rest of the budget dies with the player.
        if (on_pixel && on_pixel(pixel_ctx, p)) return;
    }
}

int MovementSystem::ice_delay(Player& p, int want_godir) const {
    // AI is exempt: sub_41F29B gates the whole buffer push+resolve block on
    // the player-type byte (+16 != 1, i.e. NOT a computer player) — an AI's
    // desired direction reaches the mover unlagged even on Hockey Rink.
    if (p.ai) return want_godir;

    const int level = std::clamp(s_.tuning.level_index, 0, 10);
    const int delay_ms = s_.tuning.ice_delay_ms[level];
    if (delay_ms <= 0) return want_godir;  // inert off Hockey Rink; buffer left untouched

    // Age + shift + insert (sub_41F29B ~23060-23077): the original ages every
    // slot by the frame delta (dword_464958), shifts the buffer down one, and
    // inserts the fresh sample at slot 0 — once per DISPLAYED frame. This is
    // called once per canonical sub-frame (player_turn's kSubFrames loop), so
    // it stays a plain FIFO push: slot k's age is the sum of k sub-frame deltas
    // (~5.56 ms each at kSubFrames=9), so the 30-slot buffer spans only ~167 ms.
    // FRAMERATE-COUPLED, BY DESIGN DECISION (2026-07-22): the original's ice lag
    // is nominally getvalue(450+level) = 250 ms for Hockey Rink, but it too is
    // 30 frames of buffer, so at the canonical ~184 fps free-run (ADR-0006, the
    // rate the port targets via kSubFrames=9) it CAPS at slot 29 ≈ 161 ms on
    // BOTH sides — the port matches the native at that rate. (On slower/vsync-
    // locked native hardware 30 frames span 500-1000 ms and the full 250 ms is
    // delivered; the port deliberately does NOT chase that — it pins the ~184
    // fps behaviour. To honour the nominal 250 ms instead you'd grow
    // kIceHistoryLen to >= 46, a hashed-state + golden change, explicitly NOT
    // done.) facts.md "Ice / input-lag".
    for (int k = Player::kIceHistoryLen - 1; k > 0; --k) p.ice_history[k] = p.ice_history[k - 1];
    p.ice_history[0] = static_cast<std::int8_t>(want_godir);

    // Resolve (sub_41F29B ~23071-23077): walk from the freshest sample toward
    // the oldest, using the first whose age has reached delay_ms — i.e. the
    // smallest k with k*(50/3) >= delay_ms (the original's measured ages
    // jitter ±1 ms around the same 60 fps train), clamped to the buffer's own
    // capacity (the original's behaviour when a delay exceeds its 30-slot
    // history: the loop runs out and the LAST (oldest) sample it read stays
    // in effect).
    int k = (delay_ms * kSubFrames + kMsPerTick - 1) / kMsPerTick;
    if (k >= Player::kIceHistoryLen) k = Player::kIceHistoryLen - 1;
    return p.ice_history[k];
}

}  // namespace bomber::sim
