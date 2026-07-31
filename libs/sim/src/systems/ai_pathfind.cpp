// AI pathfinding: the three 100-node BFS wavefront variants (flee sub_40970B,
// directed sub_4092A1, powerup-scan sub_409C1F) and the step-into-flame veto
// (sub_40A76E). See ai.hpp for the subsystem API and docs/re/ai.md for the RE
// facts.
//
// All three draw the ±1 tie-break ONCE at entry, before any expansion — that
// draw is part of the RNG contract, and it flips the order equal-length branches
// expand so ties resolve randomized-but-deterministically. All three seed in
// FIXED godir order 0..3: the original's seed loops are plain ascending 0..3
// loops and the tie draw steers only the ±90° child-spawn order deeper in the
// walk. Seeding in tie-flipped order instead made equal-danger first steps flip
// ~50% per decide, an oscillation the original does not have (2026-07-16
// movement audit).

#include "systems/ai.hpp"

#include <array>

#include "bomber/sim/rng.hpp"
#include "grid.hpp"

namespace bomber::sim {

namespace {

// The MACHINERY the three wavefronts share — the original's fixed 100-node open
// list (dword_45ED68) and a visited set. Only the machinery: each variant keeps
// its own body, goal test and sub_XXXX, because that 1:1 mapping is what makes
// the port checkable against the binary.
//
// Visited is an EPOCH grid rather than a cleared bitmap: bumping a counter is an
// O(1) reset and stays deterministic. The counter is monotonic and every
// Frontier bumps it on construction, so a mark left by any earlier call — of any
// variant — can never match.
struct Frontier {
    struct Node {
        int x, y, first, depth;
    };
    std::array<Node, 100> open{};
    int n = 0;
    int head = 0;

    Frontier() { ++epoch(); }

    static std::uint64_t& epoch() {
        static thread_local std::uint64_t e = 0;
        return e;
    }

    // True the FIRST time (x,y) is seen this call. Callers must have cleared
    // obstacle_at first, which also rejects out-of-bounds.
    static bool first_visit(int x, int y) {
        static thread_local std::array<std::array<std::uint64_t, kGridWidth>, kGridHeight> seen{};
        if (seen[y][x] == epoch()) return false;
        seen[y][x] = epoch();
        return true;
    }

    // The 100-node cap is the original's: a push past it is DROPPED, not grown.
    void push(int x, int y, int first, int depth) {
        if (n < 100) open[n++] = {x, y, first, depth};
    }
    bool more() const { return head < n; }
    Node pop() { return open[head++]; }

