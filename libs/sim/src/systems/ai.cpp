// The computer-player AI (ADR-0005, docs/re/ai.md). Stage 2: the dispatcher
// skeleton, the danger + obstacle grids, the flee branch + the wander fallback.
// Stage 3: the directed BFS (sub_4092A1), behaviour 2's directed branch, the
// powerup scan (sub_409C1F), and behaviour 5 (seek a nearby powerup, sub_40BAF5).
// Stage 4: behaviour 0 (grab-glove drop/hold, sub_40BD44) and behaviour 3
// (blast bricks, sub_40AD8D) — both DROP by setting the bomb-key edge on the
// produced input so the normal BombSystem path runs in player_turn.
// Every mechanic mirrors a named original function; comments cite the sub_XXXX.
// Integer only; RNG only via State::rng in the RE'd order/count (docs/re/ai.md
// §8). See ai.hpp for the staged scope.

#include "systems/ai.hpp"

#include <algorithm>  // std::max for the blast-bricks getvalue(915) clamp

#include "bomber/sim/rng.hpp"
#include "bomber/sim/simulation.hpp"  // enclose_pos / enclose_total for the wall look-ahead
#include "grid.hpp"

namespace bomber::sim {

namespace {

// Godir unit vectors in the original's order (0=Up,1=Right,2=Down,3=Left),
// exactly dword_45BECC={0,1,0,-1} / dword_45BEDC={-1,0,1,0} (docs/re/ai.md §3.3).
constexpr int kDX[4] = {0, 1, 0, -1};
constexpr int kDY[4] = {-1, 0, 1, 0};

// Godir -> our Direction enum for writing the input direction flag.
constexpr Direction kGodirDir[4] = {Direction::Up, Direction::Right, Direction::Down,
                                    Direction::Left};

// Behaviour 4's enemy-scan cross (sub_40ABED, docs/re/ai.md §3.4 / §9.4). The
// original walks i in 0..4 reading dword_45BAB0[i] for the X offset and
// dword_45BA9C[i] for the Y offset. dword_45BA9C[5] = {0,-1,0,1,0} is a clean
// vertical cross; dword_45BAB0[] is a ONE-element array {-1}, so reading [1..4]
// runs PAST it into the adjacent .data — an original out-of-bounds read. Reading
// the shipped BM95.EXE bytes at 0x45BAB0..0x45BAC0 gives {-1,0,0,0,1} for those
// five dwords (the "garbage" after -1 happens to be 0,0,0,1). Combined with the
// Y table that yields a PERFECT 5-tile plus/cross centred on the AI: LEFT, UP,
// SELF(0,0), DOWN, RIGHT. We reproduce those exact constants (a faithful image
// of the OOB read, not a crash — it never faults in practice). Index 2 is the
// AI's own tile; the original excludes self by zeroing its actor +0 across the
// sub_421CB5 probe, which our scan mirrors by skipping the self slot.
constexpr int kEnemyScanX[5] = {-1, 0, 0, 0, 1};  // dword_45BAB0[0..4] (OOB tail)
constexpr int kEnemyScanY[5] = {0, -1, 0, 1, 0};  // dword_45BA9C[5]

}  // namespace

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
// placed on THIS tile". Byte-exact reduction (docs/re/ai.md §3.3): no bomb here
// (sub_422E48) AND the cell is blank floor (sub_425FB9 == 0, i.e. NOT a wall or
// brick). It is NOT an escape search — the original does no look-ahead here and
// trusts behaviour 2 to flee the resulting blast. The original also skips a
// campaign-entity here (sub_405654, the rover/ghost list dword_45E0A8); that
// list is empty in the versus AI, so it drops out. Used by behaviour 3.
bool AISystem::drop_tile_clear(int tx, int ty) const {
    if (!grid::in_grid(tx, ty)) return false;                // out of bounds: sub_425FB9 -> 1
    if (grid::bomb_at(s_, tx, ty) != nullptr) return false;  // sub_422E48
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

// ---------------------------------------------------------------------------
// Flee BFS (sub_40970B, docs/re/ai.md §5.2). Same 100-node wavefront as the
// directed BFS but with no goal tile: every reachable cell is scored by the
// danger grid, tracking the minimum; the instant a danger-0 tile is reached its
// path's first step is returned, else the frontier is exhausted and the first
// step toward the lowest-danger tile is returned. Draws the ±1 tie-break ONCE
// at entry (part of the RNG contract), which flips the order equal-length
// branches expand so ties resolve randomized-but-deterministically.
// Returns the first-step godir (0..3), or -1 if boxed in; writes best_x/best_y.
// ---------------------------------------------------------------------------
int AISystem::flee_bfs(int sx, int sy, int& best_x, int& best_y) {
    // Per-call tie-break: 2*(rand()%2)-1 == -1 or +1 (docs/re/ai.md §5.2). This
    // is the flee behaviour's single RNG draw, taken before any expansion.
    const int tie = 2 * static_cast<int>(random_below(s_, 2)) - 1;

    // Node: (x, y, first_dir godir 0..3). Fixed 100-node open list (frontier
    // cap), matching dword_45ED68's 100 nodes. Visited tracked on a per-cell
    // epoch grid (O(1) reset via a bumped epoch — deterministic, no per-tick
    // clear). Cost is the ring order, implicit in the FIFO expansion.
    struct Node {
        int x, y, first, depth;
    };
    Node open[100];
    int open_n = 0;

    static thread_local std::uint64_t visited_epoch[kGridHeight][kGridWidth] = {};
    static thread_local std::uint64_t epoch = 0;
    ++epoch;

    best_x = sx;
    best_y = sy;
    // The best-tracker inits at 10000 (sub_40970B 9936), NOT at the start
    // tile's own danger — so with ANY open neighbour the BFS returns a step,
    // even when nothing beats the danger the AI is standing in; -1 means
    // "fully boxed in", nothing else (CORRECTED 2026-07-12, facts.md "AI
    // danger map" item 4 — the old start-danger init returned -1 whenever no
    // strictly-safer tile existed, sending the caller down the chain).
    std::int32_t best_danger = 10000;

    // Seed with the (up to 4) open, in-bounds neighbours of the start, each
    // tagged with the godir it came from (the eventual return value). The seed
    // order uses the tie-break so equal-cost first steps are randomized.
    auto consider = [&](int nx, int ny, int first) {
        if (obstacle_at(nx, ny)) return;
        if (visited_epoch[ny][nx] == epoch) return;
        visited_epoch[ny][nx] = epoch;
        if (open_n < 100) open[open_n++] = {nx, ny, first, 1};
        const std::int32_t dn = danger_at(nx, ny);
        if (dn < best_danger) {
            best_danger = dn;
            best_x = nx;
            best_y = ny;
        }
    };
    visited_epoch[sy][sx] = epoch;
    // Expand the four godirs in a tie-flipped order (0..3 or 3..0):
    for (int s2 = 0; s2 < 4; ++s2) {
        const int g = tie > 0 ? s2 : (3 - s2);
        consider(sx + kDX[g], sy + kDY[g], g);
    }
    if (best_danger == 0) {
        // A neighbour is already safe — return the first step toward it.
        for (int k = 0; k < open_n; ++k)
            if (open[k].x == best_x && open[k].y == best_y) return open[k].first;
    }

    // Breadth-first expansion over the frontier, propagating each node's
    // first-step tag. Stop early the moment a danger-0 tile is reached.
    int head = 0;
    while (head < open_n) {
        const Node cur = open[head++];
        // Ring cap: sub_40970B stops expanding after 20 rings (its a4 = 20,
        // pseudo.c 10048-10053) while keeping the best-so-far step — an
        // improvement further than 20 steps away is invisible to the
        // original's flee (CORRECTED 2026-07-12, facts.md "AI danger map").
        if (cur.depth >= 20) continue;
        for (int s2 = 0; s2 < 4; ++s2) {
            const int g = tie > 0 ? s2 : (3 - s2);
            const int nx = cur.x + kDX[g], ny = cur.y + kDY[g];
            if (obstacle_at(nx, ny)) continue;
            if (visited_epoch[ny][nx] == epoch) continue;
            visited_epoch[ny][nx] = epoch;
            const std::int32_t dn = danger_at(nx, ny);
            if (dn < best_danger) {
                best_danger = dn;
                best_x = nx;
                best_y = ny;
                if (dn == 0) return cur.first;  // reached a fully-safe tile: done
            }
            if (open_n < 100) open[open_n++] = {nx, ny, cur.first, cur.depth + 1};
        }
    }

    // Frontier exhausted: return the first step toward the min-danger tile.
    for (int k = 0; k < open_n; ++k)
        if (open[k].x == best_x && open[k].y == best_y) return open[k].first;
    return -1;
}

// ---------------------------------------------------------------------------
// Directed BFS (sub_4092A1, docs/re/ai.md §5.1). The same 100-node wavefront as
// the flee BFS, but with a fixed goal tile (tx,ty): it returns the first-step
// godir of a shortest path to the goal within `max_depth` rings, else -1. The
// original's goal test compares each expanded node against (a4,a3) == (tx,ty)
// (docs/re/ai.md §9.1); we test node==(tx,ty) directly. Draws the ±1 tie-break
// ONCE at entry — behaviour 2's directed step and behaviour 5's path each take
// exactly this one BFS draw (RNG contract §8). If the start already equals the
// goal, the original short-circuits before touching the frontier and returns
// firstdir 0 (no step); we mirror that (return -1, no expansion, but the draw
// is already taken, matching v35 = 2*(rand%2)-1 at the very top).
// ---------------------------------------------------------------------------
int AISystem::directed_bfs(int sx, int sy, int tx, int ty, int max_depth, int& out_iters) {
    // Per-call tie-break: 2*(rand()%2)-1 (docs/re/ai.md §5.1). Drawn at entry,
    // before any expansion and before the start==goal check, exactly as the
    // original draws v35 first (line 9705).
    const int tie = 2 * static_cast<int>(random_below(s_, 2)) - 1;
    out_iters = 0;

    // start == goal: the original's `if (a1 != a4 || a2 != a3)` guard skips the
    // whole search (firstdir stays 0). No path step needed — we are already there.
    if (sx == tx && sy == ty) return -1;

    struct Node {
        int x, y, first;
    };
    Node open[100];
    int open_n = 0;

    static thread_local std::uint64_t visited_epoch[kGridHeight][kGridWidth] = {};
    static thread_local std::uint64_t epoch = 0;
    ++epoch;

    // Seed with the (up to 4) open, in-bounds neighbours of the start, tagged
    // with the godir they came from (the eventual return value), in tie-flipped
    // order so equal-length first steps resolve randomized-but-deterministically.
    auto seed = [&](int nx, int ny, int first) -> int {
        if (obstacle_at(nx, ny)) return 0;
        if (nx == tx && ny == ty) return first + 1;  // neighbour IS the goal
        if (visited_epoch[ny][nx] == epoch) return 0;
        visited_epoch[ny][nx] = epoch;
        if (open_n < 100) open[open_n++] = {nx, ny, first};
        return 0;
    };
    visited_epoch[sy][sx] = epoch;
    for (int s2 = 0; s2 < 4; ++s2) {
        const int g = tie > 0 ? s2 : (3 - s2);
        const int hit = seed(sx + kDX[g], sy + kDY[g], g);
        if (hit) {
            // A neighbour IS the goal: found in "pass 0", so out_iters == 0 — this
            // matches the original's ring counter v32, which is not yet
            // incremented when the goal is hit on the first sweep. Behaviour 5
            // gates its 50%-give-up draw on `!iters`, so the iters==0 boundary
            // (goal ≤2 tiles away) must match the original exactly (RNG contract).
            out_iters = 0;
            return hit - 1;
        }
    }

    // Breadth-first expansion, propagating each node's first-step tag. out_iters
    // increments once per completed ring (the original's ring counter v32, capped
    // at a5 == max_depth). While the distance-1 ring is processed out_iters stays
    // 0, so a goal at distance 2 is also found with out_iters == 0 — matching the
    // original (see the iters==0 note above). Stop the instant the goal is hit.
    int head = 0;
    int ring_end = open_n;
    while (head < open_n) {
        if (head == ring_end) {
            if (++out_iters > max_depth) break;  // depth cap (v32 > a5)
            ring_end = open_n;
        }
        const Node cur = open[head++];
        for (int s2 = 0; s2 < 4; ++s2) {
            const int g = tie > 0 ? s2 : (3 - s2);
            const int nx = cur.x + kDX[g], ny = cur.y + kDY[g];
            if (obstacle_at(nx, ny)) continue;
            if (nx == tx && ny == ty) {
                return cur.first;  // reached the goal: first step of this path
            }
            if (visited_epoch[ny][nx] == epoch) continue;
            visited_epoch[ny][nx] = epoch;
            if (open_n < 100) open[open_n++] = {nx, ny, cur.first};
        }
    }
    // Boxed in at the start (zero open neighbours were ever seeded, so the
    // while loop above never ran and out_iters is still its initial 0): the
    // original's do..while ALWAYS completes one pass before testing its loop
    // condition, so v32 (iters) becomes 1 even when nothing was found
    // (pseudo.c 9705-9821) -- it is never left at 0 once the function is
    // actually invoked. Behaviours 5/6 gate their unreachable-target give-up
    // draw on `iters == 0`, so leaving this at 0 draws a spurious extra
    // rand()%2 in the boxed-in case (RESOLVED, docs/re/ai.md §5.1/§9). This is
    // a no-op whenever any neighbour WAS seeded: the ring-drain above already
    // bumps out_iters to >= 1 before the max-depth or exhaustion exit.
    if (out_iters == 0) out_iters = 1;
    return -1;  // unreachable within max_depth
}

// ---------------------------------------------------------------------------
// Powerup scan BFS (sub_409C1F, docs/re/ai.md §5.5). Same wavefront as the
// directed BFS, but the goal test is "a floor powerup lies on this tile"
// (sub_42542D != 0 -> our s.floor[y][x] != None). Returns the first-step godir
// toward the nearest reachable floor powerup within `max_depth` rings, writing
// its tile to (found_x,found_y) and the ring count to out_iters; -1 if none.
// Draws the ±1 tie-break ONCE at entry (RNG contract §8, row b5b). Used only by
// behaviour 5's acquire step.
// ---------------------------------------------------------------------------
int AISystem::powerup_scan_bfs(int sx, int sy, int max_depth, int& out_iters, int& found_x,
                               int& found_y) {
    const int tie = 2 * static_cast<int>(random_below(s_, 2)) - 1;
    out_iters = 0;
    found_x = -1;
    found_y = -1;

    // A powerup underfoot: the original seeds only NEIGHBOURS (it never tests the
    // start tile), so a powerup on the start tile is NOT found by the scan — the
    // AI would already be standing on it and the Powerups system collects it.
    struct Node {
        int x, y, first;
    };
    Node open[100];
    int open_n = 0;

    static thread_local std::uint64_t visited_epoch[kGridHeight][kGridWidth] = {};
    static thread_local std::uint64_t epoch = 0;
    ++epoch;

    auto is_powerup = [&](int x, int y) {
        return grid::in_grid(x, y) && s_.floor[y][x] != PowerupType::None;
    };

    auto seed = [&](int nx, int ny, int first) -> int {
        if (obstacle_at(nx, ny)) return 0;
        if (is_powerup(nx, ny)) {
            found_x = nx;
            found_y = ny;
            return first + 1;
        }
        if (visited_epoch[ny][nx] == epoch) return 0;
        visited_epoch[ny][nx] = epoch;
        if (open_n < 100) open[open_n++] = {nx, ny, first};
        return 0;
    };
    visited_epoch[sy][sx] = epoch;
    for (int s2 = 0; s2 < 4; ++s2) {
        const int g = tie > 0 ? s2 : (3 - s2);
        const int hit = seed(sx + kDX[g], sy + kDY[g], g);
        if (hit) {
            out_iters = 0;  // "pass 0" hit — matches the original's ring counter
            return hit - 1;
        }
    }

    int head = 0;
    int ring_end = open_n;
    while (head < open_n) {
        if (head == ring_end) {
            if (++out_iters > max_depth) break;
            ring_end = open_n;
        }
        const Node cur = open[head++];
        for (int s2 = 0; s2 < 4; ++s2) {
            const int g = tie > 0 ? s2 : (3 - s2);
            const int nx = cur.x + kDX[g], ny = cur.y + kDY[g];
            if (obstacle_at(nx, ny)) continue;
            if (is_powerup(nx, ny)) {
                found_x = nx;
                found_y = ny;
                return cur.first;
            }
            if (visited_epoch[ny][nx] == epoch) continue;
            visited_epoch[ny][nx] = epoch;
            if (open_n < 100) open[open_n++] = {nx, ny, cur.first};
        }
    }
    // Same boxed-in iters=0-vs-1 correction as directed_bfs above (RESOLVED,
    // docs/re/ai.md §5.1/§9) -- kept for structural parity with sub_4092A1's
    // do..while, though currently unobservable here: behaviour 5 only reads
    // this iters value via `range+1 >= iters` gated on a FOUND cell, and a
    // boxed-in scan finds none, so the comparison (and this boundary) is
    // never reached from sub_40BAF5's acquire step.
    if (out_iters == 0) out_iters = 1;
    return -1;
}

// sub_40A76E: the step-into-flame veto. If the tile one step along godir `g`
// from (tx,ty) is on fire, cancel the step (return -1) and clear the AI state,
// so the AI never voluntarily walks into flame even when the flee said to.
int AISystem::flame_veto(int i, int tx, int ty, int g) {
    if (g < 0) return g;
    const int nx = tx + kDX[g], ny = ty + kDY[g];
    if (grid::in_grid(nx, ny) && s_.flame[ny][nx] > 0) {
        s_.brains[i].state_flag = 0;
        return -1;
    }
    return g;
}

// ---------------------------------------------------------------------------
// Behaviour 0 — sub_40BD44, grab-glove drop/hold (docs/re/ai.md §3.0), the
// highest-priority behaviour. Only for a player holding Grab (+92):
//   - if already carrying a grabbed bomb (+148): write the bomb key UP (+56=0,
//     +54=0) and act (return 1), suppressing every lower behaviour. This is NOT
//     an indefinite hold: the mover's carried-bomb throw block fires on !+56, so
//     forcing the key up LOBS the bomb the very next tick — the grab-AI grabs
//     then throws forward (docs/re/ai.md §3.0 CORRECTION);
//   - else, if standing on its OWN resting bomb, press the bomb key on a 1/2
//     whim (rand()%2) to snatch it: player_turn's drop block sees a fresh
//     action1 edge over the own bomb underfoot and routes to try_grab.
// RNG (§8 row b0): the rand()%2 fires ONLY when grab && !carrying && own bomb
// underfoot; otherwise this behaviour draws nothing. Team note: the original's
// bomb.team==player.team reduces, in a no-team match, to "the bomb is mine"
// (bomb.owner==self) — the same reduction player_turn's own grab block uses.
// ---------------------------------------------------------------------------
bool AISystem::behave_grab_drop(int i, PlayerInput& out) {
    Player& p = s_.players[i];
    if (!p.grab) return false;  // sub_40BD44: !+92 -> not our behaviour

    if (p.carrying) {
        // Carrying a grabbed bomb (+148): write the bomb key up (the original
        // sets +56=0; +54=0). We leave action1=false — a release — so
        // player_turn's throw block (fires on !action1, the same !+56 gate)
        // LOBS the carried bomb forward next tick, exactly as the original does.
        out.action1 = false;
        return true;  // act, short-circuit the chain
    }

    // Own bomb underfoot? bomb_at at the AI's tile, owner == self. sub_422E48
    // (pseudo.c 25031-25051) matches a RESTING **or SLIDING** bomb (motion !=
    // flying(2) && != carried(3)) — grid::bomb_at already excludes flying/
    // carried, and the original does NOT additionally require the bomb to be
    // at rest (RESOLVED: an earlier `!under->moving` guard here was a real
    // deviation — it silently dropped the whole draw for a sliding own bomb,
    // where the original still rolls; try_grab already supports mid-slide
    // pickup, bombs.cpp "motion states 0 AND 1 both qualify").
    const Bomb* under = grid::bomb_at(s_, p.tile_x(), p.tile_y());
    const bool own = under != nullptr && under->owner == static_cast<std::uint8_t>(i);
    // rand()%2 TRUTHY (== 1) -> grab it: pseudo.c 11025 is
    // `v3 && *(v3+62)==*(a1+62) && rand()%2` used directly as the branch
    // condition, not `!(rand()%2)` (RESOLVED: an earlier `== 0` here was an
    // inverted-polarity deviation from both the binary and ai.md §3.0's own
    // transcription, which already read it correctly as "and rand()%2").
    if (own && random_below(s_, 2) != 0) {
        press_bomb(out);  // fresh bomb-key edge -> try_grab in player_turn
        return true;
    }
    return false;  // pass down to behaviour 1/2/...
}

// ---------------------------------------------------------------------------
// Behaviour 2 — sub_40B20F, "walk the path" (docs/re/ai.md §3.2). The danger-
// present branch either paths to a held directed target (Stage 3, sub_4092A1) or
// flees to the safest reachable tile (Stage 2, sub_40970B); the danger-clear
// branch passes down unless boxed in. Returns true if it acted.
// ---------------------------------------------------------------------------
bool AISystem::behave_walk_path(int i, PlayerInput& out) {
    Player& p = s_.players[i];
    Brain& br = s_.brains[i];
    const int px = p.tile_x(), py = p.tile_y();

    if (danger_at(px, py) != 0) {
        // Standing in / near a threat. Two sub-modes (docs/re/ai.md §3.2):
        //   (a) a DIRECTED path target is held -> path to it (sub_4092A1), Stage 3;
        //   (b) no target -> FLEE to the safest reachable tile (sub_40970B), Stage 2.
        // Exactly ONE BFS runs, so exactly one BFS tie-break draw is taken this
        // tick (RNG contract §8 row b2) whichever sub-mode fires.

        // Stale-target invalidation: if a target is held whose captured cost was 0
        // (!cost, i.e. it was safe when chosen) but the tile is now dangerous, drop
        // it. Mirrors `if (+2 && !+8 && danger(target)) +2 = 0` (line 10784).
        if (br.has_path_target && br.path_target_cost == 0 &&
            danger_at(br.path_target_x, br.path_target_y) != 0) {
            br.has_path_target = false;
        }

        if (br.has_path_target) {
            // (a) Directed: walk one step toward the held goal (docs/re/ai.md
            // §3.2 directed branch). maxdist = 20 (the original's literal).
            int iters = 0;
            const int first = directed_bfs(px, py, br.path_target_x, br.path_target_y, 20, iters);
            if (first < 0) {
                // No path within depth -> drop the target and pass down (the
                // original clears +2 and returns 0).
                br.has_path_target = false;
                return false;
            }
            const int g = flame_veto(i, px, py, first);  // sub_40A76E veto
            write_move(out, g);
            // The original's return here is `vel_perp(+44)>>16 != -1`, i.e. the
            // godir word the veto may have just set to -1 (pseudo.c 10841-10842:
            // `sub_40A76E(v4); return *(int*)(v4+44)>>16 != -1;`) -- NOT an
            // unconditional 1. A flame-vetoed step makes behaviour 2 PASS DOWN
            // (return 0), giving 3-7 a turn (and their draws) this tick, rather
            // than stalling. RNG-order-critical: fixing a fall-through this
            // hard-coded `true` used to swallow (docs/re/ai.md §3.2 RESOLVED).
            return g != -1;
        }

        // (b) Flee: no directed goal, run to the lowest-danger reachable tile.
        int bx = 0, by = 0;
        const int first = flee_bfs(px, py, bx, by);  // the flee behaviour's RNG draw

        // The original always latches has_path_target = 1 after the flee BFS
        // (line 10820) and then either STANDS or STEPS — the danger branch
        // NEVER passes down (CORRECTED 2026-07-12, facts.md "AI danger map"
        // item 4): the `here <= min` stand-latch (10824-10829) covers both
        // "no strictly-safer tile" AND "fully boxed in" (flee firstdir 0),
        // storing the OWN tile as target, writing godir -1 and returning 1 —
        // so behaviours 3-7 never run and draw NOTHING that frame. The old
        // pass-down ran the rest of the chain on every such frame: extra whim
        // draws, possible bomb drops and wander re-rolls while standing in
        // inescapable danger — an RNG-stream and activity divergence.
        const std::int32_t here = danger_at(px, py);
        if (first < 0 || here <= danger_at(bx, by)) {
            br.has_path_target = true;
            br.path_target_x = static_cast<std::int16_t>(px);
            br.path_target_y = static_cast<std::int16_t>(py);
            br.path_target_cost = here;  // nonzero: the stale-target drop above ignores it
            write_move(out, -1);
            return true;
        }

        // Step toward the safer tile, but veto a step that lands on flame.
        br.has_path_target = true;
        br.path_target_x = static_cast<std::int16_t>(bx);
        br.path_target_y = static_cast<std::int16_t>(by);
        br.path_target_cost = danger_at(bx, by);
        const int g = flame_veto(i, px, py, first);
        write_move(out, g);
        // Same `vel_perp(+44)>>16 != -1` return as the directed branch above
        // (pseudo.c 10841-10842): a flame-vetoed flee step passes down instead
        // of stalling, letting 3-7 take this tick's turn (docs/re/ai.md §3.2
        // RESOLVED). The "can't improve" branch above (line ~569) is unaffected
        // -- the original returns 1 unconditionally there and never calls the
        // veto (it has no godir to veto: the step is already -1).
        return g != -1;
    }

    // Danger-clear (safe). The original: if trigger held (and no punch) roll
    // rand()%10 to detonate remote bombs (Stage 5 — the whim's draw lands here in
    // the §8 order, before the neighbour scan). The && short-circuits so the %10
    // draw is taken ONLY when trigger && !punch, matching `if (+95 && !+91 &&
    // !(rand()%10)) +57=1`. Setting action2 routes to detonate_triggered in
    // player_turn (edge-gated on action2 && !prev_action2). Then check whether ANY
    // neighbour is walkable+safe; if boxed in with nowhere safe, "act" (stall) so
    // the chain stops, else pass down to let a lower behaviour (wander) drift.
    if (p.trigger && !p.punch && random_below(s_, 10) == 0) press_action(out);
    br.has_path_target = false;
    for (int g = 0; g < 4; ++g)
        if (safe_tile(px + kDX[g], py + kDY[g])) return false;  // a way out exists: pass down
    // Boxed in and nowhere safe to step: stall in place and stop the chain.
    write_move(out, -1);
    return true;
}

// ---------------------------------------------------------------------------
// Behaviour 3 — sub_40AD8D, blast bricks (docs/re/ai.md §3.3). Drops a bomb to
// open the board when standing next to a destroyable brick, gated on a 1-in-
// getvalue(915)=5 whim. Structure (byte-exact):
//   1. capacity guard: sub_4245DA(me) — my live-bomb count — must be < my
//      maxBombs(+86) (the disasm-confirmed comparand, §9.3 RESOLVED);
//   2. constipation (+134) blocks the drop;
//   3. count the orthogonally-adjacent BRICK tiles (cell type 2);
//   4. if none: clear state_flag 9->0 (situation resolved) and pass down;
//   5. if the standing tile is clear to drop on (sub_423188): roll rand()%5 —
//      on 0, press the bomb key (drop) and set state_flag = 9; else pass down.
// It does NOT flee here: state_flag=9 is a "committed to the drop" marker; the
// danger grid lights up under the new bomb next tick and behaviour 2 (higher
// priority) paths the AI out (docs/re/ai.md §3.3). state_flag=9 self-clears when
// there are no adjacent bricks / capacity fills — i.e. once the AI has moved.
// RNG (§8 row b3): the rand()%5 fires ONLY when the capacity/constipation/brick/
// clearance gates all pass; otherwise this behaviour draws nothing.
// ---------------------------------------------------------------------------
bool AISystem::behave_blast_bricks(int i, PlayerInput& out) {
    Player& p = s_.players[i];
    Brain& br = s_.brains[i];
    const int px = p.tile_x(), py = p.tile_y();

    // (1) Capacity guard (sub_4245DA(me) >= maxBombs(+86) -> pass down; §9.3
    // RESOLVED from the raw disasm — NOT a bombs-in-my-column rule). On the
    // "no spare slot" branch the original also clears a stale state_flag 9
    // before returning 0.
    if (out_of_bomb_slots(p)) {
        if (br.state_flag == 9) br.state_flag = 0;
        return false;
    }

    // (2) Constipation (+134): can't drop at all. (No state clear on this branch,
    // matching the original — the `if (+134) return 0` is inside the capacity-ok
    // block, before the brick count / state handling.)
    if (p.sick(Disease::Constipation)) return false;

    // (3) Count orthogonally-adjacent BRICK tiles (sub_425FB9 == 2 -> Cell::Brick).
    int bricks = 0;
    for (int d = 0; d < 4; ++d) {
        const int nx = px + kDX[d], ny = py + kDY[d];
        if (grid::in_grid(nx, ny) && s_.cells[ny][nx] == Cell::Brick) ++bricks;
    }

    // (4) No adjacent bricks: clear the commit flag and pass down.
    if (bricks == 0) {
        if (br.state_flag == 9) br.state_flag = 0;
        return false;
    }

    // (5) The standing tile must be clear to drop a bomb on (sub_423188). Not a
    // look-ahead escape check — behaviour 2 handles the flee next tick.
    if (!drop_tile_clear(px, py)) return false;

    // The 1-in-getvalue(915)=5 whim. r = max(1, getvalue(915)); rand()%r != 0 ->
    // pass down; == 0 -> DROP. ai_blast_chance is our getvalue(915) tunable.
    const auto r = static_cast<std::uint32_t>(std::max(1, s_.tuning.ai_blast_chance));
    if (random_below(s_, r) != 0) return false;

    // Drop: a fresh bomb-key edge routes to BombSystem::drop in player_turn (the
    // normal placement, with the same dud-gate RNG a human drop takes). Mark the
    // commit; behaviour 2 flees the resulting blast on the following tick(s).
    press_bomb(out);
    br.state_flag = 9;
    return true;
}

// ---------------------------------------------------------------------------
// Behaviour 5 — sub_40BAF5, seek a nearby powerup (docs/re/ai.md §3.5). On a
// 1/50 whim (only while NOT already latched) it BFS-scans (sub_409C1F) for a
// floor powerup within getvalue(920)=4 steps; if one is within range+1 steps it
// latches the target, then each tick paths toward it (sub_4092A1, maxdist =
// range+1) and steps one tile, giving up on a ~10-tick timeout, on loss of the
// powerup, or (50%) if it becomes unreachable. RNG order within this behaviour
// (§8 rows b5a..b5d): %50 acquire -> [scan tie-break if acquiring] -> path
// tie-break -> %2 give-up. Returns true if it acted (short-circuits the chain).
//
// Faithful quirk (docs/re/ai.md §3.5, verified in the decompile): the acquire
// resets the ENEMY-seek timer (+12) rather than the powerup-seek timer (+28) —
// an original field mix-up. We reproduce it (enemy_seek.timer = 0) for exact
// field parity; it is a no-op here since behaviour 6 (enemy-seek) is a Stage-5
// stub that never advances +12. The powerup timer (+28) is NOT reset on acquire.
// ---------------------------------------------------------------------------
bool AISystem::behave_seek_powerup(int i, PlayerInput& out) {
    Player& p = s_.players[i];
    Brain& br = s_.brains[i];
    const int px = p.tile_x(), py = p.tile_y();
    const int range = s_.tuning.ai_powerup_range;  // getvalue(920) = 4

    // Acquire: only when not already pursuing, on a 1/50 whim. The `&&` short-
    // circuits so the %50 draw is taken ONLY when !active (matches the original).
    if (!br.pow_seek.active && random_below(s_, 50) == 0) {
        int iters = 0, fx = 0, fy = 0;
        const int first = powerup_scan_bfs(px, py, range, iters, fx, fy);  // scan tie-break draw
        if (first >= 0) {
            // Found a reachable floor powerup; accept it only if within range+1
            // steps (the original: `getvalue(920)+1 >= nsteps`).
            if (range + 1 >= iters) {
                br.pow_seek.active = true;
                br.enemy_seek.timer = 0;  // the +12 write quirk (see header note)
                br.pow_seek.tile_x = static_cast<std::int16_t>(fx);
                br.pow_seek.tile_y = static_cast<std::int16_t>(fy);
            }
        }
    }

    if (!br.pow_seek.active) return false;  // no target: pass down to behaviour 6/7

    // Timeout after 500 ms of pursuit (original: `timer(+28) += frameDelta`
    // per frame; give up at `10 * [0x46494C]` = 10 × 50 ms. NO rand draw on
    // this timeout, unlike enemy-seek). The timer is wall-clock ms, accrued
    // per sub-frame — at the canonical 60 fps that is ~30 decides, exactly
    // the original's own count.
    br.pow_seek.timer += delta_ms_;
    if (br.pow_seek.timer >= 10 * kMsPerTick) {
        br.pow_seek.active = false;
        return false;
    }

    // Liveness: the original reloads the cell pointer and drops the target if it
    // is null or no longer a live powerup (*cell != 2). Our tile image: if the
    // floor powerup at the stored tile is gone (collected/blasted), give up.
    const int tx = br.pow_seek.tile_x, ty = br.pow_seek.tile_y;
    if (!grid::in_grid(tx, ty) || s_.floor[ty][tx] == PowerupType::None) {
        br.pow_seek.active = false;
        return false;
    }

    // Path toward the powerup (maxdist = range+1, the original's v3+1). One BFS
    // tie-break draw. If unreachable (0 iters) give up 50% of the time BEFORE the
    // firstdir check, exactly as the original orders it (line 10989).
    int iters = 0;
    const int first = directed_bfs(px, py, tx, ty, range + 1, iters);
    if (iters == 0 && random_below(s_, 2) != 0) {
        br.pow_seek.active = false;
    }
    if (first < 0) {
        br.pow_seek.active = false;
        return false;
    }

    // Take the step, but only if the next tile is safe to stand on (sub_40A59D);
    // otherwise hold (godir -1). The original records the step dir at +36 first.
    br.pow_seek.step_dir = static_cast<std::int8_t>(first);
    const int nx = px + kDX[first & 3], ny = py + kDY[first & 3];
    write_move(out, safe_tile(nx, ny) ? first : -1);
    return true;  // acted
}

// ---------------------------------------------------------------------------
// Behaviour 1 — sub_40BE02, punch a bomb ahead (docs/re/ai.md §3.1). Byte-exact
// (0x40BE02): only for a player holding the punch glove (+91). On a 1-in-4 whim
// (rand()%4 != 0 -> pass) it scans the four orthogonally-adjacent tiles (the
// 4-element godir tables dword_45BECC/45BEDC) for a bomb; if one is found it
// faces that tile (+46 = i) and sets the action key edge (+57=1; +55=0) so
// player_turn's try_punch swings. Returns true if it acted.
//
// Port note: try_punch uses p.facing, and MovementSystem::move sets p.facing = d
// unconditionally (even when the step is blocked by the bomb), so writing the
// direction toward the bomb faces the AI at it before the action2 block runs.
// RNG (§8 row b1): the rand()%4 fires ONLY when the player holds punch; a punch
// with no bomb ahead still draws the %4 (it is above the bomb scan) but does not
// act. player_turn also gates try_punch on !action1 — the AI's action1 is false
// here (we only set action2), so the swing is not suppressed.
// ---------------------------------------------------------------------------
bool AISystem::behave_punch(int i, PlayerInput& out) {
    Player& p = s_.players[i];
    if (!p.punch) return false;                  // sub_40BE02: !+91 -> not our behaviour
    if (random_below(s_, 4) != 0) return false;  // rand()%4 != 0 -> consider it only 1-in-4

    const int px = p.tile_x(), py = p.tile_y();
    int face = -1;
    for (int g = 0; g < 4; ++g) {  // the 4-element godir cross (dword_45BECC/45BEDC)
        if (grid::bomb_at(s_, px + kDX[g], py + kDY[g]) != nullptr) {
            face = g;
            break;  // first bomb found, in godir order (Up,Right,Down,Left)
        }
    }
    if (face < 0) return false;  // no bomb orthogonally adjacent: pass down

    // Face the bomb (the direction flag sets p.facing in the mover) and set the
    // action2 edge so try_punch launches the bomb 3 tiles in that direction.
    write_move(out, face);
    press_action(out);
    return true;
}

// ---------------------------------------------------------------------------
// Behaviour 4 — sub_40ABED, drop a bomb next to an enemy (docs/re/ai.md §3.4).
// Byte-exact (0x40ABED). Structure:
//   1. capacity guard: sub_4245DA(me) — my live-bomb count — must be < my
//      maxBombs(+86), the same disasm-confirmed spare-slot gate as behaviour 3
//      (§9.3 RESOLVED);
//   2. a Manhattan gate on the actor's SNAPPED position (+20/+24): abs(tileX) +
//      abs(tileY) >= 3. In the original +20/+24 is a stale spawn/punch snapshot
//      (set to the current position only at spawn/punch, NOT during walking), so
//      this is a near-constant TRUE per player — abs-sum of a spawn tile is >= 3
//      for every real Bomberman start except a hypothetical (1,1) corner. We do
//      not track that snapshot field; the faithful determinable analog is the
//      AI's current tile, which EQUALS the spawn snapshot at match start and
//      gives the same near-always-true result (docs/re/ai.md §3.4 [VERIFY]);
//   3. scan the 5-tile plus/cross (kEnemyScanX/Y, the OOB tables) for a live
//      (active +0, not-dead +8) ENEMY player (sub_421CB5 `*i && !i[2]`, pseudo.c
//      24207 — `!i[2]` is +8/died-this-round, NOT the +58 stun), self excluded
//      (the original zeroes its own actor +0 across the probe; we skip the self);
//   4. per hit: in team mode skip a teammate (dword_464964 gate; same_team()
//      below — see docs/re/ai.md §3.4/§5.3, "our semantics" note at same_team's
//      definition for what counts as a team on an all-zero roster); the
//      standing tile must be clear to drop on (sub_423188); then drop on a 1-in-5
//      whim (rand()%5==0).
// It does NOT flee here — behaviour 2 (higher priority) paths the AI out of the
// new blast next tick. RNG (§8 row b4): the rand()%5 fires ONLY once a live enemy
// is found on the cross AND the clearance gate passes; otherwise no draw.
// ---------------------------------------------------------------------------
bool AISystem::behave_bomb_enemy(int i, PlayerInput& out) {
    Player& p = s_.players[i];
    const int px = p.tile_x(), py = p.tile_y();

    // (1) Capacity guard (sub_4245DA(me) >= maxBombs(+86) -> return 0; §9.3
    // RESOLVED), matching behaviour 3.
    if (out_of_bomb_slots(p)) return false;

    // (2) Manhattan gate: abs(tileX) + abs(tileY) >= 3 over the actor's snapped
    // position. See the header note — we use the current tile as the faithful
    // analog of the stale +20/+24 snapshot (equal at spawn, near-always true).
    const int mx = px < 0 ? -px : px;
    const int my = py < 0 ? -py : py;
    if (mx + my < 3) return false;

    // (3) Scan the 5-tile cross for a live enemy (sub_421CB5), self excluded.
    for (int k = 0; k < 5; ++k) {
        const int tx = px + kEnemyScanX[k], ty = py + kEnemyScanY[k];
        if (!grid::in_grid(tx, ty)) continue;
        int who = -1;
        for (int j = 0; j < kMaxPlayers; ++j) {
            const Player& q = s_.players[j];
            if (j == i) continue;  // self excluded (the original's +0-zeroing trick)
            // sub_421CB5 checks +0 (active) && !+8 (not dead); our present && alive.
            // NO +58/stun check — a stunned-but-alive enemy is still bombable.
            if (q.present && q.alive && q.tile_x() == tx && q.tile_y() == ty) {
                who = j;
                break;  // sub_421CB5 returns the first live (active, not-dead) player here
            }
        }
        if (who < 0) continue;

        // (4) Team gate (dword_464964 && me.team == cell.team -> skip; §3.4). A
        // same-team hit is not an enemy: the ORIGINAL's `return 0` here ends the
        // whole behaviour (it does NOT continue scanning the rest of the cross),
        // so we mirror that exactly. On an all-zero roster same_team() is always
        // false, so this never fires there (byte-identical to before). Then the
        // standing tile must be clear to drop on (sub_423188), and the 1-in-5 whim.
        if (same_team(i, who)) return false;
        if (!drop_tile_clear(px, py)) return false;  // matches the original's return 0
        if (random_below(s_, 5) != 0) return false;  // rand()%5 != 0 -> pass
        press_bomb(out);  // bomb-key edge -> BombSystem::drop in player_turn
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// A same-team player is not an enemy (docs/re/ai.md §5.3: "in team mode only
// if its team +84 differs"; the +84 wiring itself is the documented follow-up
// this change lands). Our semantics (not RE'd beyond the byte's existence,
// see Player::team): two ACTIVE players are teammates when their team values
// are equal, INCLUDING both-zero — so a fully-zeroed roster (every existing
// scenario) has i.team==j.team for every pair and would wrongly call every
// player a teammate of every other. To keep the untamed (all-zero) path
// byte-identical to "team mode off", we special-case team==0 as "no team" on
// both sides: zero never matches zero. Only a nonzero, shared value is a team.
bool AISystem::same_team(int a, int b) const {
    const std::uint8_t ta = s_.players[a].team, tb = s_.players[b].team;
    return ta != 0 && ta == tb;
}

// Enemy finder — sub_422718 (docs/re/ai.md §5.3). Picks a live opponent to
// pursue, starting the scan at a random slot so targeting is random (not
// nearest). Two passes, byte-exact:
//   pass 1: start = rand()%10, scan 10 slots forward (wrapping); accept the
//           first that is NOT self, present (+16 != 0), NOT another AI/computer
//           (+16 != 1), active (+0), NOT DEAD (+8, `v7[2]` in pseudo.c 24741 —
//           NOT the +58 stun), and (team mode) not a teammate. This pass draws
//           ONE rand()%10 for its start index.
//   pass 2 (only if pass 1 finds nothing): start = a SECOND rand()%10; the same
//           scan but RELAXED to include other AI players (drops the +16 != 1
//           test). Returns the first live, non-teammate opponent, else -1.
// Returns the chosen slot index, or -1 if no live opponent exists. The team
// filter (same_team above) reduces to "never true" on an all-zero roster, so
// this is exactly "slot != self" there, byte-identical to before Player::team
// existed. Both rand()%10 draws are part of the RNG contract regardless of
// whether a hit is found (they sit inside behaviour 6's acquire, between its
// rand()%50 and the timer/BFS steps — §8 lists only the outer draws), so the
// team filter changes WHO is picked, never HOW MANY draws happen.
// ---------------------------------------------------------------------------
int AISystem::pick_live_enemy(int self) {
    // Pass 1: prefer a live human opponent (skip other AI, +16==1 -> ai==true).
    const int start1 = static_cast<int>(random_below(s_, 10));
    for (int n = 0; n < kMaxPlayers; ++n) {
        const int j = (start1 + n) % kMaxPlayers;
        if (j == self) continue;  // a1 == v7 (self)
        const Player& q = s_.players[j];
        if (!q.present) continue;          // !+16 (absent)
        if (q.ai) continue;                // +16 == 1 (another computer player)
        if (!q.alive) continue;            // !+0 (inactive) / v7[2] (+8 dead) — NOT +58 stun
        if (same_team(self, j)) continue;  // team mode: skip a teammate
        return j;  // the first live, non-teammate human opponent (slot != self)
    }
    // Pass 2: fall back to ANY live opponent (incl. other AI) — the relaxed scan.
    const int start2 = static_cast<int>(random_below(s_, 10));
    for (int n = 0; n < kMaxPlayers; ++n) {
        const int j = (start2 + n) % kMaxPlayers;
        if (j == self) continue;  // a1 == v8 (self)
        const Player& q = s_.players[j];
        if (!q.present) continue;          // !+16 (absent)
        if (!q.alive) continue;            // !+0 (inactive) / v8[2] (+8 dead) — NOT +58 stun
        if (same_team(self, j)) continue;  // team mode: skip a teammate
        return j;                          // any live, non-teammate opponent
    }
    return -1;  // no live opponent
}

// ---------------------------------------------------------------------------
// Behaviour 6 — sub_40B8C2, seek an enemy (docs/re/ai.md §3.6). Structurally the
// twin of behaviour 5 but targets a PLAYER (via pick_live_enemy / sub_422718),
// using the enemy-seek field trio and maxdist 20:
//   - acquire: only while NOT already latched, on a 1/50 whim -> pick a live
//     opponent, store its SLOT + reset the ~10-tick timer + set the latch;
//   - timeout: tick the timer; once it reaches 10, give up on a 1/50 roll (UNLIKE
//     powerup-seek, whose timeout is unconditional — this one keeps the target if
//     the roll fails and the timer keeps growing);
//   - liveness: drop the target if its slot is now gone / inactive / dead
//     (original: `!v4 || *v4 != 1 || v4[2]` — v4[2] is +8/died-this-round =
//     our !alive, NOT the +58 stun, so a stunned-but-alive foe is still chased);
//   - path: directed BFS toward the target's tile (maxdist 20); if unreachable
//     (0 iters) give up 50% of the time BEFORE the firstdir check; then step one
//     tile if the next tile is safe (sub_40A59D), else hold.
// It does NOT drop bombs — the bombing of a cornered foe is behaviour 4, above it
// in priority. RNG order within this behaviour (§8 rows b6a..b6d + the inner
// pick_live_enemy draws): %50 acquire -> [pick_live_enemy rand()%10 (+ a 2nd
// %10 if pass 1 is empty) when acquiring] -> [%50 give-up if timed out] -> BFS
// tie-break -> [%2 give-up if unreachable]. Returns true if it acted.
// ---------------------------------------------------------------------------
bool AISystem::behave_seek_enemy(int i, PlayerInput& out) {
    Player& p = s_.players[i];
    Brain& br = s_.brains[i];
    const int px = p.tile_x(), py = p.tile_y();

    // Acquire: only when not already pursuing, on a 1/50 whim. The && short-
    // circuits so the %50 draw is taken ONLY when !active (matches the original).
    if (!br.enemy_seek.active && random_below(s_, 50) == 0) {
        const int slot = pick_live_enemy(i);  // sub_422718 (its own rand()%10 draws)
        if (slot >= 0) {
            br.enemy_seek.active = true;
            br.enemy_seek.timer = 0;
            br.enemy_seek.target_slot = static_cast<std::int8_t>(slot);
        }
    }

    if (!br.enemy_seek.active) return false;  // no target: pass down to behaviour 7

    // Timeout: accrue the frame delta, then at >= 500 ms give up on a 1/50
    // roll. The %50 is evaluated ONLY when the timer condition holds
    // (short-circuit &&), exactly as the original orders
    // `(10*msPerFrame <= +12) && !(rand()%50)` — +12 accrues frameDelta per
    // displayed frame, so the threshold is 10 × 50 ms of wall clock.
    br.enemy_seek.timer += delta_ms_;
    if (br.enemy_seek.timer >= 10 * kMsPerTick && random_below(s_, 50) == 0) {
        br.enemy_seek.active = false;
        return false;
    }

    // Liveness: the original reloads the actor pointer and drops the target if it
    // is null / not the alive value / DEAD (`!v4 || *v4 != 1 || v4[2]` — v4[2] is
    // +8/died-this-round = our !alive, NOT the +58 stun; a stunned-but-alive foe
    // is still pursued). Our slot image: give up if the slot is no longer a live
    // player.
    // bugprone-signed-char-misuse (NOLINT below) — target_slot (std::int8_t)
    // is a genuine signed small int; the negative-slot check right below
    // relies on its sign, so casting through unsigned char first would break it.
    const int ts = br.enemy_seek.target_slot;  // NOLINT(bugprone-signed-char-misuse)
    if (ts < 0 || ts >= kMaxPlayers) {
        br.enemy_seek.active = false;
        return false;
    }
    const Player& target = s_.players[ts];
    if (!target.present || !target.alive) {
        br.enemy_seek.active = false;
        return false;
    }

    // Path toward the target's tile (maxdist = 20, the original's literal). One
    // BFS tie-break draw. If unreachable (0 iters) give up 50% of the time BEFORE
    // the firstdir check, exactly as the original orders it (line 10914).
    int iters = 0;
    const int first = directed_bfs(px, py, target.tile_x(), target.tile_y(), 20, iters);
    if (iters == 0 && random_below(s_, 2) != 0) {
        br.enemy_seek.active = false;
    }
    if (first < 0) {
        br.enemy_seek.active = false;
        return false;
    }

    // Take the step, but only if the next tile is safe to stand on (sub_40A59D);
    // otherwise hold (godir -1). The original records the step dir at +20 first.
    br.enemy_seek.step_dir = static_cast<std::int8_t>(first);
    const int nx = px + kDX[first & 3], ny = py + kDY[first & 3];
    write_move(out, safe_tile(nx, ny) ? first : -1);
    return true;  // acted
}

// ---------------------------------------------------------------------------
// Behaviour 7 — sub_40A81F, wander fallback (docs/re/ai.md §3.7). RNG order:
// rand()%25 (pick a new turn?) -> rand()%2 (which +-90 turn) -> [step] ->
// rand()%4 (re-roll when blocked). Returns true if it acted (stepped).
// ---------------------------------------------------------------------------
bool AISystem::behave_wander(int i, PlayerInput& out) {
    Player& p = s_.players[i];
    Brain& br = s_.brains[i];
    const int px = p.tile_x(), py = p.tile_y();

    if (random_below(s_, 25) == 0) {
        // Turn +-90 off the current wander dir. The original bases this on a
        // stored byte (+62 BYTE2); with personality 0 that seed is 0, so the
        // base is the current wander dir. v2 = (base + 2*(rand%2) - 1) & 3.
        const int turn = (br.wander_dir + 2 * static_cast<int>(random_below(s_, 2)) - 1) & 3;
        // Adopt the new turn only if the CURRENT wander dir is itself safe to
        // step (mirrors the original's guard before overwriting wander_dir).
        if (safe_tile(px + kDX[br.wander_dir & 3], py + kDY[br.wander_dir & 3]))
            br.wander_dir = static_cast<std::int8_t>(turn);
    }

    const int wg = br.wander_dir & 3;
    if (safe_tile(px + kDX[wg], py + kDY[wg])) {
        write_move(out, wg);
        return true;  // step that way
    }
    // Blocked: re-roll the wander dir and pass down (the fallback of the
    // fallback — with nothing below, the AI simply holds still this tick).
    br.wander_dir = static_cast<std::int8_t>(random_below(s_, 4));
    return false;
}

void AISystem::write_move(PlayerInput& out, int godir) {
    out.up = out.down = out.left = out.right = false;
    if (godir < 0) return;
    switch (kGodirDir[godir & 3]) {
        case Direction::Up: out.up = true; break;
        case Direction::Right: out.right = true; break;
        case Direction::Down: out.down = true; break;
        case Direction::Left: out.left = true; break;
    }
}

// The bomb-key edge (+56=1; +54=0 in the original) -> action1. player_turn's drop
// block is edge-gated on action1 && !prev_action1; the AI never sets prev_action1
// itself, so a single-tick action1=true is a fresh press (docs/re/ai.md §7).
void AISystem::press_bomb(PlayerInput& out) {
    out.action1 = true;
}

// The action-key edge (+57=1; +55=0 in the original) -> action2. player_turn's
// action block is edge-gated on action2 && !prev_action2, so a single-tick
// action2=true is a fresh press routed to punch (+91) / trigger (+95). The AI
// never sets prev_action2 itself (docs/re/ai.md §7).
void AISystem::press_action(PlayerInput& out) {
    out.action2 = true;
}

// ---------------------------------------------------------------------------
// Dispatcher — sub_40A1C6 (docs/re/ai.md §2). Draw A (leading scratch), the
// behaviour chain (first behaviour that acts short-circuits), Draw B (trailing
// scratch). Draws A/B are heap-debug residue kept for exact RNG parity: a bare
// rand() advances the stream with the value discarded, which next_random models.
//
// DISPATCHER ORDER (ADR-0005 §8): ALL 8 behaviours are now live (Stage 5 landed
// behaviours 1/4/6 + the safe-branch trigger whim). Draws land in the §8 slots:
// A (scratch) -> the single fired behaviour's draws -> B (scratch). Because
// golden has no AI players, no draw stream regresses (ADR-0005 §7). Behaviours:
// [0] grab-glove (%2 when eligible), [1] punch (%4 when holding punch), [2]
// walk/flee (BFS tie-break; safe-branch %10 trigger whim), [3] blast bricks
// (%915 when the gates pass), [4] bomb-near-enemy (%5 when a foe is on the cross
// and the tile is clear), [5] seek powerup (%50-acquire / scan / path / %2), [6]
// seek enemy (%50-acquire + the finder's %10 draws / %50-timeout / path / %2),
// [7] wander. Only ONE behaviour body runs per tick (short-circuit).
// ---------------------------------------------------------------------------
void AISystem::decide(int i, PlayerInput& out, std::int32_t delta_ms) {
    delta_ms_ = delta_ms;  // this frame's ms delta — the pursuit timers accrue it
    ensure_grids();

    // Draw A — leading scratch alloc (sub_40A1C6 line 10359). Unconditional.
    (void)next_random(s_);

    // The behaviour chain, in priority order (off_45BA78[8]). The first behaviour
    // that acts short-circuits the rest; its draws (in the fixed intra-behaviour
    // order) are the only behaviour draws taken this tick.
    bool acted = false;
    if (!acted) acted = behave_grab_drop(i, out);     // [0] sub_40BD44 grab-glove
    if (!acted) acted = behave_punch(i, out);         // [1] sub_40BE02 punch a bomb ahead
    if (!acted) acted = behave_walk_path(i, out);     // [2] sub_40B20F walk/flee
    if (!acted) acted = behave_blast_bricks(i, out);  // [3] sub_40AD8D blast bricks
    if (!acted) acted = behave_bomb_enemy(i, out);    // [4] sub_40ABED bomb near an enemy
    if (!acted) acted = behave_seek_powerup(i, out);  // [5] sub_40BAF5 seek powerup
    if (!acted) acted = behave_seek_enemy(i, out);    // [6] sub_40B8C2 seek an enemy
    if (!acted) acted = behave_wander(i, out);        // [7] sub_40A81F wander

    // If nothing acted (e.g. wander blocked and re-rolled), leave `out` with no
    // movement — the AI simply holds still this tick, as the original does when
    // the whole chain passes without a step.
    if (!acted) write_move(out, -1);

    // Draw B — trailing scratch alloc (sub_40A1C6 line 10412). Unconditional.
    (void)next_random(s_);
}

}  // namespace bomber::sim
