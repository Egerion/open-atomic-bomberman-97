#pragma once

#include "bomber/sim/state.hpp"

// Internal grid/geometry helpers shared by the systems. Not installed —
// nothing outside libs/sim/src may include this header.

namespace bomber::sim::grid {

inline Fixed tile_center_x(int tx) { return tx * kTileWF + kTileWF / 2; }
inline Fixed tile_center_y(int ty) { return ty * kTileHF + kTileHF / 2; }

inline bool in_grid(int tx, int ty) {
    return tx >= 0 && tx < kGridWidth && ty >= 0 && ty < kGridHeight;
}

inline int dir_dx(Direction d) {
    return d == Direction::Left ? -1 : d == Direction::Right ? 1 : 0;
}
inline int dir_dy(Direction d) {
    return d == Direction::Up ? -1 : d == Direction::Down ? 1 : 0;
}

// The original's direction indexing (godir): 0=Up, 1=Right, 2=Down, 3=Left.
// Rotations like (dir ± 1) & 3 depend on this exact ordering.
inline int to_godir(Direction d) {
    switch (d) {
        case Direction::Up: return 0;
        case Direction::Right: return 1;
        case Direction::Down: return 2;
        case Direction::Left: return 3;
    }
    return 1;
}
inline Direction from_godir(int g) {
    static constexpr Direction kDirs[4] = {Direction::Up, Direction::Right, Direction::Down,
                                           Direction::Left};
    return kDirs[g & 3];
}

// A tile a player/bomb may occupy: inside the grid, not solid, not a brick
// (crumbling bricks still block until fully gone).
inline bool tile_open(const State& s, int tx, int ty) {
    if (!in_grid(tx, ty)) return false;
    if (s.cells[ty][tx] != Cell::Blank) return false;
    if (s.burning[ty][tx] > 0) return false;
    return true;
}

// The grounded bomb on a tile, if any (airborne bombs occupy no tile).
inline Bomb* bomb_at(State& s, int tx, int ty) {
    for (auto& b : s.bombs)
        if (b.active && !b.flying && b.tile_x() == tx && b.tile_y() == ty) return &b;
    return nullptr;
}

inline const Bomb* bomb_at(const State& s, int tx, int ty) {
    return bomb_at(const_cast<State&>(s), tx, ty);
}

}  // namespace bomber::sim::grid
