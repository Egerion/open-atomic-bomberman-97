#pragma once

#include <array>
#include <cstdint>

#include "bomber/sim/constants.hpp"
#include "bomber/sim/types.hpp"

namespace bomber::sim {

// One player's complete gameplay state. Deliberately a plain aggregate:
// every field is part of the deterministic state and must be covered by
// state_hash() (see CLAUDE.md "Determinism contract").
struct Player {
    bool present = false;
    bool alive = false;
    Fixed x = 0, y = 0;  // center position in field pixels * 100
    Direction facing = Direction::Down;
    std::int32_t speed = 0;       // movement budget added per tick (VALUELST id 42 + skates)
    std::int32_t move_budget = 0; // carried sub-pixel budget; spent 100 per 1px step (sub_41EC84)
    std::int32_t max_bombs = 1;
    std::int32_t bombs_placed = 0;
    std::int32_t flame = 2;       // cells beyond the epicenter
    std::int32_t skates = 0;
    bool kick = false;
    bool punch = false;
    bool grab = false;
    bool spooge = false;          // lays a line of bombs ahead (Spooger powerup)
    bool trigger = false;
    std::int32_t stun = 0;        // ticks of enforced pause (bomb pickup, head hits)
    // Carried bomb (picked up with the grab glove); its fuse is frozen.
    bool carrying = false;
    std::int32_t carried_fuse = 0, carried_flame = 2;
    bool carried_jelly = false, carried_trigger = false;
    std::uint8_t carried_owner = 0;
    bool jelly = false;
    bool prev_action1 = false;    // for edge detection (part of state!)
    bool prev_action2 = false;
    // Diseases: per-kind active flags sharing one countdown; a healthy player
    // has disease_timer == 0. disease_fresh gates re-spreading (VALUELST id 129).
    std::array<bool, kDiseaseKinds> disease{};
    std::int32_t disease_timer = 0;
    std::int32_t disease_fresh = 0;

    int tile_x() const { return static_cast<int>(x / kTileWF); }
    int tile_y() const { return static_cast<int>(y / kTileHF); }
    bool sick(Disease d) const { return disease[static_cast<int>(d)]; }
};

}  // namespace bomber::sim
