#pragma once

#include <cstddef>
#include <cstdint>

#include "bomber/sim/state.hpp"

namespace bomber::sim {

class PowerupSystem;

// Explosions and their aftermath: flame spread, chain reactions, brick
// burning, and the per-tick fade of flames / crumbling bricks.
class FlameSystem {
public:
    // powerups: the skull-relocation compensation when a flame destroys a
    // Disease token while diseases_destroyable is off reuses
    // PowerupSystem::scatter (the same sub_4255B2 the head-hit drop uses).
    FlameSystem(State& s, PowerupSystem& powerups) : s_(s), powerups_(powerups) {}

    // Detonates the bomb at bombs[bomb_index] (no-op if already inactive):
    // frees the owner's slot, spreads flame in all four directions (skipping
    // `skip_dir` when >= 0, a godir 0-3), and QUEUES — not immediately
    // detonates — any bomb its own arm reaches (see queue_chain). skip_dir
    // lets a chain-triggered bomb avoid re-blasting back toward the flame
    // that triggered it (bomb+56, sub_42331C ~25621/25645); every other
    // caller uses the default (no restriction). docs/re/facts.md "Chain-
    // reaction timing".
    void explode(std::size_t bomb_index, int skip_dir = -1);

    // Marks bomb_id for forced detonation at the next drain_chain_queue()
    // call (sub_423209): used when a flame arm reaches another bomb, a
    // flying bomb lands on flame, a trigger bomb is remote-detonated, or a
    // sliding bomb enters flame. Whether that is THIS tick or the NEXT one
    // depends on where in the tick the caller sits relative to the drain —
    // see the callers (BombSystem::detonate_triggered resolves the same
    // tick; a flame-arm/slide/landing hit waits for the next one) and
    // docs/re/facts.md "Chain-reaction timing". Queueing the same bomb again
    // before it drains overwrites its skip_dir (last push wins), matching
    // the original's unconditional per-entry overwrite of bomb+56.
    void queue_chain(std::uint32_t bomb_id, int skip_dir = -1);

    // Tick step (right after players act, before bombs move): drains the
    // pending-chain queue, exploding every still-active queued bomb in
    // ascending bomb order. See docs/re/facts.md "Chain-reaction timing" for
    // which pushes this catches (same tick) versus defers to next time.
    void drain_chain_queue();

    // Tick step: flames fade; crumbling bricks finish. A brick tile is left
    // Brick (blocking) at ignition — see spread_to — so this is also where
    // it finally opens up.
    void age_flames_and_bricks();

private:
    // Ignites the exploding bomb's own tile unconditionally (sub_42331C
    // epicentre block). Distinct from spread_to: no bomb/powerup occupancy
    // stop applies here, only to the extending arm.
    // `colour` alongside `owner` on both igniters: the original's flame-cell
    // init (sub_426FCC) takes them as separate arguments (colour from the
    // bomb's +60 byte, owner from its +62 word) and stores both in the cell
    // record — see Bomb::colour.
    bool ignite_epicentre(int tx, int ty, std::uint8_t owner, std::uint8_t colour);

    // A flame ARM reaches (tx,ty), travelling in direction `from_dir`
    // (sub_42331C per-direction loop). `is_last_of_reach` is whether this is
    // the LAST tile of the bomb's FULL configured reach (true only on the
    // final loop iteration, regardless of whether the arm actually gets this
    // far before something stops it) — it decides tip vs. mid if this tile
    // ends up igniting as a plain flame cell (see FlameKind's doc comment).
    // Returns true if the arm continues past this cell, false if it stops
    // here (bomb chain-queued, powerup burned, solid wall, or brick
    // ignited).
    bool spread_to(int tx, int ty, std::uint8_t owner, std::uint8_t colour,
                   Direction from_dir, bool is_last_of_reach);

    // Destroys any floor powerup at (tx,ty), with the diseases_destroyable
    // skull-relocation compensation. Shared by the epicentre and the arm.
    void burn_powerup_here(int tx, int ty);

    // A brick at (tx,ty) just ignited (or re-ignited) and (tx,ty) is still
    // hiding a token: if that token is Punch/Grab/SuperDisease ("over-
    // powerful" powers, VALUELST id 102's own comment) and the match is
    // still within its opening overpowered_relocate_seconds, relocate the
    // record elsewhere instead of letting it reveal here (sub_425107's
    // early gated branch, pseudo.c 26295-26336). No-op otherwise. Mutates
    // s.hidden/s.floor at (tx,ty) and (if relocated) at the target tile;
    // the caller's own hidden->floor reveal check runs AFTER this and does
    // the right thing either way — see docs/re/facts.md "Overpowered-
    // powerup relocation".
    void relocate_overpowered_here(int tx, int ty);

    State& s_;
    PowerupSystem& powerups_;
};

}  // namespace bomber::sim
