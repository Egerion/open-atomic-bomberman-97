#pragma once

#include <cstddef>
#include <cstdint>

#include "bomber/sim/state.hpp"

namespace bomber::sim {

class PowerupSystem;

// Explosions and their aftermath: flame spread, chain reactions, brick burning,
// and the per-tick fade of flames and crumbling bricks.
class FlameSystem {
public:
    // powerups: the skull-relocation compensation, when a flame destroys a
    // Disease token while diseases_destroyable is off, reuses
    // PowerupSystem::scatter — the same sub_4255B2 the head-hit drop uses.
    FlameSystem(State& s, PowerupSystem& powerups) : s_(s), powerups_(powerups) {}

    // Detonates bombs[bomb_index] (a no-op if already inactive): frees the
    // owner's slot, spreads flame in all four directions skipping `skip_dir` when
    // >= 0, and QUEUES — rather than immediately detonating — any bomb its arm
    // reaches. skip_dir lets a chain-triggered bomb avoid re-blasting back toward
    // the flame that triggered it (bomb+56, sub_42331C ~25621/25645).
    void explode(std::size_t bomb_index, int skip_dir = -1);

    // Marks bomb_id for forced detonation at the next drain (sub_423209).
    // Whether that is THIS tick or the next depends on where the caller sits
    // relative to the drain: BombSystem::detonate_triggered resolves the same
    // tick, a flame-arm/slide/landing hit waits (facts.md "Chain-reaction
    // timing"). Re-queueing before the drain overwrites skip_dir, last push wins,
    // matching the original's unconditional per-entry overwrite of bomb+56.
    void queue_chain(std::uint32_t bomb_id, int skip_dir = -1);

    // Tick step, right after players act and before bombs move: explode every
    // still-active queued bomb, in ascending bomb order.
    void drain_chain_queue();

    // Tick step: flames fade, crumbling bricks finish. A brick tile is left Brick
    // (blocking) at ignition — see spread_to — so this is also where it opens up.
    void age_flames_and_bricks();

private:
    // The exploding bomb's own tile, ignited unconditionally (sub_42331C's
    // epicentre block): no occupancy stop applies here, only to the arm.
    //
    // `colour` travels alongside `owner` on both igniters because the original's
    // flame-cell init sub_426FCC takes them as separate arguments — colour from
    // the bomb's +60 byte, owner from its +62 word — and stores both in the cell.
    bool ignite_epicentre(int tx, int ty, std::uint8_t owner, std::uint8_t colour);

    // A flame ARM reaches (tx,ty) travelling in `from_dir`. `is_last_of_reach` is
    // whether this is the last tile of the bomb's FULL configured reach —
    // true only on the final loop iteration, regardless of whether the arm
    // actually gets that far — and decides tip vs mid if this tile ignites as a
    // plain flame cell. Returns true if the arm continues past this cell.
    bool spread_to(int tx, int ty, std::uint8_t owner, std::uint8_t colour, Direction from_dir,
                   bool is_last_of_reach);

    // Destroys any floor powerup at (tx,ty), with the diseases_destroyable
    // skull-relocation compensation. Shared by the epicentre and the arm.
    void burn_powerup_here(int tx, int ty);

    // A brick at (tx,ty) just ignited and still hides a token: if that token is
    // "over-powerful" and the match is still within its opening window, relocate
    // the record instead of letting it reveal here. The caller's own
    // hidden->floor reveal check runs AFTER this and does the right thing either
    // way. See the definition for the sub_425107 citation.
    void relocate_overpowered_here(int tx, int ty);

    State& s_;
    PowerupSystem& powerups_;
};

}  // namespace bomber::sim
