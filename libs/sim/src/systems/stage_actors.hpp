#pragma once

#include "bomber/sim/state.hpp"
#include "systems/movement.hpp"

namespace bomber::sim {

// Stage "extra" actors: conveyors (type 2), trampolines (type 3), dirarrows
// (type 0) and warpholes (type 1) — the player-facing mechanics. A faithful
// port of the actor branches in the character update sub_41F29B and the
// per-pixel stepper sub_41EC84. See docs/re/stage-actors.md.
//
// Ownership of the tick step: this system is invoked from simulation.cpp's
// player turn. The conveyor is a MOVE-BUDGET contribution applied in the SAME
// step as the input move, so move_on_actor() wraps the movement call. The
// trampoline and the warphole have exactly ONE trigger site, matching the
// binary: the step-on hook the per-pixel mover calls when its along-axis
// offset-to-tile-centre reads -1 (MovementSystem::move's on_center — see its
// doc comment for the full reading of sub_41EC84). There is deliberately no
// post-tick "is the player standing on one" fallback: the original has no such
// path, and ours took players the original leaves alone.
//
// Dirarrows (type 0) do NOT steer walking players in the original — they only
// re-steer sliding BOMBS (bombs.cpp / sub_42331C). The bomb-side conveyor/
// dirarrow/warp reactions live in bombs.cpp (stage-actors.md §5-6), reading the
// same hashed actor grids.
class StageActorSystem {
public:
    StageActorSystem(State& s, MovementSystem& movement) : s_(s), movement_(movement) {}

    // The whole warp is 18 ticks: 9 warp-out then 9 warp-in (the original's
    // frame counter advances past 8 before each state ends — sub_41F29B
    // ~23215). The player relocates to the exit at the midpoint (out→in
    // boundary) and is state-gated + invulnerable for the full duration.
    //
    // The FIRST of those 18 is the tick the warp is triggered on: sub_41F29B
    // runs the mover (and so sub_41EC84's trigger) BEFORE the animation/state
    // dispatch — the mover's exit jumps straight to the dispatch head — so the
    // state-6 block already runs once on the triggering frame and its counter
    // reads 1 by the end of it. simulation.cpp therefore advances this timer
    // once at the end of the tick a warp/bounce starts; without that the whole
    // flight sat a tick late and ran a tick long.
    static constexpr std::int32_t kWarpTicks = 18;
    static constexpr std::int32_t kWarpMid = kWarpTicks / 2;  // relocate here (9)

    // True while the player is mid-bounce (trampoline hop). The caller pauses
    // movement/input during the hop, mirroring the original's state-gated
    // behaviour (player state +78==5, guarded by sub_41DE63).
    bool bouncing(const Player& p) const { return p.bounce > 0; }

    // Advance a trampoline hop by one tick. Call for a bouncing player instead
    // of the normal movement turn.
    //
    // sub_41F29B's state-5 branch (the +78 state word equal to 5, ~23150; raw
    // disassembly 0x420280..0x42053f): the
    // hop is NOT an in-place freeze — at the APEX (the tick the frame counter
    // reaches getvalue(680)/2 == 15) the original RELOCATES the player to a
    // random nearby open tile. That relocation draws the sim RNG (two rand()%5
    // per attempt, up to 100 attempts), so it must run through State::rng here.
    // Takes no player index: the relocation is written straight to `p`, and the
    // accept test looks only at the board (solid / bomb), never at another
    // player — so, like tick_warp, there is nothing here a slot number answers.
    // Non-const: it decrements the countdown, may draw RNG, and moves the player.
    void tick_bounce(Player& p);

    // True while the player is mid-warp (warp-out or warp-in). Like a bounce the
    // caller pauses movement/input for the whole warp; the player is invulnerable
    // (states 6/7). See docs/re/stage-actors.md §5.
    bool warping(const Player& p) const { return p.warp > 0; }

    // Advance a warp by one tick. At the warp-out→warp-in midpoint the player is
    // relocated to the exit tile captured at step-on (Player::warp_to_*, the
    // original's stored +20/+24) — robust even if the trigger tick's walk slid
    // the player off the warphole. No RNG. Call for a warping player instead of
    // the normal movement turn.
    void tick_warp(Player& p) const;

    // Move a player for the tick, folding in any conveyor under them. want_godir
    // is the resolved input direction (0=Up,1=Right,2=Down,3=Left) or -1 for no
    // input. Returns true if the player actually shifted this tick (for the
    // caller's kick-on-blocked check).
    //
    // sub_41F29B: on a conveyor, if the player HAS input the belt adds a bonus
    // when moving with it / a penalty against it; if the player has NO input the
    // belt FORCES its direction and pushes. It never overrides an active input.
    // (Dirarrows do not affect players — see the class comment.)
    //
    // on_pixel/pixel_ctx are handed straight to MovementSystem::move — the
    // per-pixel flame-death/pickup slot (sub_41EC84 22699-22717), which the
    // original runs for BOTH the input-driven and the belt-forced walk (the
    // mover is the same call either way).
    // delta_ms is the sub-frame quantum (constants.hpp kSubFrameMs) forwarded
    // to MovementSystem::move's per-frame budget accrual.
    bool move_on_actor(Player& p, int want_godir, bool moving, std::int32_t delta_ms = kMsPerTick,
                       MovementSystem::PixelFn on_pixel = nullptr, void* pixel_ctx = nullptr);

private:
    // The step-on triggers, reached ONLY from on_step_center — i.e. only from
    // sub_41EC84's along-axis offset-to-centre == -1 point. Return true if the
    // warp/bounce started.
    bool start_warp(Player& p, int player_index, int tx, int ty);
    bool start_bounce(Player& p, int player_index, int tx, int ty);

    // Context handed to the MovementSystem step-on callback so the free
    // function can reach this system and the player index.
    struct StepOnCtx {
        StageActorSystem* self;
        int player_index;
    };
    // MovementSystem::StepOnFn — the warphole/trampoline trigger, fired by the
    // per-pixel mover one pixel short of a tile centre on the axis of travel
    // (§5). Both actors are gated identically here, as in the binary.
    static void on_step_center(void* ctx, Player& p, int tx, int ty);

    State& s_;
    MovementSystem& movement_;
};

}  // namespace bomber::sim
