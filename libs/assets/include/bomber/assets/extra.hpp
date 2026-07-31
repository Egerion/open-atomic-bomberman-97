#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

// EXTRA<N>.RES parser — the stage-actor placement data source (NOT the .SCH
// grid). See docs/re/stage-actors.md §2 for the full RE. Plain-text, line
// oriented, comma/space separated:
//
//   -A,<dir>,<x>,<y>                    dirArrow   (N/E/S/W)
//   -C,<dir>,<x>,<y>                    conveyor   (N/E/S/W)
//   -T,<x>,<y>                          trampoline
//   -T,H,H                             trampoline, random odd-parity placement
//   -W,<type>,<idno>,<x>,<y>,<linkto>  warphole
//
// Negative coordinates wrap from the far edge; over-large ones clamp. Direction
// letters map n/e/s/w -> godir 0/1/2/3 (Up/Right/Down/Left).

namespace bomber::assets::extra {

enum class Kind : std::uint8_t { DirArrow = 0, Warphole = 1, Conveyor = 2, Trampoline = 3 };

struct Actor {
    Kind kind = Kind::DirArrow;
    int x = 0, y = 0;         // tile coords, already normalized to the board
    int dir = 0;              // godir (0=Up,1=Right,2=Down,3=Left); N/A for tramp/warp
    int idno = 0;             // warphole only
    int linkto = 0;           // warphole only
    bool random = false;      // trampoline placed with -T,H (resolved by caller RNG)
};

// Coordinates are normalized against the board exactly as sub_404E99 does. A
// '-T,H,H' line yields random==true with x/y unset: the CALLER resolves those
// with a setup-only RNG, so the sim's per-tick stream stays untouched
// (docs/re/stage-actors.md §8).
//
// A missing file is an empty vector, and a malformed line is skipped — the
// original aborts there, but a board is still playable without one actor.
std::vector<Actor> parse(const std::filesystem::path& path, int board_w, int board_h);

std::vector<Actor> load_for_board(const std::filesystem::path& game_dir, int board, int board_w,
                                  int board_h);

}  // namespace bomber::assets::extra
