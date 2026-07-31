#pragma once

// Anti-corruption layer between the asset formats and the simulation: builds
// a sim::MatchConfig from a parsed .SCH scheme and (optionally) a parsed
// VALUELST.RES, and picks stages from the enabled rotation. SDL-free.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>  // std::swap — was reached through a transitive include
#include <vector>

#include "bomber/assets/extra.hpp"
#include "bomber/assets/reslist.hpp"
#include "bomber/assets/sch.hpp"
#include "bomber/match/level_registry.hpp"
#include "bomber/sim/match_config.hpp"

namespace bomber::match {

// Per-slot TEAM roster a scheme asks for (docs/re/facts.md "The .SCH -S row's
// 4th field is the per-slot TEAM"). Reproduces sub_403EEE's two-step exactly:
//
//   1. sub_4049C0 — called by the reader BEFORE it opens the file — seeds
//      every one of the 10 start records with the low bit of its slot index,
//      so the standing default is the alternating 0,1,0,1,… pattern, NOT a
//      flat 0.
//   2. the reader's 'S' case overwrites that dword for a slot whose row
//      carries a 4th field, storing it as a boolean.
//
// The reader's tail loop then pushes each slot's value into the player
// record's +84 team byte (sub_422437). In the port that byte is the setup
// screen's own per-slot team, which the user may still flip with 'T'
// afterwards — the original's precedence, since sub_410F81 loads the scheme
// before it enters its key loop.
template <std::size_t N>
constexpr void scheme_setup_teams(const assets::sch::Scheme& scheme, std::array<int, N>& team) {
    for (std::size_t i = 0; i < N; ++i) team[i] = static_cast<int>(i) & 1;  // sub_4049C0
    for (const auto& sp : scheme.spawns)
        if (sp.has_team && sp.player >= 0 && static_cast<std::size_t>(sp.player) < N)
            team[static_cast<std::size_t>(sp.player)] = sp.team != 0 ? 1 : 0;
}

// The stages of build_match_config, in the order it runs them. They are split
// out rather than inlined because each carries its own RE citation and its own
// RNG stream, and a reader chasing one of those should not have to walk the
// other four. The ORDER of the calls, and the draw order inside each, is the
// contract — see the determinism rules in CLAUDE.md.
namespace detail {

// Per-match destructible-brick fill (sub_4260F5, pseudo.c ~26928): the .SCH
// ':' cells are only brick *candidates*. The original walks the board
// ROW-MAJOR (y outer, x inner) and for each brick candidate draws
// `rand() % 100`; if that is `>= brick_density` the candidate is knocked
// back to blank — so on average `brick_density`% of the ':' cells become
// real bricks, a fresh random layout every match. `#` (solid) and `.`
// (blank) cells are copied verbatim and consume NO draw (the original tests
// "this cell is a brick candidate" FIRST and the && short-circuits before
// the draw). Density is the scheme's `-B`
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
inline void fill_bricks(sim::MatchConfig& cfg, const assets::sch::Scheme& scheme,
                        std::uint32_t seed) {
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
}

// Slot bound, BOTH ways. `sp.player` is the "-S" row's 1st field exactly as
// sch.cpp's to_int() read it, and a scheme is untrusted input like every
// other 1997 file the loaders take — more so here, since the game ships a
// scheme PICKER and a scheme EDITOR, so a hand-edited .SCH reaches this line
// by design rather than by accident. Unbounded it was an out-of-bounds
// vector write: "-S,-1,0,0" makes the `>=` resize test false (it is a SIGNED
// comparison), skips the grow, and then indexes `spawns` at size_t(-1);
// "-S,2000000000,0,0" resizes toward 16 GB instead. The upper bound is the
// original's own: sub_4049C0 seeds exactly 10 start records, so a row naming
// a slot outside [0, kMaxPlayers) has nowhere to land and is dropped — the
// same rule scheme_setup_teams applies to the SAME field.
inline void place_spawns(sim::MatchConfig& cfg, const assets::sch::Scheme& scheme) {
    for (const auto& sp : scheme.spawns) {
        if (sp.player < 0 || sp.player >= sim::kMaxPlayers) continue;
        const auto slot = static_cast<std::size_t>(sp.player);
        if (slot >= cfg.spawns.size()) cfg.spawns.resize(slot + 1);
        cfg.spawns[slot] = {std::clamp(sp.x, 0, sim::kGridWidth - 1),
                            std::clamp(sp.y, 0, sim::kGridHeight - 1)};
    }
}

// "Random Start" (options.ini `random_start=`, dword_464AE8, Options
// screen row 1 — docs/re/results-and-options.md §3). CONFIRMED shuffle
// (docs/re/facts.md "Options toggles"): the original shuffles its two
// 10-slot start-coordinate arrays (dword_46460C/dword_46465C) with 200
// random pair-swaps: each iteration draws two indices with rand() % 10 and,
// when they differ, swaps that pair in BOTH the X and the Y array — at round
// init (sub_421793
// pseudo.c 23996-24012; the demo-replay stepper sub_40133F 4472-4488
// repeats it verbatim). Every player still starts at one of the scheme's
// authored spawn points, just reassigned. Mirrored here 1:1 (the %10 is
// the arrays' fixed size; ours is spawns.size(), 10 for a full .SCH).
// Driven by a SETUP-ONLY LCG seeded off the match seed (decorrelated
// from the brick-fill/actor streams), never sim::State::rng
// (the original's rand() is wall-clock seeded), so the per-tick RNG draw
// contract is untouched and identical seeds reproduce identical starts.
inline void shuffle_spawns(sim::MatchConfig& cfg, std::uint32_t seed) {
    if (cfg.spawns.size() <= 1) return;
    std::uint32_t start_lcg = seed ^ 0x2545F491u;
    auto start_roll = [&start_lcg](std::uint32_t n) {
        start_lcg = start_lcg * 1664525u + 1013904223u;
        return (start_lcg >> 16) % n;
    };
    const auto n = static_cast<std::uint32_t>(cfg.spawns.size());
    for (int i = 0; i < 200; ++i) {
        const std::uint32_t a = start_roll(n);
        const std::uint32_t b = start_roll(n);
        if (a != b) std::swap(cfg.spawns[a], cfg.spawns[b]);
    }
}

// Per-spawn TEAM (the -S 4th field), gated by the Options team flag — see
// scheme_setup_teams for the parity default and the precedence.
inline void apply_team_roster(sim::MatchConfig& cfg, const assets::sch::Scheme& scheme) {
    std::array<int, sim::kMaxPlayers> slot_team{};
    scheme_setup_teams(scheme, slot_team);
    for (int i = 0; i < sim::kMaxPlayers; ++i)
        cfg.team[i] = static_cast<std::uint8_t>(slot_team[i] + 1);
}

// The .SCH '-P' powerup rows: forbid flags, spawn-count overrides, and the
// "born with" starting inventory.
//
// "Born with" is a COUNT that REPLACES the VALUELST starting-inventory
// baseline, not an additive one-shot grant (docs/re/facts.md "The .SCH
// -P row's 2nd field is a COUNT that REPLACES the starting
// inventory"). sub_403EEE's tail loop calls the value-table SETTER
// sub_4121BF(50 + kind, count) for every kind whose count is > 0, so
// the scheme overwrites the very id sub_4214BC reads when it seeds a
// fresh player's inventory byte — and the id the death-scatter /
// head-hit surplus tests read too. Writing it into Tuning::start_with
// (which build_state already applies to all 13 kinds, and
// PowerupSystem already uses as the surplus threshold) reproduces both
// readers for free.
//
// Order: this runs AFTER build_match_config's VALUELST pass, matching the
// original (the value table is loaded at boot, the scheme overwrites
// it at load). The `> 0` gate is the original's own — a row with count
// 0 cannot zero the default 1 bomb / 2 flame, it just leaves the
// baseline alone.
//
// Divergence, deliberate: sub_4121BF's write is global and nothing
// restores it, so in the original a scheme's counts leak into the next
// match. Tuning is rebuilt per match here, so each match sees only its
// own scheme.
inline void apply_powerup_rules(sim::MatchConfig& cfg, const assets::sch::Scheme& scheme) {
    for (const auto& pr : scheme.powerups) {
        if (pr.id < 0 || pr.id >= sim::kPowerupKinds) continue;
        if (pr.forbidden) cfg.forbidden[pr.id] = true;
        if (pr.has_override) cfg.spawn_override[pr.id] = pr.override_value;
        if (pr.born_with > 0) cfg.tuning.start_with[pr.id] = pr.born_with;
    }
}

}  // namespace detail

// `team_play` is the Options-screen team gate (dword_464964): the original
// writes the team byte either way and the flag decides whether anything reads
// it as a team, and sim::Player::team's own convention reserves 0 for "no
// team / solo side". So an OFF gate leaves every cfg.team[] at 0 (every player
// its own side) and an ON gate shifts the scheme's 0/1 byte up by one into the
// sim's 1/2. Defaults false, so every caller that predates this argument keeps
// an all-zero roster and is byte-identical.
inline sim::MatchConfig build_match_config(const assets::sch::Scheme& scheme, int player_count,
                                           std::uint32_t seed,
                                           const assets::res::ValueList* values = nullptr,
                                           bool random_start = false, bool team_play = false) {
    sim::MatchConfig cfg;
    cfg.player_count = player_count;
    cfg.seed = seed;

    if (values)
        for (const auto& [id, v] : values->values) cfg.tuning.apply(id, v);

    detail::fill_bricks(cfg, scheme, seed);
    detail::place_spawns(cfg, scheme);
    if (random_start) detail::shuffle_spawns(cfg, seed);
    if (team_play) detail::apply_team_roster(cfg, scheme);
    detail::apply_powerup_rules(cfg, scheme);
    return cfg;
}

// Overlays parsed EXTRA<N>.RES stage actors onto a MatchConfig
// (docs/re/stage-actors.md). The layout is a static, hashed sim input like the
// cell grid. Random '-T,H' trampolines are resolved HERE with a setup-only LCG
// seeded off the match seed, NOT the sim's per-tick RNG — so the determinism
// contract (RNG draw order/count per tick) is untouched, while identical seeds
// still reproduce identical boards. Placement mirrors the original's odd-parity,
// no-overlap rule for '-T,H'.
namespace detail {

// A placed warphole, tracked while actors go down so sub_405A81's idno/linkto
// resolution can run once they are all placed.
struct Warp {
    int x, y, idno, linkto;
};

// The grid writes and the setup-only LCG apply_actors needs, as one cheap stack
// object holding a reference to the config it mutates — the same shape libs/sim's
// systems use. It exists so the actor dispatch stays a FLAT switch: the random
// trampoline search used to sit inline inside its own `case`, putting that draw
// five levels deep (loop / switch / if / retry-loop / accept-test).
class ActorPlacer {
public:
    ActorPlacer(sim::MatchConfig& cfg, std::uint32_t seed)
        : cfg_(cfg), lcg_(seed ? seed : 0x1234567u) {}  // setup-only stream

