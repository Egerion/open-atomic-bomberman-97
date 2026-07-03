#include "systems/flames.hpp"

#include <algorithm>

#include "grid.hpp"

namespace bomber::sim {

bool FlameSystem::spread_to(int tx, int ty, std::uint8_t owner) {
    State& s = s_;
    if (!grid::in_grid(tx, ty)) return false;
    Cell& c = s.cells[ty][tx];
    if (c == Cell::Solid) return false;
    if (c == Cell::Brick) {
        c = Cell::Blank;
        s.burning[ty][tx] = static_cast<std::uint8_t>(
            std::clamp<std::int32_t>(s.tuning.brick_burn_frames, 1, 255));
        s.events.push_back({Event::Type::BrickDestroyed, -1, static_cast<std::int8_t>(tx),
                            static_cast<std::int8_t>(ty), 0});
        return false;  // flame stops at the brick it destroys
    }
    if (s.burning[ty][tx] > 0) return false;

    s.flame[ty][tx] = static_cast<std::uint8_t>(
        std::clamp<std::int32_t>(s.tuning.flame_frames, 1, 255));
    s.flame_owner[ty][tx] = owner;

    if (s.floor[ty][tx] != PowerupType::None) {
        s.events.push_back({Event::Type::PowerupBurned, -1, static_cast<std::int8_t>(tx),
                            static_cast<std::int8_t>(ty),
                            static_cast<std::int8_t>(s.floor[ty][tx])});
        s.floor[ty][tx] = PowerupType::None;
    }

    // Chain reaction: bombs caught in the blast go off in the same tick.
    for (std::size_t i = 0; i < s.bombs.size(); ++i) {
        Bomb& b = s.bombs[i];
        if (b.active && b.tile_x() == tx && b.tile_y() == ty) explode(i);
    }
    return true;
}

void FlameSystem::explode(std::size_t bomb_index) {
    State& s = s_;
    Bomb& b = s.bombs[bomb_index];
    if (!b.active) return;
    b.active = false;
    if (s.players[b.owner].bombs_placed > 0) --s.players[b.owner].bombs_placed;

    int cx = b.tile_x(), cy = b.tile_y();
    int reach = b.flame;
    s.events.push_back({Event::Type::Explosion, static_cast<std::int8_t>(b.owner),
                        static_cast<std::int8_t>(cx), static_cast<std::int8_t>(cy), 0});
    spread_to(cx, cy, b.owner);
    for (Direction d : {Direction::Up, Direction::Down, Direction::Left, Direction::Right}) {
        for (int i = 1; i <= reach; ++i) {
            if (!spread_to(cx + grid::dir_dx(d) * i, cy + grid::dir_dy(d) * i, b.owner)) break;
        }
    }
}

void FlameSystem::age_flames_and_bricks() {
    State& s = s_;
    for (int y = 0; y < kGridHeight; ++y) {
        for (int x = 0; x < kGridWidth; ++x) {
            if (s.flame[y][x] > 0) --s.flame[y][x];
            if (s.burning[y][x] > 0 && --s.burning[y][x] == 0) {
                if (s.hidden[y][x] != PowerupType::None) {
                    s.floor[y][x] = s.hidden[y][x];
                    s.hidden[y][x] = PowerupType::None;
                    s.events.push_back({Event::Type::PowerupRevealed, -1,
                                        static_cast<std::int8_t>(x), static_cast<std::int8_t>(y),
                                        static_cast<std::int8_t>(s.floor[y][x])});
                }
            }
        }
    }
}

}  // namespace bomber::sim
