// The computer-player AI (ADR-0005, docs/re/ai.md). Stage 2: the dispatcher
// skeleton, the danger + obstacle grids, the flee branch + the wander fallback.
// Stage 3: the directed BFS (sub_4092A1), behaviour 2's directed branch, the
// powerup scan (sub_409C1F), and behaviour 5 (seek a nearby powerup, sub_40BAF5).
// Stage 4: behaviour 0 (grab-glove drop/hold, sub_40BD44) and behaviour 3
// (blast bricks, sub_40AD8D) — both DROP by setting the bomb-key edge on the
// produced input so the normal BombSystem path runs in player_turn.
// Every mechanic mirrors a named original function; comments cite the sub_XXXX.
// Integer only; RNG only via State::rng in the RE'd order/count (docs/re/ai.md
// §8). See ai.hpp for the staged scope.
//
// This subsystem is split across translation units for readability (a pure
// file-split, no behaviour change): ai_grids.cpp (danger/obstacle grids + the
// tile predicates), ai_pathfind.cpp (the 3 BFS variants + the flame veto),
// ai_behaviours.cpp (the 8 behave_* + the enemy finder), and this file (the
// dispatcher entry + the PlayerInput adapters). Shared godir tables live in
// ai_internal.hpp. See ai.hpp for the API and staged scope.

#include "systems/ai.hpp"

#include "bomber/sim/rng.hpp"

namespace bomber::sim {

namespace {

// Godir -> our Direction enum for writing the input direction flag.
constexpr Direction kGodirDir[4] = {Direction::Up, Direction::Right, Direction::Down,
                                    Direction::Left};

}  // namespace

void AISystem::write_move(PlayerInput& out, int godir) {
    out.up = out.down = out.left = out.right = false;
    if (godir < 0) return;
    switch (kGodirDir[godir & 3]) {
        case Direction::Up: out.up = true; break;
        case Direction::Right: out.right = true; break;
        case Direction::Down: out.down = true; break;
        case Direction::Left: out.left = true; break;
    }
}

// The bomb-key edge (+56=1; +54=0 in the original) -> action1. player_turn's drop
// block is edge-gated on action1 && !prev_action1; the AI never sets prev_action1
// itself, so a single-tick action1=true is a fresh press (docs/re/ai.md §7).
void AISystem::press_bomb(PlayerInput& out) {
    out.action1 = true;
}

// The action-key edge (+57=1; +55=0 in the original) -> action2. player_turn's
// action block is edge-gated on action2 && !prev_action2, so a single-tick
// action2=true is a fresh press routed to punch (+91) / trigger (+95). The AI
// never sets prev_action2 itself (docs/re/ai.md §7).
void AISystem::press_action(PlayerInput& out) {
    out.action2 = true;
}

// ---------------------------------------------------------------------------
// Dispatcher — sub_40A1C6 (docs/re/ai.md §2). Draw A (leading scratch), the
// behaviour chain (first behaviour that acts short-circuits), Draw B (trailing
// scratch). Draws A/B are heap-debug residue kept for exact RNG parity: a bare
// rand() advances the stream with the value discarded, which next_random models.
//
// DISPATCHER ORDER (ADR-0005 §8): ALL 8 behaviours are now live (Stage 5 landed
// behaviours 1/4/6 + the safe-branch trigger whim). Draws land in the §8 slots:
// A (scratch) -> the single fired behaviour's draws -> B (scratch). Because
// golden has no AI players, no draw stream regresses (ADR-0005 §7). Behaviours:
// [0] grab-glove (%2 when eligible), [1] punch (%4 when holding punch), [2]
// walk/flee (BFS tie-break; safe-branch %10 trigger whim), [3] blast bricks
// (%915 when the gates pass), [4] bomb-near-enemy (%5 when a foe is on the cross
// and the tile is clear), [5] seek powerup (%50-acquire / scan / path / %2), [6]
// seek enemy (%50-acquire + the finder's %10 draws / %50-timeout / path / %2),
// [7] wander. Only ONE behaviour body runs per tick (short-circuit).
// ---------------------------------------------------------------------------
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