    bool occupied(int x, int y) const { return cfg_.actor_type[y][x] != sim::ActorType::None; }

    std::uint32_t roll() {
        lcg_ = lcg_ * 1664525u + 1013904223u;
        return lcg_ >> 16;
    }

    void place(int x, int y, sim::ActorType t, int dir) {
        if (x < 0 || x >= sim::kGridWidth || y < 0 || y >= sim::kGridHeight) return;
        cfg_.actor_type[y][x] = t;
        cfg_.actor_dir[y][x] = static_cast<std::uint8_t>(dir & 3);
        // DELIBERATELY NO CELL WRITE — an actor does NOT clear the tile it sits
        // on (docs/re/facts.md "Stage actors do not clear the tile they sit
        // on"). sub_4056CA's four cases are asymmetric: only case 1 (warphole)
        // writes a cell, through sub_425E9B, and it is handled on the Warphole
        // branch of the dispatch below. Cases 0 (dirarrow), 2 (conveyor) and 3
        // (trampoline) contain no cells write at all — each only gates its
        // DRAWING on `if (!sub_425FB9(x,y))`, i.e. "draw the belt/arrow/
        // trampoline only while the tile is empty". A sweep of the whole
        // cells-writer family (sub_425E36/425E9B/425EFC/425F79, 16 call sites)
        // finds flame, the netplay tile sync, the warphole pair, the fill, tile
        // regeneration and the HURRY wall — and no actor placement.
        //
        // Blanking here was a live divergence worth up to a THIRD of a stage's
        // bricks — measured on BASIC.SCH at 4 players over 400 seeds, ANCIENT
        // EGYPT (44 dirarrows) lost 32.3 of 98.8 bricks, 32.7%, and INNER CITY
        // TRASH (32 conveyors) 28.7, 29.1% — and it opened those tiles for
        // walking from round start where the original has them brick-blocked
        // until somebody bombs them. The
        // "warphole looks closed" concern the old comment cited is the renderer's
        // job and is already handled the original's way: renderer.cpp's actor
        // pass skips a tile that is not Blank, which IS `!sub_425FB9(x,y)`.
    }

