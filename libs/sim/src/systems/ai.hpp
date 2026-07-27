#pragma once

#include <array>
#include <cstdint>

#include "bomber/sim/state.hpp"
#include "bomber/sim/types.hpp"

namespace bomber::sim {

// The computer-player AI (ADR-0005, docs/re/ai.md). A faithful port of the
// original's per-brain dispatcher sub_40A1C6 and its behaviour chain
// off_45BA78[8]: for one AI player it fills that player's PlayerInput exactly
// as a keyboard would (direction + bomb/action edges), and the shared mover
// consumes it in player_turn. The AI is an INPUT PROVIDER, not a mover.
//
// Determinism (ADR-0003 / CLAUDE.md): integer only; all randomness through
// State::rng in the RE'd order/count (docs/re/ai.md §8). The danger grid and
// obstacle grid are per-tick scratch (rebuilt each tick from hashed State, like
// State::events) and are NEVER hashed. The only hashed AI state is Player::ai
// and State::brains.
//
// STAGE 2 SCOPE: the dispatcher (draws A/B), the danger + obstacle grids, the
// flee branch of behaviour 2 (sub_40B20F) with its flee BFS (sub_40970B) and
// flame veto (sub_40A76E), and the wander fallback (behaviour 7 sub_40A81F).
//
// STAGE 3 SCOPE (ADR-0005 §8): the directed BFS (sub_4092A1) and the DIRECTED
// branch of behaviour 2 (walk toward a held path target), the powerup scan BFS
// (sub_409C1F), and the seek-a-nearby-powerup behaviour (behaviour 5 sub_40BAF5,
// getvalue(920)=4).
//
// STAGE 4 SCOPE (ADR-0005 §8): behaviour 0 (grab-glove drop/hold, sub_40BD44)
// and behaviour 3 (blast bricks, sub_40AD8D, getvalue(915)=5). Both DROP by
// setting the bomb-key edge on the produced PlayerInput so the normal
// BombSystem path runs in player_turn (the AI is an input provider, §7); the
// blast-bricks flee is handled by behaviour 2 on the following tick(s), not
// inside behaviour 3.
//
// STAGE 5 SCOPE (ADR-0005 §8, the FINAL stage): behaviour 1 (punch a bomb ahead,
// sub_40BE02), behaviour 4 (bomb-near-enemy, sub_40ABED), behaviour 6 (seek an
// enemy, sub_40B8C2 + the enemy finder sub_422718), and the remote-detonation
// whim in behaviour 2's safe branch (trigger && !punch && rand()%10). This
// completes all 8 behaviours. TEAM: the original's team compares (+62 grab /
// +84 aggression, gated on the team-mode global dword_464964) reduce, in a
// no-team match, to "target = any live player with slot != self"; the port
// keeps that reduction (no Player::team field — see docs/re/ai.md §3.4/§5.3), so
// NO new hashed field and the golden stays FROZEN.
class AISystem {
public:
    explicit AISystem(State& s) : s_(s) {}

    // Runs once per canonical SUB-FRAME from player_turn's movement loop
    // (docs/re/facts.md "Canonical frame cadence" — the original invokes
    // sub_40A1C6 once per DISPLAYED frame, so the whims, RNG draws and
    // pursuit timers all run at frame rate; `delta_ms` is that frame's
    // integer-ms delta, constants.hpp kSubFrameMs). Fills `out` from State +
    // brains[i], mutating brains[i]. Mirrors sub_40A1C6: the leading scratch
    // draw A, the fired behaviour's draws, the trailing scratch draw B (A/B
    // are heap-debug residue kept for exact RNG parity — docs/re/ai.md
    // §2/§8). The danger and obstacle grids are (re)built lazily on the first
    // decide() of the TICK — the original rebuilds them per frame, but bombs/
    // flames are static between our sub-frames, so one build is identical.
    void decide(int i, PlayerInput& out, std::int32_t delta_ms);

private:
    // Integer scratch grids, rebuilt once per tick and shared by every AI player
    // (as in the original — a sim-global grid, not per-AI). NOT hashed.
    using Grid = std::array<std::array<std::int32_t, kGridWidth>, kGridHeight>;

    // Rebuild danger_ + obstacle_ from hashed State for the given tick. Cached
    // by tick number so N AI players in one tick pay for the rebuild once.
    void ensure_grids();

    std::int32_t danger_at(int tx, int ty) const;   // sub_424D37: 0 == safe
    bool obstacle_at(int tx, int ty) const;          // sub_409083: 1 == blocked
    bool safe_tile(int tx, int ty) const;            // sub_40A59D step-onto gate

