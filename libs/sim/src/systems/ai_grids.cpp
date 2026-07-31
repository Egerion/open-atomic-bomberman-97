// AI danger/obstacle grids and the tile predicates that read them (sub_424D37
// danger reader, sub_409083 obstacle grid, plus safe_tile / drop_tile_clear /
// out_of_bomb_slots). See ai.hpp for the subsystem API and docs/re/ai.md for the
// RE facts.

#include "systems/ai.hpp"

#include <algorithm>

#include "bomber/sim/simulation.hpp"  // enclose_pos / enclose_total for the wall look-ahead
#include "grid.hpp"

namespace bomber::sim {

// The danger writer keeps the strongest threat: grid = max(grid, v)
// (sub_424DFE).
void AISystem::raise_danger(int x, int y, std::int32_t v) {
    if (grid::in_grid(x, y) && v > danger_[y][x]) danger_[y][x] = v;
}

// The bomb updater (sub_42331C 25683-25705) writes v at the bomb tile and
// propagates the SAME v along the 4 rays out to the bomb's flame length,
// stopping at a wall/brick (sub_425FB9) or another bomb (sub_422E48), and one
// tile PAST a floor powerup (sub_42542D).
void AISystem::stamp_blast(int bx, int by, int reach, std::int32_t v) {
    raise_danger(bx, by, v);
    for (int d = 0; d < 4; ++d) {
        for (int i = 1; i <= reach; ++i) {
            const int rx = bx + grid::kDx[d] * i, ry = by + grid::kDy[d] * i;
            if (!grid::in_grid(rx, ry)) break;
            // Stop AT a wall/brick without marking it: flame won't reach a solid,
            // and a brick blocks propagation.
            if (s_.cells[ry][rx] != Cell::Blank || s_.burning[ry][rx] > 0) break;
            if (grid::bomb_at(s_, rx, ry) != nullptr) break;  // stop AT another grounded bomb
            raise_danger(rx, ry, v);
            if (s_.floor[ry][rx] != PowerupType::None) break;  // mark it, then stop
        }
    }
}

// Danger source 2, live bombs. Three facts CORRECTED by the 2026-07-12
// danger-map audit (facts.md "AI danger map"):
//   - the bomb's +68 is the elapsed fuse phase in MILLISECONDS (it accrues the
//     per-frame ms delta), so v ranges 100..~2100 over a 2 s fuse. Cross-source
//     ordering against the closing walls (110..250) and flame (1000) depends on
//     that scale, so tick counts MUST be ms-scaled. A waiting trigger bomb
//     accrues without bound; our image has no age counter, so it ranks at a full
//     fuse's worth — past the point it outranks flame, as an aged trigger bomb
//     does in the original (documented approximation);
//   - FLYING bombs stamp too: the tail stamp's only gates are the active flag and
//     the carried-pass parity, motion is never tested, so a punched or thrown
//     bomb casts its blast from its instantaneous arc tile every frame (its +68
//     is frozen mid-air, matching our paused fuse). The old `|| b.flying` skip
//     made AIs stand calmly under a sailing bomb;
//   - the per-bomb duration word +74 (our fuse_init), not the global tuning
//     value, anchors the elapsed computation.
void AISystem::stamp_bomb_danger() {
    for (const auto& b : s_.bombs) {
        if (!b.active) continue;  // carried slots are inactive — stamped below
        std::int32_t elapsed_ms = b.fuse_init * kMsPerTick;  // aged trigger bomb (see above)
        if (b.fuse > 0) elapsed_ms = std::max(0, (b.fuse_init - b.fuse) * kMsPerTick);
        stamp_blast(b.tile_x(), b.tile_y(), b.flame, elapsed_ms + 100);
    }
    // Carried bombs (motion 3): sub_42331C's carried pass (25784, called per
    // frame at 29530) runs the SAME tail stamp — a bomb on a player's head
    // projects its blast from the CARRIER's tile every frame, which is why the
    // original's AIs scatter around a bomb-carrying player. Our carried bombs are
    // deactivated slots (BombSystem::try_grab), so stamp them off the carrier's
    // own fields. The elapsed term is 0 because the port tracks no carry age; the
    // throw restarts the fuse anyway (documented approximation).
    for (const auto& p : s_.players) {
        if (!p.present || !p.alive || !p.carrying) continue;
        stamp_blast(p.tile_x(), p.tile_y(), p.carried_flame, 100);
    }
}

// Danger source 3, the closing walls ("fire-god", docs/re/ai.md §4.3): during the
// hurry phase the enclosure updater (sub_426818 ~27200) walks the NEXT
// getvalue(910) = 15 imminent spiral bricks writing a decaying v = 10*910 + 100,
// then -= 10 per step, so AIs flee the shrinking arena ~15 tiles ahead.
//
// The original's walk (pseudo.c 27201-27223) advances its own spiral cursor one
// candidate per iteration and, when the candidate exits the current ring, TURNS
// instead — burning that iteration on a re-stamp of the pre-turn tile with the
// already-decayed value — so near corners it covers fewer than 15 distinct future
// bricks. Reproduced by burning an iteration whenever the direction between
// consecutive spiral positions changes; the ring-advance diagonal counts as a
// turn too, a documented approximation of the ++ring/++x/++y branch (facts.md
// "AI danger map" item 6).
void AISystem::stamp_wall_danger() {
    const int depth = s_.tuning.enclosement_depth;
    const int lookahead = s_.tuning.fire_god_lookahead;  // getvalue(910) = 15
    const int total = enclose_total(depth);
    std::int32_t v = 10 * lookahead + 100;
    int idx = s_.enclose_index;  // walls not yet dropped, in order
    int prev_x = -1, prev_y = -1, pdx = 0, pdy = 0;
    for (int k = 0; k < lookahead && idx < total; ++k) {
        int wx = 0, wy = 0;
        if (!enclose_pos(idx, depth, &wx, &wy)) break;
        const int dx = wx - prev_x, dy = wy - prev_y;
        if (prev_x >= 0 && (dx != pdx || dy != pdy)) {
            raise_danger(prev_x, prev_y, v);  // the wasted corner iteration
            v -= 10;
            pdx = dx;
            pdy = dy;
            continue;  // the spiral does not advance this iteration
        }
        raise_danger(wx, wy, v);
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

// Rebuilt from hashed State once per tick and shared by every AI player, as in
// the original (a sim-global grid, not per-AI). NOT hashed — a pure function of
// State, like s.events. docs/re/ai.md §4/§5.
void AISystem::ensure_grids() {
    if (grids_valid_ && grids_tick_ == s_.tick) return;
    grids_tick_ = s_.tick;
    grids_valid_ = true;

    // Obstacle grid (sub_409083): 1 where a tile is solid/brick/burning or has a
    // GROUNDED bomb, else 0. Airborne bombs occupy no tile.
    grid::for_each_cell([&](int x, int y) {
        obstacle_[y][x] = grid::tile_open(s_, x, y) ? 0 : 1;
        danger_[y][x] = 0;
    });
    for (const auto& b : s_.bombs)
        if (b.active && !b.flying) obstacle_[b.tile_y()][b.tile_x()] = 1;

    // Danger source 1: an active flame is a flat 1000 at every lit cell (the
    // flame updater sub_426D06 writes 1000, ai.md §4.1) — maximally dangerous.
    grid::for_each_cell([&](int x, int y) {
        if (s_.flame[y][x] > 0) raise_danger(x, y, 1000);
    });
    stamp_bomb_danger();
    if (s_.hurry) stamp_wall_danger();
}

std::int32_t AISystem::danger_at(int tx, int ty) const {
    if (!grid::in_grid(tx, ty)) return 0;  // sub_424D37 bounds-checks to 0
    return danger_[ty][tx];
}

bool AISystem::obstacle_at(int tx, int ty) const {
    if (!grid::in_grid(tx, ty)) return true;  // sub_409083: out-of-bounds == blocked
    return obstacle_[ty][tx] != 0;
}

// sub_40A59D "may I stand here", the exact predicate from the binary (0x40A59D,
// read fresh): NOT a bomb (sub_422E48), NOT a solid/brick cell (sub_425FB9 != 0),
// NOT flame (sub_42708D), and danger == 0 (sub_424D37 == 0).
//
// It does NOT reject a floor powerup — sub_40A59D never calls sub_42542D — so the
// AI MAY step onto a powerup tile, which is how behaviour 5 takes its final step
// onto the target and lets the Powerups system collect it. A Stage-2
// over-rejection treated powerup tiles as unwalkable; powerups affect only the
// sub_42542D goal tests.
bool AISystem::safe_tile(int tx, int ty) const {
    if (!grid::in_grid(tx, ty)) return false;
    if (s_.cells[ty][tx] != Cell::Blank || s_.burning[ty][tx] > 0) return false;  // sub_425FB9
    if (grid::bomb_at(s_, tx, ty) != nullptr) return false;                       // sub_422E48
    if (s_.flame[ty][tx] > 0) return false;                                       // sub_42708D
    return danger_at(tx, ty) == 0;                                                // sub_424D37
}

// sub_423188, the drop-tile CLEARANCE predicate: three tests in this order —
// sub_422E48 reports no bomb on the tile; the stage-actor registry lookup
// (sub_405654) does not hit an actor whose type is 1; and the blank-cell
// predicate sub_425FB9 returns 0. It is NOT an escape search: the original does
// no look-ahead here and trusts behaviour 2 to flee the resulting blast.
//
// CORRECTED 2026-07-26 (facts.md "AI never bombs a warphole"): sub_405654 is the
// STAGE-ACTOR registry lookup (dword_45E0A8, stride 38 dwords, tile match on
// +28/+32 — docs/re/stage-actors.md §1), NOT a campaign rover/ghost list, and the
// field it tests is the actor's TYPE word at +4. So type 1 means a WARPHOLE — the
// identical tail sub_4230A5 uses to make a warphole impassable to a sliding bomb.
// ai.md §3.3's old "rover/ghost list, empty in versus" gloss was wrong and this
// port inherited it, which is why our AI bombed warpholes (and span the deny SFX)
// where the original never even presses the key. Only type 1 blocks: an AI may
// still drop on a dirarrow(0), conveyor(2) or trampoline(3).
bool AISystem::drop_tile_clear(int tx, int ty) const {
    if (!grid::in_grid(tx, ty)) return false;                // out of bounds: sub_425FB9 -> 1
    if (grid::bomb_at(s_, tx, ty) != nullptr) return false;  // sub_422E48
    if (s_.actor_type[ty][tx] == ActorType::Warphole) return false;     // sub_405654 type == 1
    return s_.cells[ty][tx] == Cell::Blank && s_.burning[ty][tx] == 0;  // sub_425FB9 == 0
}

// sub_4245DA(idx): the count of live bomb slots OWNED by player idx — the bomb
// dword at +60's high word is the owner index written at creation (sub_422EDE
// word-store to +62), not a tile X. Behaviours 3/4 gate on that count staying
// strictly below the player's max-bomb byte at +86 (the comparand the decompiler
// lost as an "undefined edx" is a byte load from the actor's +0x56 in the raw
// disassembly) — the standard spare-capacity check, the same count the mover
// compares at a normal drop. bombs_placed is the sim's maintained equivalent of
// that owner scan. docs/re/ai.md §9.3 (RESOLVED).
bool AISystem::out_of_bomb_slots(const Player& p) const {
    return p.bombs_placed >= p.max_bombs;
}

}  // namespace bomber::sim
