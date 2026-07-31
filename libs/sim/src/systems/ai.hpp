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
// Determinism (ADR-0003): integer only; all randomness through State::rng in the
// RE'd order/count (docs/re/ai.md §8). The danger and obstacle grids are
// per-tick scratch, rebuilt from hashed State like State::events, and are NEVER
// hashed. The hashed state the AI reads or writes is Player::ai, Player::team
// and State::brains.
//
// TEAM (docs/re/ai.md §3.4/§5.3): the original's genuine team compares are the
// +84 byte in behaviour 4 (sub_40ABED) and in the enemy finder (sub_422718),
// both gated on the team-mode global dword_464964. The +62 compare in the grab
// behaviour is the bomb OWNER — a mislabel that ai.md §1.1/§3.0 retracts, so it
// is NOT a team compare and is not ported as one. Player::team is a HASHED
// field; an all-zero roster has no teammates by construction, so every
// pre-team scenario and its RNG stream run exactly as before.
class AISystem {
public:
    explicit AISystem(State& s) : s_(s) {}

    // Runs once per canonical SUB-FRAME from player_turn's movement loop, where
    // the original invokes sub_40A1C6 once per DISPLAYED frame — so the whims,
    // RNG draws and pursuit timers all run at frame rate (docs/re/facts.md
    // "Canonical frame cadence"). Fills `out` from State + brains[i], mutating
    // brains[i]. Mirrors sub_40A1C6: the leading scratch draw A, the fired
    // behaviour's draws, the trailing scratch draw B (A/B are heap-debug residue
    // kept for exact RNG parity — docs/re/ai.md §2/§8).
    //
    // APPROXIMATION, not an equivalence: the grids are rebuilt lazily on the
    // first decide() of the TICK where the original rebuilds them every FRAME.
    // Flames are indeed static across a tick's sub-frames, but bombs are NOT —
    // the bomb-action tail runs per sub-frame, so a bomb dropped on sub-frame k
    // is invisible to the grids for the rest of that tick, and the carried-bomb
    // stamp is taken from a carrier position that moves every sub-frame.
    void decide(int i, PlayerInput& out, std::int32_t delta_ms);

private:
    // Integer scratch grids, rebuilt once per tick and shared by every AI player
    // (as in the original — a sim-global grid, not per-AI). NOT hashed.
    using Grid = std::array<std::array<std::int32_t, kGridWidth>, kGridHeight>;

    // Rebuild danger_ + obstacle_ for the given tick, cached by tick number so N
    // AI players in one tick pay for the rebuild once. The three danger sources
    // are stamped in the order the original's own per-frame passes run them.
    void ensure_grids();
    void raise_danger(int x, int y, std::int32_t v);
    void stamp_blast(int bx, int by, int reach, std::int32_t v);
    void stamp_bomb_danger();
    void stamp_wall_danger();

    std::int32_t danger_at(int tx, int ty) const;  // sub_424D37: 0 == safe
    bool obstacle_at(int tx, int ty) const;        // sub_409083: 1 == blocked
    bool safe_tile(int tx, int ty) const;          // sub_40A59D step-onto gate

    // sub_423188: "may a bomb be placed on THIS tile" — no bomb here
    // (sub_422E48), NO WARPHOLE here (sub_405654 -> actor type +4 == 1; corrected
    // 2026-07-26, see ai_grids.cpp and docs/re/ai.md §3.3) and the cell is blank
    // floor (sub_425FB9 == 0). NOT an escape search. Gates behaviours 3 and 4.
    bool drop_tile_clear(int tx, int ty) const;

    // Behaviours 3/4's entry gate, inverted: sub_4245DA counts the player's OWN
    // live bomb slots (owner word at bomb +62) and the comparand is the capacity
    // byte +86 — both byte-confirmed from the raw disasm (docs/re/ai.md §9.3
    // RESOLVED). bombs_placed is the sim's maintained equivalent of that scan.
    bool out_of_bomb_slots(const Player& p) const;

