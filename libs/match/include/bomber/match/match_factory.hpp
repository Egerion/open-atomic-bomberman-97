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
                                           const assets::res::ValueList* values = nullptr) {
    sim::MatchConfig cfg;
    cfg.player_count = player_count;
    cfg.seed = seed;

    if (values)
        for (const auto& [id, v] : values->values) cfg.tuning.apply(id, v);

    for (int y = 0; y < sim::kGridHeight && y < scheme.height(); ++y) {
        for (int x = 0; x < sim::kGridWidth && x < scheme.width(); ++x) {
            switch (scheme.at(x, y)) {
                case assets::sch::Cell::Solid: cfg.cells[y][x] = sim::Cell::Solid; break;
                case assets::sch::Cell::Brick: cfg.cells[y][x] = sim::Cell::Brick; break;
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
    };
    std::uint32_t lcg = seed ? seed : 0x1234567u;  // setup-only stream
    auto roll = [&]() { lcg = lcg * 1664525u + 1013904223u; return lcg >> 16; };

    for (const auto& a : actors) {
        switch (a.kind) {
            case Kind::Conveyor: place(a.x, a.y, sim::ActorType::Conveyor, a.dir); break;
            case Kind::DirArrow: place(a.x, a.y, sim::ActorType::DirArrow, a.dir); break;
            case Kind::Warphole: place(a.x, a.y, sim::ActorType::Warphole, 0); break;
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