    // Warphole one-time setup knockout (sub_4056CA case 1, the block guarded by
    // the +146 latch still being clear): when a warphole is first activated it
    // clears its OWN tile (through sub_425E9B with cell value 0) AND then clears
    // ONE RANDOM ADJACENT tile too. This is the ONLY one of the four actor cases
    // that writes a cell at all — see place() above.
    //
    // The original's search is a nested rejection
    // loop: the INNER loop draws a cardinal direction with rand()%4 and offsets
    // the warphole's tile by that direction's dx/dy, repeating while the
    // resulting X is negative; the OUTER loop repeats that whole inner search
    // while the X is off the right edge or the Y is off either vertical edge.
    // The accepted neighbour is then passed to sub_425E9B with cell value 0,
    // setting that tile to Blank unconditionally (brick OR solid, whatever sat
    // there). dx/dy are the cos/sin dir tables {0,1,0,-1}/{-1,0,1,0}; both are
    // never 0 together so the centre is never picked — no explicit skip needed.
    // One knockout per warphole (the +146 latch). Driven by the setup-only LCG
    // (like the '-T,H' trampoline placement), NEVER State::rng, so the per-tick
    // RNG contract is untouched and boards with no warpholes are unaffected.
    void knockout_neighbour(int x, int y) {
        static constexpr std::array<int, 4> kDx{0, 1, 0, -1};  // dword_45BECC
        static constexpr std::array<int, 4> kDy{-1, 0, 1, 0};  // dword_45BEDC
        int nx = 0, ny = 0;
        do {
            const auto d = static_cast<std::size_t>(roll() % 4);  // one draw per attempt
            nx = kDx[d] + x;
            ny = kDy[d] + y;
        } while (nx < 0 || nx >= sim::kGridWidth || ny < 0 || ny >= sim::kGridHeight);
        cfg_.cells[ny][nx] = sim::Cell::Blank;  // sub_425E9B(nx, ny, 0)
    }