    // The eight behaviours of off_45BA78, declared in the chain order decide()
    // runs them; each returns true if it acted and short-circuits the rest. Their
    // gates, whims and RNG draws are documented at their definitions in
    // ai_behaviours.cpp.
    bool behave_grab_drop(int i, PlayerInput& out);       // [0] sub_40BD44
    bool behave_punch(int i, PlayerInput& out);           // [1] sub_40BE02
    bool behave_walk_path(int i, PlayerInput& out);       // [2] sub_40B20F
    bool walk_path_safe_branch(int i, PlayerInput& out);  // behaviour 2's danger-clear half
    bool behave_blast_bricks(int i, PlayerInput& out);    // [3] sub_40AD8D
    bool behave_bomb_enemy(int i, PlayerInput& out);      // [4] sub_40ABED
    bool behave_seek_powerup(int i, PlayerInput& out);    // [5] sub_40BAF5
    bool behave_seek_enemy(int i, PlayerInput& out);      // [6] sub_40B8C2
    bool behave_wander(int i, PlayerInput& out);          // [7] sub_40A81F

    // Enemy finder (sub_422718): a live, non-teammate opponent to pursue, or -1.
    // Two passes, each drawing ONE rand()%10 for its start slot; both draws are
    // part of the RNG contract even though §8's table lists only behaviour 6's
    // outer draws.
    int pick_live_enemy(int self);

    // Teammates are players whose Player::team is equal AND nonzero — team 0
    // never matches team 0, so an all-zero roster never has teammates.
    bool same_team(int a, int b) const;

    // The three wavefronts, all 100-node and all drawing their ±1 tie-break once
    // at entry (part of the RNG contract). They differ in goal test and citation:
    //   flee (sub_40970B, §5.2)        — lowest danger_at reachable, into best_x/y;
    //   directed (sub_4092A1, §5.1)    — a fixed goal tile within max_depth;
    //   powerup scan (sub_409C1F, §5.5) — nearest floor powerup (sub_42542D).
    // Each returns the godir (0..3) of the first step, or -1.
    int flee_bfs(int sx, int sy, int& best_x, int& best_y);
    int directed_bfs(int sx, int sy, int tx, int ty, int max_depth, int& out_iters);
    int powerup_scan_bfs(int sx, int sy, int max_depth, int& out_iters, int& found_x,
                         int& found_y);

    // The flame-safety veto (sub_40A76E): if the tile one step along godir `g`
    // from (tx,ty) is on fire, cancel the step (return -1) and clear state_flag.
    int flame_veto(int i, int tx, int ty, int g);

    // Translate a chosen godir (or -1 = no move) into the PlayerInput flags,
    // exactly the bytes a keyboard would set (docs/re/ai.md §7).
    static void write_move(PlayerInput& out, int godir);

    // A key DOWN written by behaviours 0/3/4 (bomb) or 1 (action) is a
    // MANUFACTURED edge: the original writes the PAIR `+56 = 1; +54 = 0`
    // (sub_40BD44 806-807, sub_40AD8D 399-400, sub_40ABED 347-348, sub_40BE02
    // 838-839) — it sets the key AND clears the previous-frame copy the drop
    // block edge-tests against, so an AI press is always a fresh edge no matter
    // what the key did last frame (facts.md "AI key presses manufacture their own
    // edge"). Without the second write, a behaviour firing on consecutive frames
    // — which behaviour 0 does almost every frame, its gate being a bare 1-in-2
    // whim — sees its own still-latched key and never edges again.
    void press_bomb(int i, PlayerInput& out);
    void press_action(int i, PlayerInput& out);

    // Behaviour 0's carrying branch writes `+56 = 0; +54 = 0` (sub_40BD44
    // 797-798). The throw block is level-gated (!+56), not edge-gated, so the +54
    // clear changes nothing here; it is mirrored to keep the writes byte-for-byte.
    void release_bomb(int i, PlayerInput& out);

    // Behaviour 2's remote-detonation whim writes `+57 = 1` ALONE (sub_40B20F
    // 631-632) and leaves +55 as the mover set it. That asymmetry is the
    // original's, not an oversight: a trigger AI that wins the 1-in-10 roll on
    // consecutive frames really does detonate only on the first.
    static void press_action_sustained(PlayerInput& out);

    State& s_;
    // The current decide()'s frame delta (ms) — pursuit timers (+12/+28) accrue
    // it, mirroring the original's `+= dword_464958` per frame. Per-call scratch,
    // NOT hashed.
    std::int32_t delta_ms_ = kMsPerTick;
    Grid danger_{};
    Grid obstacle_{};
    std::uint64_t grids_tick_ = static_cast<std::uint64_t>(-1);  // "not built yet"
    bool grids_valid_ = false;
};

}  // namespace bomber::sim
