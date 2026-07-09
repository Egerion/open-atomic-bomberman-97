#pragma once

#include <cstdint>

#include "bomber/sim/constants.hpp"
#include "bomber/sim/types.hpp"

// Campaign-mode autonomous hazard actors (docs/re/campaign.md "Rover/ghost/AI
// roster" and "Per-tick mover — sub_401B5C"). Plain aggregate, same
// determinism rules as Player/Bomb (see player.hpp's note).

namespace bomber::sim {

// Mirrors the original's actor-table +4 type byte verbatim (1/2, not 0/1)
// so the RE cross-reference in campaign.md stays legible.
enum class RoverKind : std::uint8_t { Rover = 1, Ghost = 2 };

struct Rover {
    bool alive = false;  // live/reaped flag (original's +0)
    RoverKind kind = RoverKind::Rover;
    Fixed x = 0, y = 0;            // current pixel position (original's +28/+32)
    std::uint8_t dir = 0;          // godir 0..3 (original's +42 high word / +44)
    std::int32_t speed = 0;        // per-tick budget increment (.CAM field 4/6)
    std::int32_t move_budget = 0;  // carried sub-pixel budget (original's +116)
    // Per-tick step counter (original's +48), advanced once per pixel step.
    // Drives the draw-frame index (sub_4518D0/sub_41DAA7) — presentation-
    // adjacent but a deterministic part of the original's struct, so hashed
    // like every other field here (CLAUDE.md determinism rule 4).
    std::uint16_t anim_step = 0;

    int tile_x() const { return static_cast<int>(x / kTileWF); }
    int tile_y() const { return static_cast<int>(y / kTileHF); }
};

}  // namespace bomber::sim
