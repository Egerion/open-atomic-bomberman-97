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

// Parses the text into actors. Numeric coordinates are normalized against
// (board_w, board_h) exactly as sub_404E99 does (wrap negatives, clamp overs).
// A '-T,H,H' line yields an Actor with random==true and x/y unset — the CALLER
// resolves those with a setup-only RNG so the sim's per-tick RNG stream is
// untouched (docs/re/stage-actors.md §8). Missing file => empty vector (a board
// with no EXTRA<N>.RES simply has no actors). Malformed lines are skipped
// (the format is lenient; the original aborts, but we prefer to keep playing).
std::vector<Actor> parse(const std::filesystem::path& path, int board_w, int board_h);

// Convenience: build the EXTRA<board>.RES path under a game dir and parse it.
std::vector<Actor> load_for_board(const std::filesystem::path& game_dir, int board, int board_w,
                                  int board_h);

}  // namespace bomber::assets::extra