    // '-T,H' random trampoline: odd-parity ((x+y) odd), un-occupied open tile,
    // <=100 tries. TWO draws per try, in x-then-y order — part of the contract.
    void place_random_trampoline() {
        for (int i = 0; i < 100; ++i) {
            const int x = static_cast<int>(roll() % sim::kGridWidth);
            const int y = static_cast<int>(roll() % sim::kGridHeight);
            if (((x + y) & 1) && !occupied(x, y) && cfg_.cells[y][x] == sim::Cell::Blank) {
                place(x, y, sim::ActorType::Trampoline, 0);
                return;
            }
        }
    }

private:
    sim::MatchConfig& cfg_;
    std::uint32_t lcg_;
};

// Resolve warphole exits (sub_405A81): each warphole links to the FIRST
// other warphole whose idno equals this one's linkto; no partner ⇒ the exit
// is its own tile (the player/bomb stays put, harmless). Deterministic,
// no RNG — the sim reads warp_dest_* directly on a warp.
inline void resolve_warp_links(sim::MatchConfig& cfg, const std::vector<Warp>& warps) {
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

}  // namespace detail

inline void apply_actors(sim::MatchConfig& cfg, const std::vector<assets::extra::Actor>& actors,
                         std::uint32_t seed) {
    using assets::extra::Kind;
    detail::ActorPlacer placer(cfg, seed);
    std::vector<detail::Warp> warps;

    for (const auto& a : actors) {
        switch (a.kind) {
            case Kind::Conveyor: placer.place(a.x, a.y, sim::ActorType::Conveyor, a.dir); break;
            case Kind::DirArrow: placer.place(a.x, a.y, sim::ActorType::DirArrow, a.dir); break;
            case Kind::Warphole:
                placer.place(a.x, a.y, sim::ActorType::Warphole, 0);
                if (a.x >= 0 && a.x < sim::kGridWidth && a.y >= 0 && a.y < sim::kGridHeight) {
                    warps.push_back({a.x, a.y, a.idno, a.linkto});
                    cfg.cells[a.y][a.x] = sim::Cell::Blank;  // sub_425E9B(x, y, 0), own tile
                    placer.knockout_neighbour(a.x, a.y);     // then one random neighbour
                }
                break;
            case Kind::Trampoline:
                if (a.random)
                    placer.place_random_trampoline();
                else
                    placer.place(a.x, a.y, sim::ActorType::Trampoline, 0);
                break;
        }
    }
    detail::resolve_warp_links(cfg, warps);
}

// Picks a stage from the enabled rotation (VALUELST 1150..1160), seed-based.
// The rotation now comes from a LevelRegistry so custom maps can join it; the
// registry defaults to the 11 built-ins, so an existing `pick_stage(tuning,
// seed)` call is byte-identical — enabled_stages() over the built-ins
// reproduces the old ascending-index `allowed` list exactly, and the
// `allowed[seed % allowed.size()]` pick is unchanged (empty -> {0}).
inline int pick_stage(const sim::Tuning& tuning, std::uint32_t seed,
                      const LevelRegistry& registry = builtin_levels()) {
    std::vector<int> allowed = registry.enabled_stages(tuning);
    if (allowed.empty()) allowed.push_back(0);
    return allowed[seed % allowed.size()];
}

}  // namespace bomber::match
