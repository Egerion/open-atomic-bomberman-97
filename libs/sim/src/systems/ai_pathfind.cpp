// AI pathfinding: the three 100-node BFS wavefront variants (flee sub_40970B,
// directed sub_4092A1, powerup-scan sub_409C1F) and the step-into-flame veto
// (sub_40A76E). Split out of ai.cpp as a pure file-split (no behaviour change);
// see ai.hpp for the subsystem API and docs/re/ai.md for the RE facts.

#include "systems/ai.hpp"

#include <array>

#include "bomber/sim/rng.hpp"
#include "grid.hpp"

namespace bomber::sim {

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
    std::array<Node, 100> open{};
    int open_n = 0;

    static thread_local std::array<std::array<std::uint64_t, kGridWidth>, kGridHeight>
        visited_epoch{};
    static thread_local std::uint64_t epoch = 0;
    ++epoch;

    best_x = sx;
    best_y = sy;
    // The best-tracker inits at 10000 (sub_40970B 9936), NOT at the start
    // tile's own danger — so with ANY open neighbour the BFS returns a step,
    // even when nothing beats the danger the AI is standing in; -1 essentially
    // means "fully boxed in" (CORRECTED 2026-07-12, facts.md "AI danger map"
    // item 4 — the old start-danger init returned -1 whenever no strictly-safer
    // tile existed, sending the caller down the chain). One other path reaches
    // -1: the frontier cap below (`open_n < 100`) drops pushes once the queue is
    // full, and best_x/best_y are written BEFORE that guard, so on a very open
    // board a tile can win the best-tracker without ever being enqueued.
    std::int32_t best_danger = 10000;

    // Seed with the (up to 4) open, in-bounds neighbours of the start, each
    // tagged with the godir it came from (the eventual return value). Seed
    // order is FIXED godir 0..3 — the original's runner-seed loop is a plain
    // ascending 0..3 loop over the four godirs (sub_40970B pseudo.c ~9911-9930);
    // the ±1 tie draw (our `tie`) only flips the ±90° CHILD-spawn order deeper in
    // the walk, which our flattened expansion loop below models. Seeding in
    // tie-flipped order made equal-danger first steps flip ~50% per decide —
    // an oscillation the original does not have (2026-07-16 movement audit).
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
    for (int g = 0; g < 4; ++g) consider(sx + grid::kDx[g], sy + grid::kDy[g], g);
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
            const int nx = cur.x + grid::kDx[g], ny = cur.y + grid::kDy[g];
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
// original's goal test compares each expanded node against its goal-coordinate
// arguments, which are our (tx,ty)
// (docs/re/ai.md §9.1); we test node==(tx,ty) directly. Draws the ±1 tie-break
// ONCE at entry — behaviour 2's directed step and behaviour 5's path each take
// exactly this one BFS draw (RNG contract §8). If the start already equals the
// goal, the original short-circuits before touching the frontier and returns
// firstdir 0 (no step); we mirror that (return -1, no expansion, but the draw
// is already taken, matching the original's own 2*(rand%2)-1 tie draw at the
// very top).
// ---------------------------------------------------------------------------
int AISystem::directed_bfs(int sx, int sy, int tx, int ty, int max_depth, int& out_iters) {
    // Per-call tie-break: 2*(rand()%2)-1 (docs/re/ai.md §5.1). Drawn at entry,
    // before any expansion and before the start==goal check, exactly as the
    // original takes its tie draw first (line 9705).
    const int tie = 2 * static_cast<int>(random_below(s_, 2)) - 1;
    out_iters = 0;

    // start == goal: the original only enters the search when the start and goal
    // coordinates differ, so a coincident pair skips it entirely (firstdir stays
    // 0). No path step needed — we are already there.
    if (sx == tx && sy == ty) return -1;

    struct Node {
        int x, y, first;
    };
    std::array<Node, 100> open{};
    int open_n = 0;

    static thread_local std::array<std::array<std::uint64_t, kGridWidth>, kGridHeight>
        visited_epoch{};
    static thread_local std::uint64_t epoch = 0;
    ++epoch;

    // Seed with the (up to 4) open, in-bounds neighbours of the start, tagged
    // with the godir they came from (the eventual return value), in FIXED
    // godir order 0..3 — the original's seed loop is a plain ascending 0..3 loop
    // over the four godirs, stamping the loop index into the queued entry's
    // first-step slot (sub_4092A1 pseudo.c 9721-9740); the ±1 tie
    // draw only flips ±90° child-spawn order deeper (kept in the expansion
    // loop below). See flee_bfs's seed comment (2026-07-16 movement audit).
    auto seed = [&](int nx, int ny, int first) -> int {
        if (obstacle_at(nx, ny)) return 0;
        if (nx == tx && ny == ty) return first + 1;  // neighbour IS the goal
        if (visited_epoch[ny][nx] == epoch) return 0;
        visited_epoch[ny][nx] = epoch;
        if (open_n < 100) open[open_n++] = {nx, ny, first};
        return 0;
    };
    visited_epoch[sy][sx] = epoch;
    for (int g = 0; g < 4; ++g) {
        const int hit = seed(sx + grid::kDx[g], sy + grid::kDy[g], g);
        if (hit) {
            // A neighbour IS the goal: found in "pass 0", so out_iters == 0 — this
            // matches the original's ring counter, which is not yet
            // incremented when the goal is hit on the first sweep. Behaviour 5
            // gates its 50%-give-up draw on `!iters`, so the iters==0 boundary
            // (goal ≤2 tiles away) must match the original exactly (RNG contract).
            out_iters = 0;
            return hit - 1;
        }
    }

    // Breadth-first expansion, propagating each node's first-step tag. out_iters
    // increments once per completed ring (the original's ring counter, capped
    // at its own max-depth argument). While the distance-1 ring is processed out_iters stays
    // 0, so a goal at distance 2 is also found with out_iters == 0 — matching the
    // original (see the iters==0 note above). Stop the instant the goal is hit.
    int head = 0;
    int ring_end = open_n;
    while (head < open_n) {
        if (head == ring_end) {
            if (++out_iters > max_depth) break;  // depth cap: ring counter past max depth
            ring_end = open_n;
        }
        const Node cur = open[head++];
        for (int s2 = 0; s2 < 4; ++s2) {
            const int g = tie > 0 ? s2 : (3 - s2);
            const int nx = cur.x + grid::kDx[g], ny = cur.y + grid::kDy[g];
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
    // condition, so its ring counter (our iters) becomes 1 even when nothing was found
    // (pseudo.c 9705-9821). NOTE this normalisation is only reached on the
    // exhaustion path: the coincident start/goal early-out above returns before
    // it and DOES leave out_iters at 0. Behaviours 5/6 gate their unreachable-target give-up
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
    std::array<Node, 100> open{};
    int open_n = 0;

    static thread_local std::array<std::array<std::uint64_t, kGridWidth>, kGridHeight>
        visited_epoch{};
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
    // Fixed 0..3 seed order, same as directed_bfs above (sub_4092A1's plain
    // seed loop; the tie draw only steers the expansion below).
    for (int g = 0; g < 4; ++g) {
        const int hit = seed(sx + grid::kDx[g], sy + grid::kDy[g], g);
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
            const int nx = cur.x + grid::kDx[g], ny = cur.y + grid::kDy[g];
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
    const int nx = tx + grid::kDx[g], ny = ty + grid::kDy[g];
    if (grid::in_grid(nx, ny) && s_.flame[ny][nx] > 0) {
        s_.brains[i].state_flag = 0;
        return -1;
    }
    return g;
}

}  // namespace bomber::sim
