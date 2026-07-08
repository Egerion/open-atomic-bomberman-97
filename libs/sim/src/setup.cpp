// Initial match state construction (Simulation constructor backend).

#include <algorithm>
#include <utility>
#include <vector>

#include "bomber/sim/rng.hpp"
#include "bomber/sim/simulation.hpp"
#include "grid.hpp"
#include "systems/powerups.hpp"

namespace bomber::sim::detail {

State build_state(const MatchConfig& config) {
    State s;
    s.rng = config.seed;
    s.tuning = config.tuning;
    s.forbidden = config.forbidden;
    s.ticks_left = config.tuning.game_seconds * kTicksPerSecond;
    s.cells = config.cells;
    for (auto& row : s.hidden) row.fill(PowerupType::None);
    for (auto& row : s.floor) row.fill(PowerupType::None);
    // Stage actors are a static per-match layer (docs/re/stage-actors.md):
    // copied verbatim from the parsed EXTRA<N>.RES layout, then hashed. None
    // (255) is the empty sentinel, so start every tile empty then overlay.
    for (auto& row : s.actor_type) row.fill(ActorType::None);
    s.actor_dir = config.actor_dir;
    s.warp_dest_x = config.warp_dest_x;  // pre-resolved warphole exits (no sim RNG)
    s.warp_dest_y = config.warp_dest_y;
    for (int y = 0; y < kGridHeight; ++y)
        for (int x = 0; x < kGridWidth; ++x)
            s.actor_type[y][x] = config.actor_type[y][x];

    PowerupSystem powerups{s};

    for (int i = 0; i < config.player_count && i < kMaxPlayers; ++i) {
        if (i >= static_cast<int>(config.spawns.size())) continue;  // no spawn -> skip
        if (!config.active[i]) continue;                            // OFF slot (setup screen)
        Player& p = s.players[i];
        p.present = true;
        p.alive = true;
        p.ai = config.ai[i];      // computer-driven slot (ADR-0005); default false
        p.team = config.team[i];  // setup-screen +84 byte (docs/re/setup-screens.md); default 0
        int tx = std::clamp(config.spawns[i].x, 0, kGridWidth - 1);
        int ty = std::clamp(config.spawns[i].y, 0, kGridHeight - 1);
        // The original clears the spawn tile and its orthogonal neighbours
        // so every player starts with room to move.
        static constexpr int ndx[] = {0, 1, -1, 0, 0}, ndy[] = {0, 0, 0, 1, -1};
        for (int n = 0; n < 5; ++n) {
            int cx2 = tx + ndx[n], cy2 = ty + ndy[n];
            if (cx2 >= 0 && cx2 < kGridWidth && cy2 >= 0 && cy2 < kGridHeight &&
                s.cells[cy2][cx2] == Cell::Brick)
                s.cells[cy2][cx2] = Cell::Blank;
        }
        p.x = grid::tile_center_x(tx);
        p.y = grid::tile_center_y(ty);
        p.speed = s.tuning.start_speed;
        p.max_bombs = s.tuning.start_with[static_cast<int>(PowerupType::ExtraBomb)];
        p.flame = s.tuning.start_with[static_cast<int>(PowerupType::Flame)];
        for (int k = 0; k < kPowerupKinds; ++k)
            if (config.born_with[k]) powerups.apply(p, static_cast<PowerupType>(k));
    }

    // Hide powerups under randomly chosen bricks (seeded RNG — deterministic).
    std::vector<std::pair<int, int>> bricks;
    for (int y = 0; y < kGridHeight; ++y)
        for (int x = 0; x < kGridWidth; ++x)
            if (s.cells[y][x] == Cell::Brick) bricks.emplace_back(x, y);

    for (int k = 0; k < kPowerupKinds; ++k) {
        if (config.forbidden[k]) continue;
        std::int32_t want = config.spawn_override[k] > MatchConfig::kNoOverride
                                ? config.spawn_override[k]
                                : s.tuning.spawn_counts[k];
        std::int32_t count = want;
        if (want < 0) {
            // Negative N: |N| attempts, each with a 1-in-10 chance.
            count = 0;
            for (int i = 0; i < -want; ++i)
                if (random_below(s, 10) == 0) ++count;
        }
        for (int i = 0; i < count && !bricks.empty(); ++i) {
            std::uint32_t pick = random_below(s, static_cast<std::uint32_t>(bricks.size()));
            auto [bx, by] = bricks[pick];
            bricks.erase(bricks.begin() + pick);
            s.hidden[by][bx] = static_cast<PowerupType>(k);
        }
    }

    // Arm the dud gate (the original arms it once at match init, sub_422C7A
    // -> sub_422C13): base + rand(spread) ticks from now.
    s.dud_gate = static_cast<std::uint64_t>(s.tuning.dud_gate_base) +
                 random_below(s, static_cast<std::uint32_t>(
                                     std::max<std::int32_t>(1, s.tuning.dud_gate_rand)));
    return s;
}

}  // namespace bomber::sim::detail
