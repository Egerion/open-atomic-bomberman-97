#include "systems/enclosure.hpp"

#include <algorithm>
#include <utility>
#include <vector>

#include "systems/flames.hpp"

namespace bomber::sim {
namespace {

// Clockwise ring order starting top-left, outermost ring first.
const std::vector<std::pair<int, int>>& enclose_order() {
    static const std::vector<std::pair<int, int>> order = [] {
        std::vector<std::pair<int, int>> o;
        int rings = std::min(kGridWidth, kGridHeight) / 2 + 1;
        for (int r = 0; r < rings; ++r) {
            int x0 = r, x1 = kGridWidth - 1 - r, y0 = r, y1 = kGridHeight - 1 - r;
            if (x0 > x1 || y0 > y1) break;
            for (int x = x0; x <= x1; ++x) o.emplace_back(x, y0);
            for (int y = y0 + 1; y <= y1; ++y) o.emplace_back(x1, y);
            if (y1 > y0)
                for (int x = x1 - 1; x >= x0; --x) o.emplace_back(x, y1);
            if (x1 > x0)
                for (int y = y1 - 1; y > y0; --y) o.emplace_back(x0, y);
        }
        return o;
    }();
    return order;
}

// Depth setting -> number of rings closed (id 27: 1 = 2 rings, 3 = all).
int enclose_rings(int depth) {
    if (depth <= 0) return 0;
    if (depth >= 3) return 6;
    return depth * 2;
}

}  // namespace

int EnclosureSystem::total(int depth) {
    int rings = enclose_rings(depth);
    int n = 0;
    for (int r = 0; r < rings; ++r) {
        int w = kGridWidth - 2 * r, h = kGridHeight - 2 * r;
        if (w <= 0 || h <= 0) break;
        n += (h <= 1) ? w : (w <= 1 ? h : 2 * w + 2 * h - 4);
    }
    return std::min<int>(n, static_cast<int>(enclose_order().size()));
}

bool EnclosureSystem::position(int index, int depth, int* x, int* y) {
    if (index < 0 || index >= total(depth)) return false;
    *x = enclose_order()[static_cast<std::size_t>(index)].first;
    *y = enclose_order()[static_cast<std::size_t>(index)].second;
    return true;
}

void EnclosureSystem::drop_wall(int wx, int wy) {
    State& s = s_;
    // A bomb on the tile detonates or is eaten, per VALUELST id 46.
    for (std::size_t bi = 0; bi < s.bombs.size(); ++bi) {
        Bomb& b = s.bombs[bi];
        if (!b.active || b.tile_x() != wx || b.tile_y() != wy) continue;
        if (s.tuning.wall_detonates) {
            flames_.explode(bi);
        } else {
            b.active = false;
            if (s.players[b.owner].bombs_placed > 0) --s.players[b.owner].bombs_placed;
        }
    }
    s.cells[wy][wx] = Cell::Solid;
    s.burning[wy][wx] = 0;
    s.flame[wy][wx] = 0;
    s.hidden[wy][wx] = PowerupType::None;
    s.floor[wy][wx] = PowerupType::None;
    for (int i = 0; i < kMaxPlayers; ++i) {
        Player& p = s.players[i];
        if (p.present && p.alive && p.tile_x() == wx && p.tile_y() == wy) {
            p.alive = false;
            if (p.carrying) {
                p.carrying = false;
                if (s.players[p.carried_owner].bombs_placed > 0)
                    --s.players[p.carried_owner].bombs_placed;
            }
            s.events.push_back({Event::Type::PlayerDied, static_cast<std::int8_t>(i),
                                static_cast<std::int8_t>(wx), static_cast<std::int8_t>(wy), 0});
        }
    }
    s.events.push_back({Event::Type::WallClosed, -1, static_cast<std::int8_t>(wx),
                        static_cast<std::int8_t>(wy), 0});
}

void EnclosureSystem::update() {
    State& s = s_;
    int depth = s.tuning.enclosement_depth;

    // Arm at the threshold; the interval is chosen so the last wall lands
    // just before time-up.
    if (!s.hurry && s.ticks_left > 0 &&
        s.ticks_left <= s.tuning.hurry_seconds * kTicksPerSecond) {
        s.hurry = true;
        s.events.push_back({Event::Type::Hurry, -1, -1, -1, 0});
        int n = total(depth);
        s.enclose_interval =
            std::max(1, s.tuning.hurry_seconds * kTicksPerSecond / (n + 1));
        s.enclose_timer = s.enclose_interval;
        s.enclose_index = 0;
    }

    if (s.hurry && depth > 0 && s.enclose_index < total(depth) && --s.enclose_timer <= 0) {
        s.enclose_timer = s.enclose_interval;
        int wx = 0, wy = 0;
        if (position(s.enclose_index++, depth, &wx, &wy)) drop_wall(wx, wy);
    }
}

}  // namespace bomber::sim
