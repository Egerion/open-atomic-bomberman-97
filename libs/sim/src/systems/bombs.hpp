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

    // Spooger: with an OWN bomb already underfoot, spray a run of bombs one
    // tile at a time in the facing direction, stopping at a live player, a
    // powerup, a wall/bomb/field edge, or when the bomb supply runs out
    // (sub_41F29B spooge branch — the whole run is laid within a single
    // frame). The k-th bomb of the run gets a k-tick longer fuse (sub_41EB13
    // is called with the run index; sub_422EDE inits fuse-elapsed to -50*k ms),
    // so the line detonates as a cascade, one tile per tick.
    void spooge_ahead(Player& p, std::uint8_t owner);

    // Kicks the bomb ahead of the player if the path beyond it is clear
    // (sub_41EC84 `!v35` branch -> sub_424708 -> sub_42464B). A RESTING bomb
    // starts sliding; a bomb already SLIDING in another direction is snapped
    // to its tile centre and redirected; one sliding in the same direction is
    // a silent no-op (no event — sub_42464B only plays sound 120 for the
    // first two cases). `who` is the kicking player (event bookkeeping only).
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

    // Detonates the player's oldest grounded trigger bomb — resting OR
    // sliding; only carried/flying bombs are exempt (sub_424B41 excludes
    // motion states 2 and 3 only). True if one went off.
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
    // `fuse_stagger` is the spooge run index (sub_422EDE's a8, fuse-elapsed
    // init -50*k ms == +k ticks of countdown); 0 for a normal drop.
    void place(Player& p, std::uint8_t owner, int tx, int ty, int fuse_stagger = 0);

    // Sends a bomb into the air toward the tile `tiles` cells away in dir.
    // Coordinates stay unwrapped during flight; landing wraps the field.
    void launch(Bomb& b, Direction d, int tiles, std::int32_t arc);

    // Advances an airborne bomb; on arrival it settles on an open tile or
    // makes another one-tile hop (wrapping around the field edges).
    void fly(Bomb& b);

    // Advances a kicked bomb, stopping tile-aligned when blocked ahead. Takes
    // the bomb's index so it can detonate the bomb (via FlameSystem) when it
    // slides onto a flaming tile (sub_42331C flame check, sub_42708D). Also
    // applies the stage-actor reactions at each tile centre: a DIRARROW re-steers
    // the bomb (sub_42331C ~25532) and a WARPHOLE teleports it (stage-actors.md
    // §6). `belt` is the per-tick move budget (belt speed for a conveyor-carried
    // bomb, else kicked_bomb_speed).
    void slide(std::size_t index, std::int32_t budget);

    // A resting bomb sitting on a conveyor tile is pushed along the belt at the
    // belt speed (sub_42331C case 0, getvalue(190+idx)). Sets it moving in the
    // belt direction so slide() carries it; a bomb already moving is left alone.
    void conveyor_carry(std::size_t index);

    State& s_;
    FlameSystem& flames_;
    PowerupSystem& powerups_;
};

}  // namespace bomber::sim
