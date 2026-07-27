#pragma once

#include <cstdint>

#include "bomber/sim/constants.hpp"
#include "bomber/sim/types.hpp"

namespace bomber::sim {

// One bomb. Plain aggregate — see the determinism note in player.hpp.
struct Bomb {
    bool active = false;
    // Stable identity assigned once at creation (State::next_bomb_id),
    // never reused — lets the chain-detonation queue re-find this exact
    // bomb one tick later even though `State::bombs` compacts dead entries
    // every tick (a raw vector index would go stale). docs/re/facts.md
    // "Chain-reaction timing".
    std::uint32_t id = 0;
    std::uint8_t owner = 0;
    // Render colour, a SEPARATE field from `owner`: the original packs both
    // into the dword at bomb +60 — low byte = the placer's colour, written
    // once at creation by sub_422EDE; high word (+62) = the owner
    // id. A flame-arm chain hit copies ONLY the +62 owner word from the
    // detonating bomb into the bomb it ignites (sub_42331C, pseudo.c 25644), so kill
    // credit moves to the chainer while the bomb — and every flame it casts
    // (sub_426FCC takes colour from bomb +60 and owner from +62 separately)
    // — keeps wearing the original placer's colour. Stored as the placer's
    // SLOT here and resolved to a palette via render_colour at draw time
    // (identical, since a player's colour never changes mid-match).
    // docs/re/facts.md "Bomb/flame colour is not the owner".
    std::uint8_t colour = 0;
    Fixed x = 0, y = 0;           // center, aligned to tile unless moving
    std::int32_t fuse = 0;        // ticks until detonation (<0: waits for trigger)
    // The fuse DURATION this bomb was created with (sub_422EDE stores it at
    // word +74 for EVERY kind, trigger included; our running `fuse` is the
    // original's elapsed counter +68 recast as a countdown). Two consumers,
    // both faithful ports: a thrown carried bomb restarts from this value
    // (sub_41F29B's bomb-action tail zeroes elapsed +68 before the launch), and a
    // trigger bomb downgraded by a Trigger EVICTION relights with it
    // (sub_424C47 sets kind 0, elapsed 0). facts.md "Core-feel audit" §2/§5.
    std::int32_t fuse_init = 0;
    // Creation-tick stamp (the original's bomb +64, set to dword_464994 at
    // placement, sub_422EDE pseudo.c 25113). Used ONLY by trigger detonation:
    // sub_424B41 (pseudo.c 26036) requires a candidate's stamp be STRICTLY
    // earlier than the current tick, so a trigger bomb placed the SAME tick as
    // the trigger-detonate press cannot fire yet. facts.md-adjacent bombs.md
    // finding 4. Hashed (gameplay state per determinism rule 4).
    std::uint64_t created_tick = 0;
    std::int32_t dud_left = 0;    // fizzle ticks remaining (dud state; fuse frozen)
    std::int32_t flame = 2;
    bool jelly = false;
    bool trigger = false;
    bool moving = false;          // kicked
    // Kick+action2 "stop my bombs" (sub_4247C5 sets bomb byte +57 on the
    // owner's sliding non-jelly bombs): the slide loop consumes it by snapping
    // the bomb onto the next tile centre it reaches (sub_42331C fires it when
    // +57 is set and the bomb is at or past that centre). A DIRARROW clears it
    // (~25535). facts.md "Core-feel audit" §4.
    bool stop_pending = false;
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
