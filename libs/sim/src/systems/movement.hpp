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
    // the port of sub_41EC84's in-loop check for an offset-to-centre of -1 (the
    // pixel before the centre) — the original fires
    // the warphole/trampoline step-on DURING the walk, not after it. A plain
    // function pointer (no heap, deterministic) keeps the stepper body in the
    // .cpp; ctx carries the caller's state. See docs/re/stage-actors.md §5.
    using StepOnFn = void (*)(void* ctx, Player& p, int tx, int ty);

    // Per-pixel field callback: invoked after EVERY committed pixel step (the
    // post-commit slot of sub_41EC84's loop body, pseudo.c 22699-22717), where
    // the original checks flame death and powerup pickup at the tile of the
    // player's current pixel position. Returns true if the player DIED — the
    // mover then abandons the remaining budget and returns, exactly like the
    // original's mid-loop `return 1` (a killed player never finishes the walk
    // and never reaches the same turn's bomb actions). See docs/re/facts.md
    // "Per-tick call order — END-TO-END" finding 1.
    using PixelFn = bool (*)(void* ctx, Player& p);

    // Moves one player for ONE SUB-FRAME of delta_ms in direction d (also sets
    // facing). Positions are integer field pixels; the budget accrues
    // `frame_budget(speed, delta_ms)` per call (the original's per-displayed-
    // frame `speed × frameDelta / 50`) and is spent 100 units per one-pixel
    // step, remainder carried over. The caller (player_turn) invokes this once
    // per canonical sub-frame (constants.hpp kSubFrameMs).
    void move(Player& p, Direction d, std::int32_t delta_ms = kMsPerTick) {
        move(p, d, 0, nullptr, nullptr, true, nullptr, nullptr, delta_ms);
    }

    // Full form: `on_center(ctx, p, tx, ty)` fires each per-pixel step that
    // settles the player exactly on tile (tx,ty)'s centre — the faithful
    // step-on trigger point (sub_41EC84's offset-to-centre check for -1). Pass
    // nullptr to skip it.
    //
    // extra_budget is the conveyor term, getvalue(190+idx) 1/100-px units
    // SIGNED (with/against the belt), delta-scaled inside like the speed term
    // but never disease-scaled — sub_41F29B adds it AFTER those factors.
    //
    // use_player_speed selects whether the player's own speed (p.speed, disease-
    // scaled) is folded into the budget. sub_41F29B only adds it when the player
    // HAS a movement input (its case (b)); when the conveyor FORCES movement with
    // no input (case (a)) the budget is exactly the belt term, no speed term
    // at all. Defaults to true for every ordinary (player-initiated) move.
    // `on_pixel(pixel_ctx, p)` fires after every committed pixel step (flame
    // death + pickup live there — sub_41EC84 22699-22717); a true return kills
    // the walk (death mid-move). Pass nullptr to skip.
    void move(Player& p, Direction d, std::int32_t extra_budget, StepOnFn on_center, void* ctx,
              bool use_player_speed = true, PixelFn on_pixel = nullptr, void* pixel_ctx = nullptr,
              std::int32_t delta_ms = kMsPerTick);

    // Ice / input-lag (VALUELST ids 450-460, Hockey Rink; docs/re/facts.md
    // "Ice / input-lag", sub_41F29B ~23058-23078). Pushes this SUB-FRAME's
    // desired direction (`want_godir`: -1 = none, 0..3 = Up/Right/Down/Left)
    // into `p.ice_history` and returns the EFFECTIVE direction to actually
    // move with: the oldest-needed sample whose age has reached the level's
    // ice_delay_ms. The original pushes once per DISPLAYED frame; we push once
    // per canonical sub-frame, so a slot ages by kMsPerTick/kSubFrames ≈ 5.6 ms
    // and the 30-slot buffer spans only ~167 ms — deliberately short of the
    // nominal 250 ms, for the reasons the .cpp gives in full. Safe
    // to call unconditionally every sub-frame for every player: AI players
    // are exempt in the original (gated on the player-type byte +16 != 1) and
    // are returned unchanged with the buffer untouched; on every level but
    // Hockey Rink ice_delay_ms is 0, so this returns want_godir unchanged
    // WITHOUT writing the buffer — keeping `p.ice_history` a fixed all-zero
    // hashed field there (see player.hpp).
    int ice_delay(Player& p, int want_godir) const;

private:
    State& s_;
};

}  // namespace bomber::sim
