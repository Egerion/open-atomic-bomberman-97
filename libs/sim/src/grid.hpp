#pragma once

#include <array>

#include "bomber/sim/state.hpp"

// Internal grid/geometry helpers shared by the systems. Not installed —
// nothing outside libs/sim/src may include this header.

namespace bomber::sim::grid {

// The original's godir unit vectors, exactly dword_45BECC = {0,1,0,-1} (cos)
// and dword_45BEDC = {-1,0,1,0} (sin), GODIR-indexed 0=Up,1=Right,2=Down,3=Left
// and confirmed against the .data dump (docs/re/ai.md §3.3). The (g±1)&3 corner
// rotations every caller performs depend on this exact ordering, and this is the
// ONE definition in libs/sim — a second copy edited alone would diverge silently.
inline constexpr std::array<int, 4> kDx = {0, 1, 0, -1};
inline constexpr std::array<int, 4> kDy = {-1, 0, 1, 0};

// Every full-field sweep in libs/sim goes through here, so they all agree on
// ROW-MAJOR order (y outer, x inner). That is not cosmetic: state_hash mixes the
// field in this order, so a sweep that transposed it would silently change the
// digest, and the flame/regen sweeps rely on it to visit a row before the one
// below. `fn(x, y)`.
template <typename Fn>
inline void for_each_cell(Fn&& fn) {
    for (int y = 0; y < kGridHeight; ++y)
        for (int x = 0; x < kGridWidth; ++x) fn(x, y);
}

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
    static constexpr std::array<Direction, 4> kDirs = {Direction::Up, Direction::Right,
                                                       Direction::Down, Direction::Left};
    return kDirs[static_cast<std::size_t>(g & 3)];
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

// The live, present player standing on (tx,ty), in slot order, or -1
// (sub_421CB5 — "active +0, not dead +8"; the +58 stun does NOT disqualify).
inline int player_index_at(const State& s, int tx, int ty) {
    for (int i = 0; i < kMaxPlayers; ++i) {
        const Player& pl = s.players[i];
        if (pl.present && pl.alive && pl.tile_x() == tx && pl.tile_y() == ty) return i;
    }
    return -1;
}

inline bool player_at(const State& s, int tx, int ty) {
    return player_index_at(s, tx, ty) >= 0;
}

}  // namespace bomber::sim::grid
