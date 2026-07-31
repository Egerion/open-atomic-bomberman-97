#pragma once

#include "bomber/sim/state.hpp"
#include "systems/movement.hpp"

namespace bomber::sim {

// Stage "extra" actors — conveyors (type 2), trampolines (3), dirarrows (0) and
// warpholes (1) — the PLAYER-facing half. A faithful port of the actor branches
// in the character update sub_41F29B and the per-pixel stepper sub_41EC84. See
// docs/re/stage-actors.md.
//
// The conveyor is a MOVE-BUDGET contribution applied in the SAME step as the
// input move, so move_on_actor wraps the movement call. The trampoline and the
// warphole have exactly ONE trigger site, matching the binary: the step-on hook
// the per-pixel mover calls when its along-axis offset-to-tile-centre reads -1.
// There is deliberately NO post-tick "is the player standing on one" fallback —
// the original has no such path, and ours took players the original leaves alone.
//
// Dirarrows do NOT steer walking players; they re-steer sliding BOMBS only, and
// the whole bomb-side conveyor/dirarrow/warp reaction lives in bombs.cpp
// (stage-actors.md §5-6), reading the same hashed actor grids.
class StageActorSystem {
public:
    StageActorSystem(State& s, MovementSystem& movement) : s_(s), movement_(movement) {}

    // The whole warp is 18 ticks: 9 out then 9 in (the original's frame counter
    // advances past 8 before each state ends — sub_41F29B ~23215), relocating at
    // the midpoint and state-gated plus invulnerable throughout. The FIRST of the
    // 18 is the trigger tick itself, because sub_41F29B runs the mover — and so
    // the trigger — BEFORE the state dispatch (docs/re/player-turn.md §5).
    static constexpr std::int32_t kWarpTicks = 18;
    static constexpr std::int32_t kWarpMid = kWarpTicks / 2;  // relocate here

    bool bouncing(const Player& p) const { return p.bounce > 0; }

    // Advance a trampoline hop by one tick, instead of the normal movement turn.
    // The hop is NOT an in-place freeze: at the apex the original RELOCATES the
    // player to a random nearby open tile, which DRAWS the sim RNG (two rand()%5
    // per attempt, up to 100), so it must run through State::rng. Takes no player
    // index — the relocation is written straight to `p` and the accept test looks
    // only at the board, never at another player.
    void tick_bounce(Player& p);

    bool warping(const Player& p) const { return p.warp > 0; }

    // Advance a warp by one tick, instead of the normal movement turn. At the
    // out->in midpoint the player is relocated to the exit captured at step-on
    // (Player::warp_to_*), which is robust even if the trigger tick's walk slid
    // them off the warphole. No RNG.
    void tick_warp(Player& p) const;

    // Move a player for one sub-frame, folding in any conveyor under them.
    // `want_godir` is the resolved input direction or -1. Returns true if the
    // player actually shifted (for the caller's kick-on-blocked check).
    //
    // sub_41F29B: on a conveyor a player WITH input gets a bonus moving with the
    // belt and a penalty against it; a player with NO input is FORCED along it.
    // The belt never overrides an active input.
    //
    // on_pixel/pixel_ctx go straight to MovementSystem::move — the per-pixel
    // flame-death/pickup slot, which the original runs for BOTH the input-driven
    // and the belt-forced walk, since the mover is the same call either way.
    bool move_on_actor(Player& p, int want_godir, bool moving, std::int32_t delta_ms = kMsPerTick,
                       MovementSystem::PixelFn on_pixel = nullptr, void* pixel_ctx = nullptr);

private:
    // The step-on triggers, reached ONLY from on_step_center — i.e. only from
    // sub_41EC84's offset-to-centre == -1 point. True if the flight started.
    bool start_warp(Player& p, int player_index, int tx, int ty);
    bool start_bounce(Player& p, int player_index, int tx, int ty);

    struct StepOnCtx {
        StageActorSystem* self;
        int player_index;
    };
    // MovementSystem::StepOnFn. Both actors are gated identically here, as in the
    // binary — so a change to this hook is a change to both.
    static void on_step_center(void* ctx, Player& p, int tx, int ty);

    State& s_;
    MovementSystem& movement_;
};

}  // namespace bomber::sim