    // sub_423188: "may a bomb be placed on THIS tile" — no bomb here (sub_422E48),
    // NO WARPHOLE here (sub_405654 -> actor type +4 == 1; corrected 2026-07-26,
    // see ai_grids.cpp and docs/re/ai.md §3.3) and the cell is blank floor
    // (sub_425FB9 == 0). NOT an escape search. Gates behaviours 3 and 4.
    bool drop_tile_clear(int tx, int ty) const;

    // The behaviours-3/4 entry gate — "sub_4245DA's own-bomb count is below the
    // +86 max-bomb byte" — inverted:
    // sub_4245DA counts the player's OWN live bomb slots (owner word at bomb
    // +62), and the comparand is the bomb capacity byte +86 — both byte-
    // confirmed from the raw disasm (docs/re/ai.md §9.3 RESOLVED). bombs_placed
    // is the sim's maintained equivalent of that owner scan.
    bool out_of_bomb_slots(const Player& p) const;

    // Behaviour 0 (sub_40BD44) — grab-glove drop/lob (Stage 4). Only when the
    // player holds Grab: if already carrying a grabbed bomb, write the bomb key up
    // and act — the mover's throw block (fires on key-up) LOBS it next tick (a
    // grab-then-throw, NOT an indefinite hold; docs/re/ai.md §3.0); else, if
    // standing on its OWN resting bomb, press the bomb key on a 1/2 whim to snatch
    // it (the mover's try_grab). Returns true if it acted. Highest priority.
    bool behave_grab_drop(int i, PlayerInput& out);

    // Behaviour 2 (sub_40B20F) — walk the path. The danger-present branch paths
    // to a held target (Stage 3, directed BFS) or, with no target, flees to the
    // safest tile (Stage 2, flee BFS); the danger-clear branch passes down unless
    // boxed in. Returns true if it acted (wrote `out` and short-circuits).
    bool behave_walk_path(int i, PlayerInput& out);

    // Behaviour 3 (sub_40AD8D) — blast bricks (Stage 4). When a spare bomb slot
    // exists (live own bombs < max_bombs, docs/re/ai.md §9.3 RESOLVED), the
    // player is not constipated, at least one orthogonally-adjacent
    // tile is a brick (cell type 2), and the standing tile is clear to drop on
    // (sub_423188), press the bomb key on a 1-in-getvalue(915)=5 whim to place a
    // bomb and set state_flag = 9 ("committed to the drop"). Does NOT flee here:
    // behaviour 2 (higher priority) paths out of the new blast next tick. Returns
    // true if it acted. Sits below behaviour 2, above behaviour 5 in the chain.
    bool behave_blast_bricks(int i, PlayerInput& out);

    // Behaviour 5 (sub_40BAF5) — seek a nearby powerup (Stage 3). On a 1/50 whim
    // it scans (sub_409C1F) for a floor powerup within getvalue(920)=4 steps,
    // latches it, then each tick paths toward it (sub_4092A1) and steps one tile.
    // Returns true if it acted. Sits BELOW behaviour 2: it walks the powerup path
    // itself (it is not a delegate to behaviour 2 — that is the RE'd structure).
    bool behave_seek_powerup(int i, PlayerInput& out);

    // Behaviour 1 (sub_40BE02) — punch a bomb ahead (Stage 5). Only when the
    // player holds the punch glove (+91) and, on a 1-in-4 whim (rand()%4==0), a
    // bomb sits on an orthogonally-adjacent tile: face that tile (write the
    // direction) and set the action2 edge so player_turn's try_punch swings.
    // Returns true if it acted. Sits below behaviour 0, above behaviour 2.
    bool behave_punch(int i, PlayerInput& out);

    // Behaviour 4 (sub_40ABED) — drop a bomb next to an enemy (Stage 5). Scans a
    // 5-tile cross (the sub_40ABED offset tables — see the OOB note in the .cpp)
    // for a live enemy player (sub_421CB5, self excluded); a same-team hit ends
    // the behaviour without dropping (same_team, docs/re/ai.md TEAM follow-up);
    // otherwise, if the capacity guard + drop-tile clearance gates pass, drop a
    // bomb on a 1-in-5 whim (rand()%5==0). Sits below behaviour 3, above
    // behaviour 5. Returns true if it acted. The bomb-key edge routes to the
    // normal BombSystem::drop.
    bool behave_bomb_enemy(int i, PlayerInput& out);

    // Behaviour 6 (sub_40B8C2) — seek an enemy (Stage 5). On a 1/50 whim it
    // acquires a random live opponent (pick_live_enemy / sub_422718), records its
    // slot + a ~10-tick timer, then each tick paths toward it (directed BFS,
    // maxdist 20) and steps one tile — giving up on the timed-out 1/50 roll, on
    // loss of the target, or (50%) if unreachable. Does NOT drop bombs (that is
    // behaviour 4, higher priority). Sits below behaviour 5, above behaviour 7.
    bool behave_seek_enemy(int i, PlayerInput& out);

