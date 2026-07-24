// Shared AI internals: the godir unit vectors used by every AI translation
// unit (grids, pathfinding, behaviours). Factored out of ai.cpp as part of a
// pure file-split (no behaviour change) so the shared tables have a single
// definition instead of being duplicated across the split TUs. See ai.hpp for
// the subsystem API and staged scope.
#pragma once

namespace bomber::sim {

// Godir unit vectors in the original's order (0=Up,1=Right,2=Down,3=Left),
// exactly dword_45BECC={0,1,0,-1} / dword_45BEDC={-1,0,1,0} (docs/re/ai.md §3.3).
inline constexpr int kDX[4] = {0, 1, 0, -1};
inline constexpr int kDY[4] = {-1, 0, 1, 0};

}  // namespace bomber::sim
