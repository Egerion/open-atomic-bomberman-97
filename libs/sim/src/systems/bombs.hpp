#pragma once

#include <cstddef>
#include <cstdint>

#include "bomber/sim/state.hpp"

namespace bomber::sim {

class FlameSystem;
class PowerupSystem;

// Everything a bomb does between placement and detonation: dropping, the
// spooger spray, kicking, punching/throwing (airborne flight incl. head hits),
// grabbing, trigger detonation, and fuse ticking. Each member cites the
// sub_XXXX it ports at its definition.
class BombSystem {
public:
    BombSystem(State& s, FlameSystem& flames, PowerupSystem& powerups)
        : s_(s), flames_(flames), powerups_(powerups) {}

    // Lays a bomb on the player's tile if it is free and a slot is available.
    void drop(Player& p, std::uint8_t owner);

    // Spooger: with an OWN bomb already underfoot, spray a run of bombs one tile
    // at a time in the facing direction, the whole run within a single frame.
    // The k-th bomb gets a k-tick longer fuse, so the line detonates as a
    // cascade, one tile per tick.
    void spooge_ahead(Player& p, std::uint8_t owner);

    // Kicks the bomb ahead of the player if the path beyond it is clear. `who`
    // is the kicking player (event bookkeeping only).
    void try_kick(Player& p, Direction d, int who);

    // Kick+action2 (sub_4247C5): flags every one of the owner's SLIDING,
    // non-jelly bombs to stop at the next tile centre it reaches.
    void stop_own_sliding(int owner);

    // Punches the bomb ahead three tiles into the air.
    void try_punch(Player& p, std::uint8_t who);

    // Picks up the bomb underfoot (grab glove). Returns true if it acted.
    bool try_grab(Player& p, int who);

    // Throws the carried bomb three tiles ahead (same arc as a punch).
    void throw_carried(Player& p, int who);

    // Queues the player's oldest grounded trigger bomb for forced detonation via
    // the SAME pending-chain queue a flame-arm chain uses — but this one still
    // resolves THIS tick, since it is queued during the player pass, which
    // precedes the once-per-tick drain (facts.md "Chain-reaction timing").
    bool detonate_triggered(int owner);

    // Tick step 5: airborne bombs fly (landing on a head stuns and scatters),
    // kicked bombs slide, resting bombs ride a belt.
    void advance_bombs();

    // Tick step 6: fuses burn down (paused while airborne) and detonate.
    void tick_fuses();

private:
    // Creates one bomb on tile (tx,ty) carrying the player's flame/fuse, with the
    // disease overrides applied at drop time. `fuse_stagger` is the spooge run
    // index (0 for a normal drop).
    void place(Player& p, std::uint8_t owner, int tx, int ty, int fuse_stagger = 0);

    // Sends a bomb into the air toward the tile `tiles` cells away in dir.
    // Coordinates stay unwrapped during flight; landing wraps the field.
    void launch(Bomb& b, Direction d, int tiles, std::int32_t arc);

    // Advances an airborne bomb; on arrival it settles on an open tile or makes
    // another one-tile hop.
    void fly(Bomb& b);

    // Advances a kicked or belt-carried bomb, stopping tile-aligned when blocked
    // ahead. `budget` is the per-tick move budget.
    void slide(std::size_t index, std::int32_t budget);

    // sub_4230A5's cell-entry probe for the tile a sliding bomb is about to
    // enter. Has the SIDE EFFECT of squashing a powerup there — see the
    // definition; the probe order is load-bearing.
    bool cell_blocks_slide(int nx, int ny);
    void squash_powerup(int nx, int ny);

    // A resting bomb sitting on a conveyor tile is pushed one tick's worth along
    // the belt; a kicked/flying bomb is left to advance_bombs.
    void conveyor_carry(std::size_t index);

    State& s_;
    FlameSystem& flames_;
    PowerupSystem& powerups_;
};

}  // namespace bomber::sim
