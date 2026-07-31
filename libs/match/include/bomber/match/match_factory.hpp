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
// 4th field is the per-slot TEAM"). Reproduces sub_403EEE's two steps: seed
// every slot with the low bit of its index — the standing default is the
// alternating 0,1,0,1,… pattern, NOT a flat 0 — then let a row carrying a 4th
// field overwrite its own slot.
//
// The user may still flip a slot with 'T' afterwards; that is the original's
// precedence, since sub_410F81 loads the scheme before its key loop.
template <std::size_t N>
constexpr void scheme_setup_teams(const assets::sch::Scheme& scheme, std::array<int, N>& team) {
    for (std::size_t i = 0; i < N; ++i) team[i] = static_cast<int>(i) & 1;  // sub_4049C0
    for (const auto& sp : scheme.spawns)
        if (sp.has_team && sp.player >= 0 && static_cast<std::size_t>(sp.player) < N)
            team[static_cast<std::size_t>(sp.player)] = sp.team != 0 ? 1 : 0;
}

// The stages of build_match_config, in the order it runs them. Each carries its
// own RE citation and its own RNG stream. The ORDER of the calls, and the draw
// order inside each, is the contract — see the determinism rules in CLAUDE.md.
namespace detail {

// One `.SCH` cell -> one sim cell. Split out so fill_bricks' grid walk stays two
// plain loops with a single statement in the body.
//
// A ':' is only a brick CANDIDATE: sub_4260F5 draws `rand() % 100` for it and
// knocks it back to blank when that is `>= density`, so on average `density`% of
// them become real bricks. '#' and '.' are copied verbatim and consume NO draw
// — the original tests "is this a candidate" first and && short-circuits.
template <typename Roll>
sim::Cell resolve_cell(assets::sch::Cell cell, int density, Roll&& roll) {
    if (cell == assets::sch::Cell::Solid) return sim::Cell::Solid;
    if (cell != assets::sch::Cell::Brick) return sim::Cell::Blank;
    return static_cast<int>(roll()) >= density ? sim::Cell::Blank : sim::Cell::Brick;
}

// Per-match destructible-brick fill (sub_4260F5, pseudo.c ~26928), walked
// ROW-MAJOR because the draw order is part of the contract. Density is the
// scheme's `-B` clamped to [0,100]; BASIC.SCH ships `-B,90`.
//
// Determinism: the original seeds rand() off the wall clock, so its layout is
// not reproducible. This drives the fill from a SETUP-ONLY LCG seeded off the
// match seed — NEVER State::rng — so the per-tick draw contract is untouched
// (the fill is pre-sim, producing a static grid the sim treats as fixed input)
// while identical seeds still reproduce identical boards.
inline void fill_bricks(sim::MatchConfig& cfg, const assets::sch::Scheme& scheme,
                        std::uint32_t seed) {
    const int density = std::clamp(scheme.brick_density, 0, 100);
    std::uint32_t brick_lcg = seed ^ 0x9E3779B9u;  // decorrelated from apply_actors' stream
    auto brick_roll = [&brick_lcg]() {
        brick_lcg = brick_lcg * 1664525u + 1013904223u;
        return (brick_lcg >> 16) % 100u;  // mirrors rand() % 100
    };
    for (int y = 0; y < sim::kGridHeight && y < scheme.height(); ++y)
        for (int x = 0; x < sim::kGridWidth && x < scheme.width(); ++x)
            cfg.cells[y][x] = resolve_cell(scheme.at(x, y), density, brick_roll);
}

// SECURITY, and the bound is needed BOTH ways. `sp.player` comes straight from
// the "-S" row of an untrusted .SCH — one the game's own editor can produce.
// Unbounded this is an out-of-bounds vector write: "-S,-1,0,0" makes the SIGNED
// `>=` resize test false, skips the grow, then indexes at size_t(-1);
// "-S,2000000000,0,0" resizes toward 16 GB instead. The upper bound is the
// original's own — sub_4049C0 seeds exactly 10 start records, so a row outside
// [0, kMaxPlayers) has nowhere to land and is dropped.
inline void place_spawns(sim::MatchConfig& cfg, const assets::sch::Scheme& scheme) {
    for (const auto& sp : scheme.spawns) {
        if (sp.player < 0 || sp.player >= sim::kMaxPlayers) continue;
        const auto slot = static_cast<std::size_t>(sp.player);
        if (slot >= cfg.spawns.size()) cfg.spawns.resize(slot + 1);
        cfg.spawns[slot] = {std::clamp(sp.x, 0, sim::kGridWidth - 1),
                            std::clamp(sp.y, 0, sim::kGridHeight - 1)};
    }
}

// "Random Start" (options.ini `random_start=`, sub_421793 pseudo.c
// 23996-24012). CONFIRMED a shuffle, not a re-roll: 200 pair-swaps, each drawing
// two indices and swapping that pair in BOTH the X and Y arrays when they
// differ, so every player still starts on an authored spawn point. Mirrored 1:1
// — the original's %10 is its fixed array size, ours is spawns.size().
//
// Setup-only LCG, decorrelated from the brick-fill and actor streams and never
// State::rng, so the per-tick draw contract is untouched.
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

// The .SCH '-P' powerup rows. "Born with" REPLACES the VALUELST baseline rather
// than granting on top of it — sub_403EEE calls the value-table SETTER, so the
// scheme overwrites the id the round init and the surplus tests both read
// (docs/re/facts.md).
//
// ORDER: must run AFTER build_match_config's VALUELST pass. The `> 0` gate is
// the original's own: a count of 0 leaves the baseline alone, it does not zero
// it. Deliberate divergence — that global write is never restored, so the
// original leaks a scheme's counts into the NEXT match; Tuning is rebuilt here.
inline void apply_powerup_rules(sim::MatchConfig& cfg, const assets::sch::Scheme& scheme) {
    for (const auto& pr : scheme.powerups) {
        if (pr.id < 0 || pr.id >= sim::kPowerupKinds) continue;
        if (pr.forbidden) cfg.forbidden[pr.id] = true;
        if (pr.has_override) cfg.spawn_override[pr.id] = pr.override_value;
        if (pr.born_with > 0) cfg.tuning.start_with[pr.id] = pr.born_with;
    }
}

}  // namespace detail

