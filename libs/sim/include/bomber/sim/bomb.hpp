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
    // The fuse DURATION this bomb was created with (sub_422EDE stores it at
    // word +74 for EVERY kind, trigger included; our running `fuse` is the
    // original's elapsed counter +68 recast as a countdown). Two consumers,
    // both faithful ports: a thrown carried bomb restarts from this value
    // (sub_41F29B LABEL_246 zeroes elapsed +68 before the launch), and a
    // trigger bomb downgraded by a Trigger EVICTION relights with it
    // (sub_424C47 sets kind 0, elapsed 0). facts.md "Core-feel audit" §2/§5.
    std::int32_t fuse_init = 0;
    std::int32_t dud_left = 0;    // fizzle ticks remaining (dud state; fuse frozen)
    std::int32_t flame = 2;
    bool jelly = false;
    bool trigger = false;
    bool moving = false;          // kicked
    // Kick+action2 "stop my bombs" (sub_4247C5 sets bomb byte +57 on the
    // owner's sliding non-jelly bombs): the slide loop consumes it by snapping
    // the bomb onto the next tile centre it reaches (sub_42331C `+57 && at-or-
    // past-centre`). A DIRARROW clears it (~25535). facts.md "Core-feel audit" §4.
    bool stop_pending = false;
    // Warphole one-shot latch (docs/re/stage-actors.md §6): set when a sliding
    // bomb teleports through a warp, cleared once it leaves the warp tile — the
    // bomb analogue of Player::warp_latch, preventing a warp-to-warp ping-pong.
    bool warp_latch = false;
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
