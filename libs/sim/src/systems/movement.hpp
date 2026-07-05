#pragma once

#include "bomber/sim/state.hpp"

namespace bomber::sim {

// Player movement — a faithful port of the original per-pixel stepper
// (sub_41EC84 in BM95.EXE). See docs/re/facts.md "Player movement".
class MovementSystem {
public:
    explicit MovementSystem(State& s) : s_(s) {}

    // Step-on callback: invoked the instant a per-pixel step lands the player
    // exactly on a tile centre (both axes aligned), passing that tile. This is
    // the port of sub_41EC84's `v35 == -1` in-loop check — the original fires
    // the warphole/trampoline step-on DURING the walk, not after it. A plain
    // function pointer (no heap, deterministic) keeps the stepper body in the
    // .cpp; ctx carries the caller's state. See docs/re/stage-actors.md §5.
    using StepOnFn = void (*)(void* ctx, Player& p, int tx, int ty);

    // Moves one player for one tick in direction d (also sets facing).
    // Positions are integer field pixels; the speed budget is added per tick
    // and spent 100 units per one-pixel step, remainder carried over.
    void move(Player& p, Direction d) { move(p, d, 0, nullptr, nullptr); }

    // As above, plus a signed extra_budget added to (or subtracted from) this
    // tick's move budget before it is spent. Used by the conveyor
    // (StageActorSystem): a belt contributes getvalue(190+idx) 1/100-px units.
    // The disease speed factors apply only to the player's own speed, exactly
    // as in sub_41F29B where the conveyor term is added AFTER those factors.
    void move(Player& p, Direction d, std::int32_t extra_budget) {
        move(p, d, extra_budget, nullptr, nullptr);
    }

    // Full form: `on_center(ctx, p, tx, ty)` fires each per-pixel step that
    // settles the player exactly on tile (tx,ty)'s centre — the faithful
    // step-on trigger point (sub_41EC84 v35 == -1). Pass nullptr to skip it.
    void move(Player& p, Direction d, std::int32_t extra_budget, StepOnFn on_center, void* ctx);

private:
    State& s_;
};

}  // namespace bomber::sim
