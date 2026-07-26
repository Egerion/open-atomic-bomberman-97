// AI danger/obstacle grids and the tile predicates that read them (sub_424D37
// danger reader, sub_409083 obstacle grid, plus safe_tile / drop_tile_clear /
// out_of_bomb_slots). Split out of ai.cpp as a pure file-split (no behaviour
// change); see ai.hpp for the subsystem API and docs/re/ai.md for the RE facts.

#include "systems/ai.hpp"

#include "bomber/sim/simulation.hpp"  // enclose_pos / enclose_total for the wall look-ahead
#include "grid.hpp"
#include "systems/ai_internal.hpp"  // kDX / kDY godir vectors

namespace bomber::sim {

// ---------------------------------------------------------------------------
// Per-tick grids (sub_424D37 danger reader, sub_409083 obstacle grid).
// Rebuilt from hashed State once per tick and shared by every AI player, as in
// the original (a sim-global grid, not per-AI). NOT hashed — pure functions of
// State, like s.events. See docs/re/ai.md §4/§5.
// ---------------------------------------------------------------------------
void AISystem::ensure_grids() {
    if (grids_valid_ && grids_tick_ == s_.tick) return;
    grids_tick_ = s_.tick;
    grids_valid_ = true;

    // --- Obstacle grid (sub_409083): 1 where a tile is solid/brick(/burning) or
    // has a grounded bomb, else 0. Airborne bombs occupy no tile. ---
    for (int y = 0; y < kGridHeight; ++y) {
        for (int x = 0; x < kGridWidth; ++x) {
            obstacle_[y][x] = grid::tile_open(s_, x, y) ? 0 : 1;
            danger_[y][x] = 0;
        }
    }
    for (const auto& b : s_.bombs) {
        if (b.active && !b.flying) obstacle_[b.tile_y()][b.tile_x()] = 1;
    }

    // --- Danger grid (sub_424D37 / dword_4621F4). Writer keeps the strongest
    // threat: grid = max(grid, v) (sub_424DFE). Three sources: ---
    auto raise = [&](int x, int y, std::int32_t v) {
        if (grid::in_grid(x, y) && v > danger_[y][x]) danger_[y][x] = v;
    };

    // 1) Active flame -> a flat 1000 at every lit cell (the flame updater
    //    sub_426d06 writes 1000, docs/re/ai.md §4.1). Maximally dangerous.
    for (int y = 0; y < kGridHeight; ++y)
        for (int x = 0; x < kGridWidth; ++x)
            if (s_.flame[y][x] > 0) raise(x, y, 1000);

    // 2) Live bombs (predicted blast). The bomb updater (sub_42331C 25683-
    //    25705) writes v = (bomb+68) + 100 at the bomb tile and propagates the
    //    SAME v along the 4 rays out to the bomb's flame length, stopping at a
    //    wall/brick (sub_425FB9) or another bomb (sub_422E48), one tile past a
    //    floor powerup (sub_42542D). Three facts CORRECTED by the 2026-07-12
    //    danger-map audit (facts.md "AI danger map"):
    //      - +68 is the elapsed fuse phase in MILLISECONDS (it accrues the
    //        per-frame ms delta), so v ranges 100..~2100 over a 2 s fuse —
    //        cross-source ordering against the closing walls (110..250) and
    //        flame (1000) depends on that scale; tick counts must be
    //        ms-scaled. A waiting trigger bomb accrues without bound; our
    //        image has no age counter, so it ranks at a full fuse's worth
    //        (past the point it outranks flame, as an aged trigger bomb does
    //        in the original — documented approximation).
    //      - FLYING bombs stamp too: the tail stamp's only gates are the
    //        active flag and the carried-pass parity — motion is never
    //        tested, so a punched/thrown bomb casts its blast from its
    //        instantaneous arc tile every frame (its +68 is frozen mid-air,
    //        matching our paused fuse). The old `|| b.flying` skip made AIs
    //        stand calmly under a sailing bomb.
    //      - The per-bomb duration word +74 (our fuse_init), not the global
    //        tuning value, anchors the elapsed computation.
    auto stamp_blast = [&](int bx, int by, int reach, std::int32_t v) {
        raise(bx, by, v);
        for (int d = 0; d < 4; ++d) {
            for (int i = 1; i <= reach; ++i) {
                const int rx = bx + kDX[d] * i, ry = by + kDY[d] * i;
                if (!grid::in_grid(rx, ry)) break;
                // Stop AT a wall/brick without marking it (flame won't reach a
                // solid; a brick blocks propagation). Matches sub_425FB9 != 0.
                if (s_.cells[ry][rx] != Cell::Blank || s_.burning[ry][rx] > 0) break;
                // Stop AT another grounded bomb (sub_422E48) without marking past.
                if (grid::bomb_at(s_, rx, ry) != nullptr) break;
                raise(rx, ry, v);
                // One tile PAST a floor powerup: mark it, then stop.
                if (s_.floor[ry][rx] != PowerupType::None) break;
            }
        }
    };
    for (const auto& b : s_.bombs) {
        if (!b.active) continue;  // carried slots are inactive — stamped below
        std::int32_t elapsed_ms;
        if (b.fuse <= 0) {
            elapsed_ms = b.fuse_init * kMsPerTick;  // aged trigger bomb (see above)
        } else {
            elapsed_ms = (b.fuse_init - b.fuse) * kMsPerTick;
            if (elapsed_ms < 0) elapsed_ms = 0;
        }
        stamp_blast(b.tile_x(), b.tile_y(), b.flame, elapsed_ms + 100);
    }
    // Carried bombs (motion 3): sub_42331C's carried pass (25784, called per
    // frame at 29530) runs the SAME tail stamp — a bomb on a player's head
    // projects its blast from the CARRIER's tile every frame, which is why
    // the original's AIs scatter around a bomb-carrying player. Our carried
    // bombs are deactivated slots (BombSystem::try_grab), so stamp them off
    // the carrier's own fields; the elapsed term is 0 (v = 100) — the port
    // tracks no carry age (the throw restarts the fuse anyway; documented
    // approximation, facts.md "AI danger map").
    for (const auto& p : s_.players) {
        if (!p.present || !p.alive || !p.carrying) continue;
        stamp_blast(p.tile_x(), p.tile_y(), p.carried_flame, 100);
    }

    // 3) The closing walls ("fire-god"): during the hurry phase the enclosure
    //    updater (sub_426818 ~27200) walks the NEXT getvalue(910)=15 imminent
    //    spiral bricks and writes a decaying v = 10*getvalue(910)+100, then
    //    -= 10 per step, so AIs flee the shrinking arena ~15 tiles ahead
    //    (docs/re/ai.md §4.3). getvalue(910) is our fire_god_lookahead tunable.
    if (s_.hurry) {
        const int depth = s_.tuning.enclosement_depth;
        const int lookahead = s_.tuning.fire_god_lookahead;  // getvalue(910) = 15
        const int total = enclose_total(depth);
        // The original's walk (pseudo.c 27201-27223) advances its own spiral
        // cursor one candidate per iteration and, when the candidate exits
        // the current ring, TURNS instead — burning that iteration on a
        // re-stamp of the pre-turn tile with the already-decayed value — so
        // near corners it covers fewer than 15 distinct future bricks.
        // Reproduced here by burning an iteration whenever the direction
        // between consecutive spiral positions changes (the ring-advance
        // diagonal counts as a turn too — a documented approximation of the
        // ++ring/++x/++y branch; facts.md "AI danger map" item 6).
        std::int32_t v = 10 * lookahead + 100;
        int idx = s_.enclose_index;  // walls not yet dropped, in order
        int prev_x = -1, prev_y = -1, pdx = 0, pdy = 0;
        for (int k = 0; k < lookahead && idx < total; ++k) {
            int wx = 0, wy = 0;
            if (!enclose_pos(idx, depth, &wx, &wy)) break;
            if (prev_x >= 0) {
                const int dx = wx - prev_x, dy = wy - prev_y;
                if (dx != pdx || dy != pdy) {
                    raise(prev_x, prev_y, v);  // wasted corner iteration
                    v -= 10;
                    pdx = dx;
                    pdy = dy;
                    continue;  // the spiral does not advance this iteration
                }
            }
            raise(wx, wy, v);
            v -= 10;
            if (prev_x >= 0) {
                pdx = wx - prev_x;
                pdy = wy - prev_y;
            }
            prev_x = wx;
            prev_y = wy;
            ++idx;
        }
    }
}

std::int32_t AISystem::danger_at(int tx, int ty) const {
    if (!grid::in_grid(tx, ty)) return 0;  // sub_424D37 bounds-checks to 0
    return danger_[ty][tx];
}

bool AISystem::obstacle_at(int tx, int ty) const {
    if (!grid::in_grid(tx, ty)) return true;  // sub_409083: out-of-bounds == blocked
    return obstacle_[ty][tx] != 0;
}

// sub_40A59D: "may I stand here" — the exact predicate from the binary
// (0x40A59D, read fresh): NOT a bomb (sub_422E48), NOT a solid/brick cell
// (sub_425FB9 != 0), NOT flame (sub_42708D), and danger == 0 (sub_424D37 == 0).
// It does NOT reject a floor powerup — sub_40A59D never calls the powerup test
// (sub_42542D). So the AI MAY step onto a powerup tile; that is how behaviour 5
// takes its final step onto the target and lets the Powerups system collect it.
// (Corrects a Stage-2 over-rejection that treated powerup tiles as unwalkable;
// strict 1:1 with sub_40A59D — powerups only affect the sub_42542D goal tests.)
bool AISystem::safe_tile(int tx, int ty) const {
    if (!grid::in_grid(tx, ty)) return false;
    if (s_.cells[ty][tx] != Cell::Blank || s_.burning[ty][tx] > 0) return false;  // sub_425FB9
    if (grid::bomb_at(s_, tx, ty) != nullptr) return false;                       // sub_422E48
    if (s_.flame[ty][tx] > 0) return false;                                       // sub_42708D
    return danger_at(tx, ty) == 0;                                                // sub_424D37
}

// sub_423188: the drop-tile CLEARANCE predicate (0x423188) — "may a bomb be
// placed on THIS tile". Byte-exact (0x423188):
//     if (sub_422E48(x,y)) return 0;               // a bomb already here
//     v5 = sub_405654(x,y);
//     return (!v5 || v5[1] != 1) && sub_425FB9(x,y) == 0;
// It is NOT an escape search — the original does no look-ahead here and trusts
// behaviour 2 to flee the resulting blast. Gates behaviours 3 and 4.
//
// CORRECTED 2026-07-26 (facts.md "AI never bombs a warphole"): `sub_405654` is
// the STAGE-ACTOR registry lookup (dword_45E0A8, stride 38 dwords, tile match on
// +28/+32 — docs/re/stage-actors.md §1), NOT a campaign rover/ghost list, and
// `v5[1]` is the actor's TYPE word at +4. So `v5[1] != 1` rejects a WARPHOLE
// tile — the identical tail `sub_4230A5` uses to make a warphole impassable to a
// sliding bomb (stage-actors.md §4 note 4, which read the same expression
// correctly). ai.md §3.3's old "rover/ghost list, empty in versus" gloss was
// wrong and this port inherited it, which is why our AI bombed warpholes (and
// span the deny SFX) where the original never even presses the key. Only type 1
// blocks: an AI may still drop on a dirarrow(0)/conveyor(2)/trampoline(3).
bool AISystem::drop_tile_clear(int tx, int ty) const {
    if (!grid::in_grid(tx, ty)) return false;                // out of bounds: sub_425FB9 -> 1
    if (grid::bomb_at(s_, tx, ty) != nullptr) return false;  // sub_422E48
    if (s_.actor_type[ty][tx] == ActorType::Warphole) return false;  // sub_405654 -> v5[1] == 1
    return s_.cells[ty][tx] == Cell::Blank && s_.burning[ty][tx] == 0;  // sub_425FB9 == 0
}

// sub_4245DA(idx) (0x4245DA): count of live bomb slots OWNED by player idx —
// the bomb dword at +60's high word is the owner index written at creation
// (sub_422EDE word-store to +62), not a tile X. Behaviours 3/4 gate on
// `sub_4245DA(me) < maxBombs(+86)` (the comparand Hex-Rays lost as an
// "undefined edx" is `mov dl, [actor+0x56]` in the raw disasm) — i.e. the
// standard spare-bomb-capacity check, the very count the mover compares at a
// normal drop. Player::bombs_placed is the sim's maintained equivalent of
// that owner scan (BombSystem::drop gates on the same counter), so the AI
// reads it directly instead of rescanning. docs/re/ai.md §9.3 (RESOLVED).
bool AISystem::out_of_bomb_slots(const Player& p) const {
    return p.bombs_placed >= p.max_bombs;
}

}  // namespace bomber::sim