    // The first step toward (x,y), if that tile was ever enqueued.
    int first_step_to(int x, int y) const {
        for (int k = 0; k < n; ++k)
            if (open[k].x == x && open[k].y == y) return open[k].first;
        return -1;
    }
};

// 2*(rand()%2)-1 == -1 or +1: every wavefront's single entry draw.
int draw_tie_break(State& s) {
    return 2 * static_cast<int>(random_below(s, 2)) - 1;
}

}  // namespace

// Flee BFS (sub_40970B, docs/re/ai.md §5.2). The wavefront with no goal tile:
// every reachable cell is scored by the danger grid, tracking the minimum, and
// the instant a danger-0 tile is reached that path's first step is returned;
// otherwise the frontier is exhausted and the first step toward the lowest-danger
// tile is returned. -1 means boxed in.
int AISystem::flee_bfs(int sx, int sy, int& best_x, int& best_y) {
    const int tie = draw_tie_break(s_);
    Frontier f;

    best_x = sx;
    best_y = sy;
    // The best-tracker inits at 10000 (sub_40970B 9936), NOT at the start tile's
    // own danger — so with ANY open neighbour this returns a step even when
    // nothing beats the danger the AI is standing in, and -1 essentially means
    // "fully boxed in". CORRECTED 2026-07-12 (facts.md "AI danger map" item 4):
    // the old start-danger init returned -1 whenever no strictly-safer tile
    // existed, sending the caller down the chain. One other path reaches -1 — the
    // 100-node cap drops pushes once the queue is full while best_x/best_y are
    // still updated, so on a very open board a tile can win the best-tracker
    // without ever being enqueued.
    std::int32_t best_danger = 10000;

    auto take_if_better = [&](int nx, int ny) {
        const std::int32_t dn = danger_at(nx, ny);
        if (dn >= best_danger) return false;
        best_danger = dn;
        best_x = nx;
        best_y = ny;
        return dn == 0;  // a fully-safe tile ends the search
    };

    Frontier::first_visit(sx, sy);
    for (int g = 0; g < 4; ++g) {
        const int nx = sx + grid::kDx[g], ny = sy + grid::kDy[g];
        if (obstacle_at(nx, ny)) continue;
        if (!Frontier::first_visit(nx, ny)) continue;
        f.push(nx, ny, g, 1);
        (void)take_if_better(nx, ny);
    }
    if (best_danger == 0) {
        // A neighbour is already safe. If it somehow never made the open list the
        // original falls through into the expansion rather than returning -1.
        const int step = f.first_step_to(best_x, best_y);
        if (step >= 0) return step;
    }

    while (f.more()) {
        const Frontier::Node cur = f.pop();
        // Ring cap: sub_40970B stops expanding after 20 rings (its a4 = 20,
        // pseudo.c 10048-10053) while keeping the best-so-far step, so an
        // improvement further than 20 steps away is invisible to the original's
        // flee (CORRECTED 2026-07-12, facts.md "AI danger map").
        if (cur.depth >= 20) continue;
        for (int s2 = 0; s2 < 4; ++s2) {
            const int g = tie > 0 ? s2 : (3 - s2);
            const int nx = cur.x + grid::kDx[g], ny = cur.y + grid::kDy[g];
            if (obstacle_at(nx, ny)) continue;
            if (!Frontier::first_visit(nx, ny)) continue;
            if (take_if_better(nx, ny)) return cur.first;
            f.push(nx, ny, cur.first, cur.depth + 1);
        }
    }
    return f.first_step_to(best_x, best_y);  // frontier exhausted: aim at the minimum
}

// Directed BFS (sub_4092A1, docs/re/ai.md §5.1): the same wavefront with a fixed
// goal tile. Returns the first-step godir of a shortest path to (tx,ty) within
// `max_depth` rings, else -1, and writes the ring count to out_iters.
//
// out_iters increments once per COMPLETED ring, so the distance-1 ring is
// processed while it is still 0 and a goal at distance 2 is also found with
// out_iters == 0 — matching the original's ring counter, which is not yet
// incremented when the goal is hit on the first sweep. Behaviours 5/6 gate their
// 50%-give-up draw on `iters == 0`, so that boundary is RNG-contract-relevant.
int AISystem::directed_bfs(int sx, int sy, int tx, int ty, int max_depth, int& out_iters) {
    const int tie = draw_tie_break(s_);  // drawn at entry, before the start==goal check
    out_iters = 0;
    // The original only enters the search when start and goal differ, so a
    // coincident pair skips it entirely (firstdir stays 0) — but its tie draw at
    // line 9705 has already been taken.
    if (sx == tx && sy == ty) return -1;

    Frontier f;
    Frontier::first_visit(sx, sy);
    for (int g = 0; g < 4; ++g) {
        const int nx = sx + grid::kDx[g], ny = sy + grid::kDy[g];
        if (obstacle_at(nx, ny)) continue;
        if (nx == tx && ny == ty) return g;  // a neighbour IS the goal: a "pass 0" hit
        if (!Frontier::first_visit(nx, ny)) continue;
        f.push(nx, ny, g, 0);
    }

    int ring_end = f.n;
    while (f.more()) {
        if (f.head == ring_end) {
            if (++out_iters > max_depth) break;  // ring counter past the depth cap
            ring_end = f.n;
        }
        const Frontier::Node cur = f.pop();
        for (int s2 = 0; s2 < 4; ++s2) {
            const int g = tie > 0 ? s2 : (3 - s2);
            const int nx = cur.x + grid::kDx[g], ny = cur.y + grid::kDy[g];
            if (obstacle_at(nx, ny)) continue;
            if (nx == tx && ny == ty) return cur.first;
            if (!Frontier::first_visit(nx, ny)) continue;
            f.push(nx, ny, cur.first, 0);
        }
    }
    // Boxed in — zero open neighbours were ever seeded, so the loop above never
    // ran. The original's do..while ALWAYS completes one pass before testing its
    // condition, so its ring counter reads 1 even when nothing was found (pseudo.c
    // 9705-9821). Behaviours 5/6 gate their unreachable-target give-up draw on
    // `iters == 0`, so leaving this at 0 would draw a spurious extra rand()%2
    // (RESOLVED, ai.md §5.1/§9). It is a no-op whenever any neighbour WAS seeded,
    // and the coincident start/goal early-out above deliberately returns before
    // reaching here, leaving out_iters at 0.
    if (out_iters == 0) out_iters = 1;
    return -1;
}

// Powerup scan BFS (sub_409C1F, docs/re/ai.md §5.5): the same wavefront again,
// but the goal test is "a floor powerup lies on this tile" (sub_42542D != 0).
// Writes the winner's tile to (found_x, found_y) and the ring count to out_iters;
// -1 if none within max_depth.
//
// The original seeds only NEIGHBOURS and never tests the start tile, so a powerup
// UNDERFOOT is not found by the scan — the AI is already standing on it and the
// Powerups system collects it.
int AISystem::powerup_scan_bfs(int sx, int sy, int max_depth, int& out_iters, int& found_x,
                               int& found_y) {
    const int tie = draw_tie_break(s_);
    out_iters = 0;
    found_x = -1;
    found_y = -1;

    auto is_powerup = [&](int x, int y) {
        return grid::in_grid(x, y) && s_.floor[y][x] != PowerupType::None;
    };

    Frontier f;
    Frontier::first_visit(sx, sy);
    for (int g = 0; g < 4; ++g) {
        const int nx = sx + grid::kDx[g], ny = sy + grid::kDy[g];
        if (obstacle_at(nx, ny)) continue;
        if (is_powerup(nx, ny)) {
            found_x = nx;
            found_y = ny;
            return g;  // a "pass 0" hit, so out_iters stays 0
        }
        if (!Frontier::first_visit(nx, ny)) continue;
        f.push(nx, ny, g, 0);
    }

    int ring_end = f.n;
    while (f.more()) {
        if (f.head == ring_end) {
            if (++out_iters > max_depth) break;
            ring_end = f.n;
        }
        const Frontier::Node cur = f.pop();
        for (int s2 = 0; s2 < 4; ++s2) {
            const int g = tie > 0 ? s2 : (3 - s2);
            const int nx = cur.x + grid::kDx[g], ny = cur.y + grid::kDy[g];
            if (obstacle_at(nx, ny)) continue;
            if (is_powerup(nx, ny)) {
                found_x = nx;
                found_y = ny;
                return cur.first;
            }
            if (!Frontier::first_visit(nx, ny)) continue;
            f.push(nx, ny, cur.first, 0);
        }
    }
    // The same boxed-in normalisation as directed_bfs, kept for structural parity
    // with sub_4092A1's do..while though currently unobservable here: behaviour 5
    // reads this iters only through `range+1 >= iters` gated on a FOUND cell, and
    // a boxed-in scan finds none.
    if (out_iters == 0) out_iters = 1;
    return -1;
}

// sub_40A76E: the step-into-flame veto. If the tile one step along godir `g` from
// (tx,ty) is on fire, cancel the step and clear the AI state, so the AI never
// voluntarily walks into flame even when the flee said to.
int AISystem::flame_veto(int i, int tx, int ty, int g) {
    if (g < 0) return g;
    const int nx = tx + grid::kDx[g], ny = ty + grid::kDy[g];
    if (!grid::in_grid(nx, ny) || s_.flame[ny][nx] == 0) return g;
    s_.brains[i].state_flag = 0;
    return -1;
}

}  // namespace bomber::sim
