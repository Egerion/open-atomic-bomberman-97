#pragma once

#include <cstdint>

#include "bomber/sim/constants.hpp"
#include "bomber/sim/types.hpp"

namespace bomber::sim {

// One bomb. Plain aggregate — see the determinism note in player.hpp.
struct Bomb {
    bool active = false;
    std::uint8_t owner = 0;
    Fixed x = 0, y = 0;           // center, aligned to tile unless moving
    std::int32_t fuse = 0;        // ticks until detonation (<0: waits for trigger)
    std::int32_t flame = 2;
    bool jelly = false;
    bool trigger = false;
    bool moving = false;          // kicked
    Direction dir = Direction::Up;
    // Airborne (punched/thrown): travels from_* -> to_* in fly_total ticks.
    // While flying it doesn't block, can't chain, and its fuse is paused.
    bool flying = false;
    std::int32_t fly_ticks = 0, fly_total = 0, fly_arc = 0;  // arc = bounce height, px
    Fixed from_x = 0, from_y = 0, to_x = 0, to_y = 0;

    int tile_x() const { return static_cast<int>(x / kTileWF); }
    int tile_y() const { return static_cast<int>(y / kTileHF); }
};

}  // namespace bomber::sim
