#pragma once

#include <array>
#include <cstdint>

#include "bomber/sim/constants.hpp"

// Plain enumerations and input records shared across the simulation.
// Everything here is trivially copyable by design (ADR-0003): the whole
// gameplay state must remain hashable and snapshot-friendly.

namespace bomber::sim {

enum class Cell : std::uint8_t { Blank, Brick, Solid };

// Order matches the scheme -P table and VALUELST id blocks (50/400/550).
enum class PowerupType : std::uint8_t {
    ExtraBomb, Flame, Disease, Kick, Skate, Punch, Grab, Spooger,
    Goldflame, Trigger, Jelly, SuperDisease, Random,
    None = 255,
};

enum class Direction : std::uint8_t { Up, Down, Left, Right };

// The nine diseases (skull powerup), in the original's rand()%9 index order —
// see docs/re/facts.md "Disease system". Swap has no persistent flag.
enum class Disease : std::uint8_t {
    Slow, Fast, Constipation, Diarrhea, ShortFlame, Super, ShortFuse, Swap, Reversed,
};

struct PlayerInput {
    bool up = false, down = false, left = false, right = false;
    bool action1 = false;  // drop bomb / spooge
    bool action2 = false;  // throw > grab > trigger-detonate > punch
};

struct TickInputs {
    std::array<PlayerInput, kMaxPlayers> players{};
};

}  // namespace bomber::sim
