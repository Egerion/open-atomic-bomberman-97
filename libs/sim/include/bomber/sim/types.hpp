#pragma once

#include <array>
#include <cstdint>

#include "bomber/sim/constants.hpp"

// Plain enumerations and input records shared across the simulation.
// Everything here is trivially copyable by design (ADR-0003): the whole
// gameplay state must remain hashable and snapshot-friendly.

namespace bomber::sim {

enum class Cell : std::uint8_t { Blank, Brick, Solid };

// Stage "extra" actors placed from EXTRA<N>.RES (see docs/re/stage-actors.md).
// The integer values MIRROR the original's actor+4 type field so tables and
// hashes stay legible: 0=DirArrow, 1=Warphole, 2=Conveyor, 3=Trampoline.
enum class ActorType : std::uint8_t {
    None = 255,
    DirArrow = 0,
    Warphole = 1,
    Conveyor = 2,
    Trampoline = 3,
};

// Order matches the scheme -P table and VALUELST id blocks (50/400/550).
enum class PowerupType : std::uint8_t {
    ExtraBomb, Flame, Disease, Kick, Skate, Punch, Grab, Spooger,
    Goldflame, Trigger, Jelly, SuperDisease, Random,
    None = 255,
};

enum class Direction : std::uint8_t { Up, Down, Left, Right };

// A flame arm's drawn PIECE, mirroring the original's per-flame-cell "kind"
// byte and its off_45BEA0 name table (`sub_42331C`'s arm-cast loop,
// `sub_426D06`'s per-tick animator) — the integer values MIRROR that table so
// they stay legible: 0-3 = tips in compass order, 4-7 = mids in the SAME
// compass order, 8 = the epicentre. Decided once at ignition from the arm's
// own cast direction and position-within-reach — see docs/re/facts.md "Flame
// arm-shape selection". Meaningless where `State::flame` is 0.
enum class FlameKind : std::uint8_t {
    TipNorth, TipEast, TipSouth, TipWest,
    MidNorth, MidEast, MidSouth, MidWest,
    Center,
};

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