// `team_play` is the Options-screen team gate (dword_464964). sim::Player::team
// reserves 0 for "no team / solo side", so an OFF gate leaves every cfg.team[]
// at 0 and an ON gate shifts the scheme's 0/1 byte up into the sim's 1/2.
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
// cell grid. Random '-T,H' trampolines resolve HERE against a setup-only LCG,
// NOT the per-tick RNG, so the determinism contract is untouched while identical
// seeds still reproduce identical boards.
namespace detail {

// A placed warphole, tracked while actors go down so sub_405A81's idno/linkto
// resolution can run once they are all placed.
struct Warp {
    int x, y, idno, linkto;
};

// The grid writes and the setup-only LCG apply_actors needs, as one cheap stack
// object holding a reference to the config it mutates — the same shape libs/sim's
// systems use. It exists so the actor dispatch below stays a FLAT switch.
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
        // on"). sub_4056CA's four cases are asymmetric: only the warphole writes
        // a cell, and that is handled on its branch of the dispatch below.
        //
        // Blanking here was a live divergence worth up to a THIRD of a stage's
        // bricks — measured on BASIC.SCH at 4 players over 400 seeds, ANCIENT
        // EGYPT lost 32.7% and INNER CITY TRASH 29.1% — and it opened those
        // tiles for walking from round start. The "warphole looks closed"
        // concern the old comment cited is the renderer's job, and it already
        // handles it the original's way by skipping any tile that is not Blank.
    }

    // Warphole one-time setup knockout (sub_4056CA case 1): activating a
    // warphole clears its OWN tile and then ONE RANDOM ADJACENT tile,
    // unconditionally — brick or solid, whatever sat there. The only actor case
    // that writes a cell at all; see place() above.
    //
    // One draw per attempt, rejecting off-board results, mirroring the
    // original's nested rejection loop. The dx/dy tables are its cos/sin dir
    // pair, never both 0, so the centre cannot be picked and needs no skip.
    // Setup-only LCG, never State::rng.
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

// The FIRST other warphole whose idno matches `w.linkto`, or nullptr when there
// is none — sub_405A81's no-match case, where the exit is the entrance and the
// player/bomb simply stays put.
inline const Warp* warp_partner(const std::vector<Warp>& warps, const Warp& w) {
    for (const Warp& other : warps)
        if ((other.x != w.x || other.y != w.y) && other.idno == w.linkto) return &other;
    return nullptr;
}

// Resolve warphole exits (sub_405A81). Deterministic, no RNG — the sim reads
// warp_dest_* directly on a warp.
inline void resolve_warp_links(sim::MatchConfig& cfg, const std::vector<Warp>& warps) {
    for (const Warp& w : warps) {
        const Warp* dest = warp_partner(warps, w);
        cfg.warp_dest_x[w.y][w.x] = static_cast<std::uint8_t>(dest ? dest->x : w.x);
        cfg.warp_dest_y[w.y][w.x] = static_cast<std::uint8_t>(dest ? dest->y : w.y);
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
                if (a.random) {
                    placer.place_random_trampoline();
                    break;
                }
                placer.place(a.x, a.y, sim::ActorType::Trampoline, 0);
                break;
        }
    }
    detail::resolve_warp_links(cfg, warps);
}

// Picks a stage from the enabled rotation (VALUELST 1150..1160), seed-based.
// The rotation comes from a LevelRegistry so custom maps can join it; an empty
// rotation falls back to stage 0.
inline int pick_stage(const sim::Tuning& tuning, std::uint32_t seed,
                      const LevelRegistry& registry = builtin_levels()) {
    std::vector<int> allowed = registry.enabled_stages(tuning);
    if (allowed.empty()) allowed.push_back(0);
    return allowed[seed % allowed.size()];
}

}  // namespace bomber::match
