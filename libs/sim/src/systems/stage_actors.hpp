#pragma once

#include "bomber/sim/state.hpp"
#include "systems/movement.hpp"

namespace bomber::sim {

// Stage "extra" actors: conveyors and trampolines (player-facing mechanics).
// A faithful port of the actor branches in the character update sub_41F29B and
// the per-pixel stepper sub_41EC84. See docs/re/stage-actors.md.
//
// Ownership of the tick step: this system is invoked from simulation.cpp's
// player turn. The conveyor is a MOVE-BUDGET contribution that must be applied
// in the SAME step as the input move (a belt speeds/slows a walking player and
// pushes a standing one), so move_on_actor() wraps the movement call. The
// trampoline is a step-on trigger, so trampoline_after_move() runs right after
// the player has settled for the tick.
//
// Dirarrows (type 0) and warpholes (type 1) are documented but deferred here
// (stage-actors.md §5); the bomb-side conveyor/dirarrow/warp/tramp reactions
// live in bombs.cpp (stage-actors.md §6).
class StageActorSystem {
public:
    StageActorSystem(State& s, MovementSystem& movement) : s_(s), movement_(movement) {}

    // True while the player is mid-bounce (trampoline hop). The caller pauses
    // movement/input during the hop, mirroring the original's state-gated
    // behaviour (player state +78==5, guarded by sub_41DE63).
    bool bouncing(const Player& p) const { return p.bounce > 0; }

    // Advance a trampoline hop by one tick. Call for a bouncing player instead
    // of the normal movement turn.
    void tick_bounce(Player& p) const {
        if (p.bounce > 0) --p.bounce;
    }

    // Move a player for the tick, folding in any conveyor under them. want_godir
    // is the resolved input direction (0=Up,1=Right,2=Down,3=Left) or -1 for no
    // input. Returns true if the player actually shifted this tick (for the
    // caller's kick-on-blocked check).
    //
    // sub_41F29B: on a conveyor, if the player HAS input the belt adds a bonus
    // when moving with it / a penalty against it; if the player has NO input the
    // belt FORCES its direction and pushes. It never overrides an active input.
    bool move_on_actor(Player& p, int want_godir, bool moving);

    // After the player has moved, if it is now centred on a trampoline tile and
    // not already bouncing, start the hop and emit TrampolineBounce (sub_41EC84
    // step-on branch, sound 350). Returns true if a bounce started.
    bool trampoline_after_move(Player& p, int player_index);

private:
    State& s_;
    MovementSystem& movement_;
};

}  // namespace bomber::sim
