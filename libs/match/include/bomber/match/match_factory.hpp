#pragma once

// Anti-corruption layer between the asset formats and the simulation: builds
// a sim::MatchConfig from a parsed .SCH scheme and (optionally) a parsed
// VALUELST.RES, and picks stages from the enabled rotation. SDL-free.

#include <algorithm>
#include <cstdint>
#include <vector>

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

// Picks a stage from the enabled rotation (VALUELST 1150..1160), seed-based.
inline int pick_stage(const sim::Tuning& tuning, std::uint32_t seed) {
    std::vector<int> allowed;
    for (int i = 0; i < 11; ++i)
        if (tuning.level_enabled[i]) allowed.push_back(i);
    if (allowed.empty()) allowed.push_back(0);
    return allowed[seed % allowed.size()];
}

}  // namespace bomber::match