    // Behaviour 7 (sub_40A81F) — wander fallback. Returns true if it acted.
    bool behave_wander(int i, PlayerInput& out);

    // Enemy finder (sub_422718): pick a live opponent (slot != self, not a
    // teammate) to pursue, starting the scan at a random slot (rand()%10) so
    // targeting is random, not nearest. Two passes exactly as the original:
    // pass 1 prefers a live HUMAN opponent (skips other AI, +16==1) and draws
    // ONE rand()%10 for its start; if it finds none, pass 2 relaxes to ANY live
    // opponent (incl. AI) and draws a SECOND rand()%10 for its own start.
    // Returns the chosen slot, or -1 if no live opponent exists. The team
    // filter (same_team) is always false on an all-zero roster, so this reduces
    // to slot != self there (docs/re/ai.md §5.3). The rand()%10 draw(s) are part
    // of the RNG contract even though §8's table lists only behaviour 6's outer
    // draws, and fire regardless of whether the team filter excludes the hit.
    int pick_live_enemy(int self);

    // A same-team player is not an enemy (docs/re/ai.md TEAM follow-up, §3.4/
    // §5.3). Our semantics: two ACTIVE players are teammates when Player::team
    // is equal AND nonzero — team 0 never matches team 0, so an all-zero roster
    // (every existing scenario) never has teammates, matching pre-team-mode
    // behaviour exactly. See Player::team's doc comment.
    bool same_team(int a, int b) const;

    // Flee BFS (sub_40970B): from (sx,sy), a 100-node wavefront scored by
    // danger_at; returns the godir (0..3) of the first step toward the lowest-
    // danger reachable tile, or -1 if boxed in, and writes that tile to
    // best_x/best_y. Draws the ±1 tie-break once at entry (docs/re/ai.md §5.2).
    int flee_bfs(int sx, int sy, int& best_x, int& best_y);

    // Directed BFS (sub_4092A1, docs/re/ai.md §5.1): same 100-node wavefront as
    // the flee BFS but with a fixed GOAL tile (tx,ty). Returns the godir (0..3)
    // of the first step of a shortest path to the goal within `max_depth` rings,
    // or -1 if unreachable; writes the ring count to `out_iters`. Draws the ±1
    // tie-break once at entry (part of the RNG contract). Used by behaviour 2's
    // directed branch and by behaviour 5's walk-to-powerup.
    int directed_bfs(int sx, int sy, int tx, int ty, int max_depth, int& out_iters);

    // Powerup scan BFS (sub_409C1F, docs/re/ai.md §5.5): same wavefront but the
    // goal test is "a floor powerup here" (sub_42542D). Returns the godir of the
    // first step toward the nearest reachable floor powerup within `max_depth`,
    // or -1 if none; writes its tile to (found_x, found_y) and the ring count to
    // `out_iters`. Draws the ±1 tie-break once at entry. Used by behaviour 5.
    int powerup_scan_bfs(int sx, int sy, int max_depth, int& out_iters, int& found_x,
                         int& found_y);

    // The flame-safety veto (sub_40A76E): if the tile one step along godir `g`
    // from (tx,ty) is on fire, cancel the step (return -1) and clear state_flag.
    int flame_veto(int i, int tx, int ty, int g);

    // Translate a chosen godir (or -1 = no move) into the PlayerInput flags,
    // exactly the bytes a keyboard would set (docs/re/ai.md §7).
    static void write_move(PlayerInput& out, int godir);

    // Set the bomb-key edge on the produced input (the original writes +56=1;
    // +54=0). Maps to action1 (docs/re/ai.md §7 / ADR-0005 §1): player_turn's
    // drop block is edge-gated on action1 && !prev_action1, so a fresh press
    // routes to grab/spooge/drop exactly as a human keypress would. The AI never
    // sets prev_action1 itself — leaving action1=false is a "release".
    static void press_bomb(PlayerInput& out);

    // Set the action-key edge (the original writes +57=1; +55=0). Maps to
    // action2 (docs/re/ai.md §7): player_turn's action block is edge-gated on
    // action2 && !prev_action2, driving punch (+91) / trigger-detonate (+95).
    // Used by behaviour 1 (punch) and behaviour 2's remote-detonation whim.
    static void press_action(PlayerInput& out);

    State& s_;
    // The current decide()'s frame delta (ms) — pursuit timers (+12/+28)
    // accrue it, mirroring the original's `+= dword_464958` per frame. Set at
    // decide() entry; NOT hashed (per-call scratch, like the grids).
    std::int32_t delta_ms_ = kMsPerTick;
    Grid danger_{};
    Grid obstacle_{};
    std::uint64_t grids_tick_ = static_cast<std::uint64_t>(-1);  // "not built yet"
    bool grids_valid_ = false;
};

}  // namespace bomber::sim
