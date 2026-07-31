// The computer-player AI's dispatcher entry and PlayerInput adapters (ADR-0005,
// docs/re/ai.md). The rest of the subsystem is split across ai_grids.cpp
// (danger/obstacle grids + the tile predicates), ai_pathfind.cpp (the 3 BFS
// variants + the flame veto) and ai_behaviours.cpp (the 8 behave_* + the enemy
// finder); the shared godir tables live in grid.hpp, the API in ai.hpp.
//
// Integer only; RNG only via State::rng in the RE'd order/count (ai.md §8).

#include "systems/ai.hpp"

#include "bomber/sim/rng.hpp"
#include "grid.hpp"

namespace bomber::sim {

void AISystem::write_move(PlayerInput& out, int godir) {
    out.up = out.down = out.left = out.right = false;
    if (godir < 0) return;
    // grid::from_godir is the godir -> Direction map (it applies the same & 3).
    switch (grid::from_godir(godir)) {
        case Direction::Up: out.up = true; break;
        case Direction::Right: out.right = true; break;
        case Direction::Down: out.down = true; break;
        case Direction::Left: out.left = true; break;
    }
}

// The bomb-key edge -> action1. player_turn's drop block is edge-gated on
// action1 && !prev_action1, and the original's behaviours write BOTH halves of
// that test themselves: `+56 = 1` AND `+54 = 0` (docs/re/ai.md §7, facts.md
// "AI key presses manufacture their own edge"). Clearing prev_action1 is the
// +54 write — it is what lets a behaviour that fires on back-to-back frames
// keep producing edges instead of holding a key nothing can re-trigger.
void AISystem::press_bomb(int i, PlayerInput& out) {
    out.action1 = true;
    s_.players[i].prev_action1 = false;  // the original's +54 = 0
}

// The bomb-key release (+56 = 0; +54 = 0) — behaviour 0's carrying branch.
void AISystem::release_bomb(int i, PlayerInput& out) {
    out.action1 = false;
    s_.players[i].prev_action1 = false;  // the original's +54 = 0
}

// The action-key edge (+57 = 1; +55 = 0) -> action2, routed to punch (+91) /
// trigger (+95). Behaviour 1 only; see press_action_sustained for behaviour 2.
void AISystem::press_action(int i, PlayerInput& out) {
    out.action2 = true;
    s_.players[i].prev_action2 = false;  // the original's +55 = 0
}

// The action key set WITHOUT the paired +55 clear — behaviour 2's whim alone.
void AISystem::press_action_sustained(PlayerInput& out) {
    out.action2 = true;
}

// Dispatcher — sub_40A1C6 (docs/re/ai.md §2). Draw A (leading scratch), the
// behaviour chain, Draw B (trailing scratch), and the per-frame draw order is
// exactly that: A -> the SINGLE fired behaviour's draws -> B. Draws A/B are
// heap-debug residue kept for exact RNG parity — a bare rand() advances the
// stream with the value discarded, which next_random models.
void AISystem::decide(int i, PlayerInput& out, std::int32_t delta_ms) {
    delta_ms_ = delta_ms;  // this frame's ms delta — the pursuit timers accrue it
    ensure_grids();

    // Draw A — leading scratch alloc (sub_40A1C6 line 10359). Unconditional.
    (void)next_random(s_);

    // The behaviour chain, in priority order (off_45BA78[8]). The first behaviour
    // that acts short-circuits the rest; its draws (in the fixed intra-behaviour
    // order) are the only behaviour draws taken this tick.
    bool acted = false;
    if (!acted) acted = behave_grab_drop(i, out);     // [0] sub_40BD44 grab-glove
    if (!acted) acted = behave_punch(i, out);         // [1] sub_40BE02 punch a bomb ahead
    if (!acted) acted = behave_walk_path(i, out);     // [2] sub_40B20F walk/flee
    if (!acted) acted = behave_blast_bricks(i, out);  // [3] sub_40AD8D blast bricks
    if (!acted) acted = behave_bomb_enemy(i, out);    // [4] sub_40ABED bomb near an enemy
    if (!acted) acted = behave_seek_powerup(i, out);  // [5] sub_40BAF5 seek powerup
    if (!acted) acted = behave_seek_enemy(i, out);    // [6] sub_40B8C2 seek an enemy
    if (!acted) acted = behave_wander(i, out);        // [7] sub_40A81F wander

    // If nothing acted (e.g. wander blocked and re-rolled), leave `out` with no
    // movement — the AI simply holds still this tick, as the original does when
    // the whole chain passes without a step.
    if (!acted) write_move(out, -1);

    // Draw B — trailing scratch alloc (sub_40A1C6 line 10412). Unconditional.
    (void)next_random(s_);
}

}  // namespace bomber::sim
