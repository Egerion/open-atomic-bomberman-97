#pragma once

#include "bomber/sim/state.hpp"

namespace bomber::sim {

// Player movement — a faithful port of the original per-pixel stepper
// (sub_41EC84 in BM95.EXE). See docs/re/facts.md "Player movement".
class MovementSystem {
public:
    explicit MovementSystem(State& s) : s_(s) {}

    // Step-on callback: invoked when a per-pixel step is one pixel SHORT of a
    // tile centre along the TRAVEL AXIS, passing that tile — pre-step, and
    // without consulting the perpendicular axis. NOT "lands exactly on the
    // centre": testing the POST-step position is why this port used to grab
    // players the original does not (the retraction at the head of movement.cpp).
    using StepOnFn = void (*)(void* ctx, Player& p, int tx, int ty);

    // Per-pixel field callback, invoked after EVERY committed pixel step, where
    // the original checks flame death and powerup pickup. Returns true if the
    // player DIED, which abandons the remaining budget — so a killed player never
    // finishes the walk and never reaches the same turn's bomb actions.
    using PixelFn = bool (*)(void* ctx, Player& p);

    // One sub-frame's move request — a parameter object rather than a
    // nine-argument signature (docs/coding-standards.md §3). Each field is one of
    // the terms sub_41F29B hands its mover.
    struct MoveRequest {
        Direction dir{};
        std::int32_t delta_ms = kMsPerTick;
        // The conveyor term, getvalue(190+idx), SIGNED with or against the belt.
        std::int32_t extra_budget = 0;
        // sub_41F29B folds the player's own speed in ONLY when the player
        // supplied a direction — its case (b). A conveyor forcing an idle player
        // (case (a)) never reads it at all (docs/re/stage-actors.md §3).
        bool use_player_speed = true;
        StepOnFn on_center = nullptr;  // nullptr skips the step-on trigger
        void* center_ctx = nullptr;
        PixelFn on_pixel = nullptr;  // nullptr skips the flame-death/pickup check
        void* pixel_ctx = nullptr;
    };

    // Moves one player for ONE SUB-FRAME (also sets facing). Positions are
    // integer field pixels; the budget accrues frame_budget(speed, delta_ms) per
    // call and is spent 100 units per one-pixel step, remainder carried over.
    void move(Player& p, const MoveRequest& req);

    void move(Player& p, Direction d, std::int32_t delta_ms = kMsPerTick) {
        move(p, MoveRequest{d, delta_ms});
    }

    // Ice / input-lag (VALUELST ids 450-460, Hockey Rink). Pushes this
    // SUB-FRAME's desired direction into p.ice_history and returns the EFFECTIVE
    // direction to move with. Safe to call unconditionally for every player:
    // an AI, and any level with ice_delay_ms == 0, is returned unchanged with the
    // buffer UNTOUCHED — which is what keeps p.ice_history a fixed all-zero
    // hashed field elsewhere. See the .cpp for the citations and the deliberately
    // short buffer span.
    int ice_delay(Player& p, int want_godir) const;

private:
    State& s_;
};

}  // namespace bomber::sim
