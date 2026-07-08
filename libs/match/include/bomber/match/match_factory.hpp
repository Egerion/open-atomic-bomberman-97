#pragma once

// Anti-corruption layer between the asset formats and the simulation: builds
// a sim::MatchConfig from a parsed .SCH scheme and (optionally) a parsed
// VALUELST.RES, and picks stages from the enabled rotation. SDL-free.

#include <algorithm>
#include <cstdint>
#include <vector>

#include "bomber/assets/extra.hpp"
#include "bomber/assets/reslist.hpp"
#include "bomber/assets/sch.hpp"
#include "bomber/sim/match_config.hpp"

namespace bomber::match {

inline sim::MatchConfig build_match_config(const assets::sch::Scheme& scheme, int player_count,
                                           std::uint32_t seed,
                                           const assets::res::ValueList* values = nullptr,
                                           bool random_start = false) {
    sim::MatchConfig cfg;
    cfg.player_count = player_count;
    cfg.seed = seed;

    if (values)
        for (const auto& [id, v] : values->values) cfg.tuning.apply(id, v);

    // Per-match destructible-brick fill (sub_4260F5, pseudo.c ~26928): the .SCH
    // ':' cells are only brick *candidates*. The original walks the board
    // ROW-MAJOR (y outer, x inner) and for each brick candidate draws
    // `rand() % 100`; if that is `>= brick_density` the candidate is knocked
    // back to blank — so on average `brick_density`% of the ':' cells become
    // real bricks, a fresh random layout every match. `#` (solid) and `.`
    // (blank) cells are copied verbatim and consume NO draw (the original's
    // `v3 == 2 && rand()...` short-circuits). Density is the scheme's `-B`
    // value clamped to [0,100] (dword_4647A0). BASIC.SCH ships `-B,90`.
    //
    // Determinism: the original seeds rand() off the wall clock, so its layout
    // is not reproducible run-to-run. We instead drive the fill from a
    // SETUP-ONLY LCG seeded off the match seed — NEVER the sim's per-tick
    // State::rng — exactly like apply_actors resolves '-T,H' trampolines. This
    // keeps the per-tick RNG draw contract untouched (the fill is pre-sim,
    // producing the static `cells` grid the sim then treats as a fixed input),
    // while identical seeds still reproduce identical boards. The regression
    // this fixes: the fill was skipped entirely (every ':' became a brick at
    // 100%), so every match on a scheme had the SAME fixed brick layout.
    const int density = std::clamp(scheme.brick_density, 0, 100);
    std::uint32_t brick_lcg = seed ^ 0x9E3779B9u;  // decorrelated from apply_actors' stream
    auto brick_roll = [&brick_lcg]() {
        brick_lcg = brick_lcg * 1664525u + 1013904223u;
        return (brick_lcg >> 16) % 100u;  // mirrors rand() % 100
    };
    for (int y = 0; y < sim::kGridHeight && y < scheme.height(); ++y) {
        for (int x = 0; x < sim::kGridWidth && x < scheme.width(); ++x) {
            switch (scheme.at(x, y)) {
                case assets::sch::Cell::Solid: cfg.cells[y][x] = sim::Cell::Solid; break;
                case assets::sch::Cell::Brick:
                    // Draw ONLY for brick candidates (order + count mirror the
                    // original's row-major, per-':'-cell rand() % 100).
                    cfg.cells[y][x] = (static_cast<int>(brick_roll()) >= density)
                                          ? sim::Cell::Blank
                                          : sim::Cell::Brick;
                    break;
                default: cfg.cells[y][x] = sim::Cell::Blank; break;
            }
        }
    }

    for (const auto& sp : scheme.spawns) {
        if (sp.player >= static_cast<int>(cfg.spawns.size()))
            cfg.spawns.resize(sp.player + 1);
        cfg.spawns[sp.player] = {std::clamp(sp.x, 0, sim::kGridWidth - 1),
                                 std::clamp(sp.y, 0, sim::kGridHeight - 1)};
    }

    // "Random Start" (options.ini `random_start=`, dword_464AE8, Options
    // screen row 1 — docs/re/results-and-options.md §3). CONFIRMED shuffle
    // (docs/re/facts.md "Options toggles"): the original shuffles its two
    // 10-slot start-coordinate arrays (dword_46460C/dword_46465C) with 200
    // random pair-swaps — `a = rand() % 10; b = rand() % 10; if (a != b)
    // swap(x[a],x[b]), swap(y[a],y[b])` — at round init (sub_421793
    // pseudo.c 23996-24012; the demo-replay stepper sub_40133F 4472-4488
    // repeats it verbatim). Every player still starts at one of the scheme's
    // authored spawn points, just reassigned. Mirrored here 1:1 (the %10 is
    // the arrays' fixed size; ours is spawns.size(), 10 for a full .SCH).
    // Driven by a SETUP-ONLY LCG seeded off the match seed (decorrelated
    // from the brick-fill/actor streams above/below), never sim::State::rng
    // (the original's rand() is wall-clock seeded), so the per-tick RNG draw
    // contract is untouched and identical seeds reproduce identical starts.
    if (random_start && cfg.spawns.size() > 1) {
        std::uint32_t start_lcg = seed ^ 0x2545F491u;
        auto start_roll = [&start_lcg](std::uint32_t n) {
            start_lcg = start_lcg * 1664525u + 1013904223u;
            return (start_lcg >> 16) % n;
        };
        const auto n = static_cast<std::uint32_t>(cfg.spawns.size());
        for (int i = 0; i < 200; ++i) {
            std::uint32_t a = start_roll(n);
            std::uint32_t b = start_roll(n);
            if (a != b) std::swap(cfg.spawns[a], cfg.spawns[b]);
        }
    }

    for (const auto& pr : scheme.powerups) {
        if (pr.id < 0 || pr.id >= sim::kPowerupKinds) continue;
        if (pr.forbidden) cfg.forbidden[pr.id] = true;
        if (pr.born_with) cfg.born_with[pr.id] = true;
        if (pr.has_override) cfg.spawn_override[pr.id] = pr.override_value;
    }
    return cfg;
}

// Overlays parsed EXTRA<N>.RES stage actors onto a MatchConfig
// (docs/re/stage-actors.md). The layout is a static, hashed sim input like the
// cell grid. Random '-T,H' trampolines are resolved HERE with a setup-only LCG
// seeded off the match seed, NOT the sim's per-tick RNG — so the determinism
// contract (RNG draw order/count per tick) is untouched, while identical seeds
// still reproduce identical boards. Placement mirrors the original's odd-parity,
// no-overlap rule for '-T,H'.
inline void apply_actors(sim::MatchConfig& cfg, const std::vector<assets::extra::Actor>& actors,
                         std::uint32_t seed) {
    using assets::extra::Kind;
    auto occupied = [&](int x, int y) {
        return cfg.actor_type[y][x] != sim::ActorType::None;
    };
    auto place = [&](int x, int y, sim::ActorType t, int dir) {
        if (x < 0 || x >= sim::kGridWidth || y < 0 || y >= sim::kGridHeight) return;
        cfg.actor_type[y][x] = t;
        cfg.actor_dir[y][x] = static_cast<std::uint8_t>(dir & 3);
        // An actor sits on OPEN floor. The original clears the tile as it drops
        // the actor — the warphole init explicitly runs sub_425E9B(x,y,0)
        // (pseudo.c case 1), and every actor type must be walkable to work. A
        // brick/solid left under an actor (from the scheme or the brick fill,
        // which run BEFORE this overlay) renders it as a "closed" rock and
        // blocks access — the warphole-looks-closed bug. Clear the cell so the
        // conveyor/arrow/warphole/trampoline is both visible and walkable.
        cfg.cells[y][x] = sim::Cell::Blank;
    };
    std::uint32_t lcg = seed ? seed : 0x1234567u;  // setup-only stream
    auto roll = [&]() { lcg = lcg * 1664525u + 1013904223u; return lcg >> 16; };

    // Warphole one-time setup knockout (sub_4056CA case 1, the `if (!+146)`
    // block): when a warphole is first activated it clears its OWN tile
    // (sub_425E9B(x,y,0) — place() already does this) AND then clears ONE
    // RANDOM ADJACENT tile too. The original loop is
    //     do { do { d = rand()%4; nx = dx[d]+x; ny = dy[d]+y; }
    //          while (nx < 0); } while (nx >= W || ny < 0 || ny >= H);
    //     sub_425E9B(nx, ny, 0);
    // i.e. re-roll a cardinal direction until the neighbour is in-bounds, then
    // set that tile to Blank unconditionally (brick OR solid, whatever sat
    // there). dx/dy are the cos/sin dir tables {0,1,0,-1}/{-1,0,1,0}; both are
    // never 0 together so the centre is never picked — no explicit skip needed.
    // One knockout per warphole (the +146 latch). Driven by the setup-only LCG
    // (like the '-T,H' trampoline placement), NEVER State::rng, so the per-tick
    // RNG contract is untouched and boards with no warpholes are unaffected.
    static constexpr int kDx[4] = {0, 1, 0, -1};  // dword_45BECC
    static constexpr int kDy[4] = {-1, 0, 1, 0};  // dword_45BEDC
    auto knockout_neighbour = [&](int x, int y) {
        int nx = 0, ny = 0;
        do {
            int d = static_cast<int>(roll() % 4);  // one draw per attempt
            nx = kDx[d] + x;
            ny = kDy[d] + y;
        } while (nx < 0 || nx >= sim::kGridWidth || ny < 0 || ny >= sim::kGridHeight);
        cfg.cells[ny][nx] = sim::Cell::Blank;  // sub_425E9B(nx, ny, 0)
    };

    // Track placed warpholes so their idno/linkto links can be resolved into
    // destination tiles after all actors are down (mirrors sub_405A81, which
    // scans the whole registry). Kept as (x, y, idno, linkto).
    struct Warp { int x, y, idno, linkto; };
    std::vector<Warp> warps;

    for (const auto& a : actors) {
        switch (a.kind) {
            case Kind::Conveyor: place(a.x, a.y, sim::ActorType::Conveyor, a.dir); break;
            case Kind::DirArrow: place(a.x, a.y, sim::ActorType::DirArrow, a.dir); break;
            case Kind::Warphole:
                place(a.x, a.y, sim::ActorType::Warphole, 0);
                if (a.x >= 0 && a.x < sim::kGridWidth && a.y >= 0 && a.y < sim::kGridHeight) {
                    warps.push_back({a.x, a.y, a.idno, a.linkto});
                    knockout_neighbour(a.x, a.y);  // clear one random neighbour
                }
                break;
            case Kind::Trampoline:
                if (a.random) {
                    // Odd-parity ((x+y) odd), un-occupied open tile, <=100 tries.
                    for (int i = 0; i < 100; ++i) {
                        int x = static_cast<int>(roll() % sim::kGridWidth);
                        int y = static_cast<int>(roll() % sim::kGridHeight);
                        if (((x + y) & 1) && !occupied(x, y) &&
                            cfg.cells[y][x] == sim::Cell::Blank) {
                            place(x, y, sim::ActorType::Trampoline, 0);
                            break;
                        }
                    }
                } else {
                    place(a.x, a.y, sim::ActorType::Trampoline, 0);
                }
                break;
        }
    }

    // Resolve warphole exits (sub_405A81): each warphole links to the FIRST
    // other warphole whose idno equals this one's linkto; no partner ⇒ the exit
    // is its own tile (the player/bomb stays put, harmless). Deterministic,
    // no RNG — the sim reads warp_dest_* directly on a warp.
    for (const auto& w : warps) {
        int dx = w.x, dy = w.y;  // default: self (sub_405A81 with no match)
        for (const auto& other : warps) {
            if ((other.x != w.x || other.y != w.y) && other.idno == w.linkto) {
                dx = other.x;
                dy = other.y;
                break;
            }
        }
        cfg.warp_dest_x[w.y][w.x] = static_cast<std::uint8_t>(dx);
        cfg.warp_dest_y[w.y][w.x] = static_cast<std::uint8_t>(dy);
    }
}

// Picks a stage from the enabled rotation (VALUELST 1150..1160), seed-based.
inline int pick_stage(const sim::Tuning& tuning, std::uint32_t seed) {
    std::vector<int> allowed;
    for (int i = 0; i < 11; ++i)
        if (tuning.level_enabled[i]) allowed.push_back(i);
    if (allowed.empty()) allowed.push_back(0);
    return allowed[seed % allowed.size()];
}

}  // namespace bomber::match
