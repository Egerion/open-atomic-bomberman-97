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
// trampoline and warphole are step-on triggers, so trampoline_after_move()/
// warphole_after_move() run right after the player has settled for the tick.
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
    static constexpr std::int32_t kWarpTicks = 18;
    static constexpr std::int32_t kWarpMid = kWarpTicks / 2;  // relocate here (9)

    // True while the player is mid-bounce (trampoline hop). The caller pauses
    // movement/input during the hop, mirroring the original's state-gated
    // behaviour (player state +78==5, guarded by sub_41DE63).
    bool bouncing(const Player& p) const { return p.bounce > 0; }

    // Advance a trampoline hop by one tick. Call for a bouncing player instead
    // of the normal movement turn.
    //
    // sub_41F29B state 5 (v86==5, ~23150; raw disasm 0x420280..0x42053f): the
    // hop is NOT an in-place freeze — at the APEX (the tick the frame counter
    // reaches getvalue(680)/2 == 15) the original RELOCATES the player to a
    // random nearby open tile. That relocation draws the sim RNG (two rand()%5
    // per attempt, up to 100 attempts), so it must run through State::rng here.
    // player_index is needed for the position update / to identify the player.
    // Non-const: it decrements the countdown, may draw RNG, and moves the player.
    void tick_bounce(Player& p, int player_index);

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

    // After the player has moved, if it is centred on a trampoline tile and not
    // already bouncing (nor still latched from a prior bounce on this same
    // tile), start the hop and emit TrampolineBounce (sub_41EC84 step-on branch,
    // sound 350). A one-shot latch (Player::tramp_latch), cleared when the player
    // leaves the tile, stops a player parked on the centre from re-bouncing every
    // tick — the original only re-fires on the stepper's centring (v35 == -1),
    // which needs the player to arrive. Returns true if a bounce started.
    bool trampoline_after_move(Player& p, int player_index);

    // After the player has moved, if it is centred on a warphole (and not
    // latched from a warp in progress), START the two-phase warp: set the 18-tick
    // warp countdown, the re-entry latch, and emit WarpUsed (sound 1330,
    // sub_41EC84 step-on). The player does NOT move yet — tick_warp relocates it
    // at the midpoint. Clears the latch once the player is no longer centred on a
    // warphole. Returns true if a warp started. No RNG — the exit is pre-resolved
    // into State::warp_dest_* at setup. See docs/re/stage-actors.md §5.
    bool warphole_after_move(Player& p, int player_index);

private:
    // Shared step-on triggers, called from BOTH the mid-walk callback
    // (on_step_center, the faithful sub_41EC84 v35 == -1 point) and the
    // post-walk safety nets above. Each is latched, so a single walk across a
    // tile centre fires exactly once. Return true if the warp/bounce started.
    bool start_warp(Player& p, int player_index, int tx, int ty);
    bool start_bounce(Player& p, int player_index, int tx, int ty);

    // Context handed to the MovementSystem step-on callback so the free
    // function can reach this system and the player index.
    struct StepOnCtx {
        StageActorSystem* self;
        int player_index;
    };
    // MovementSystem::StepOnFn — fires the warphole/trampoline trigger the
    // instant a per-pixel step centres the player on a tile (§5).
    static void on_step_center(void* ctx, Player& p, int tx, int ty);

    State& s_;
    MovementSystem& movement_;
};

}  // namespace bomber::sim
