#pragma once

#include <cstddef>
#include <cstdint>

#include "bomber/sim/state.hpp"

namespace bomber::sim {

class FlameSystem;
class PowerupSystem;

// Everything a bomb does between placement and detonation: dropping, the
// spooger spray, kicking, punching/throwing (airborne flight incl. head
// hits), grabbing, trigger detonation, and fuse ticking.
class BombSystem {
public:
    BombSystem(State& s, FlameSystem& flames, PowerupSystem& powerups)
        : s_(s), flames_(flames), powerups_(powerups) {}

    // Lays a bomb on the player's tile if it is free and a slot is available.
    void drop(Player& p, std::uint8_t owner);

    // Spooger: with a bomb already underfoot, spray a run of bombs one tile at
    // a time in the facing direction, stopping at a wall, another bomb, a
    // powerup, the field edge, or when the bomb supply runs out (sub_41F29B
    // spooge branch — the whole run is laid within a single frame).
    void spooge_ahead(Player& p, std::uint8_t owner);

    // Kicks the resting bomb ahead of the player if the path is clear. `who`
    // is the kicking player (recorded in the BombKicked event for the kick
    // animation; events are unhashed so this does not affect determinism).
    void try_kick(Player& p, Direction d, int who);

    // Punches the bomb ahead three tiles into the air.
    void try_punch(Player& p, std::uint8_t who);

    // Picks up the bomb underfoot (grab glove). Returns true if it acted.
    bool try_grab(Player& p, int who);

    // Throws the carried bomb three tiles ahead (same arc as a punch).
    void throw_carried(Player& p, int who);

    // Detonates the player's oldest resting trigger bomb. True if one went off.
    bool detonate_triggered(int owner);

    // Tick step 2: airborne bombs fly (landing on a head stuns and scatters),
    // kicked bombs slide.
    void advance_bombs();

    // Tick step 3: fuses burn down (paused while airborne) and detonate.
    void tick_fuses();

private:
    // Creates one bomb on tile (tx,ty) carrying the player's flame/fuse, with
    // the disease overrides applied at drop time (sub_41EB13): short-fuse
    // thirds the timer, short-flame clamps the blast to one cell.
    void place(Player& p, std::uint8_t owner, int tx, int ty);

    // Sends a bomb into the air toward the tile `tiles` cells away in dir.
    // Coordinates stay unwrapped during flight; landing wraps the field.
    void launch(Bomb& b, Direction d, int tiles, std::int32_t arc);

    // Advances an airborne bomb; on arrival it settles on an open tile or
    // makes another one-tile hop (wrapping around the field edges).
    void fly(Bomb& b);

    // Advances a kicked bomb, stopping tile-aligned when blocked ahead. Takes
    // the bomb's index so it can detonate the bomb (via FlameSystem) when it
    // slides onto a flaming tile (sub_42331C flame check, sub_42708D).
    void slide(std::size_t index);

    State& s_;
    FlameSystem& flames_;
    PowerupSystem& powerups_;
};

}  // namespace bomber::sim
