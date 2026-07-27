// AI behaviour chain: the eight behave_* behaviours (off_45BA78[8]) and the
// enemy finder (pick_live_enemy / sub_422718, same_team). Split out of ai.cpp
// as a pure file-split (no behaviour change); see ai.hpp for the subsystem API
// and docs/re/ai.md for the RE facts. The dispatcher that runs the chain stays
// in ai.cpp.

#include "systems/ai.hpp"

#include <algorithm>  // std::max for the blast-bricks getvalue(915) clamp

#include "bomber/sim/rng.hpp"
#include "grid.hpp"
#include "systems/ai_internal.hpp"  // kDX / kDY godir vectors

namespace bomber::sim {

namespace {

// Behaviour 4's enemy-scan cross (sub_40ABED, docs/re/ai.md §3.4 / §9.4). The
// original walks i in 0..4 reading dword_45BAB0[i] for the X offset and
// dword_45BA9C[i] for the Y offset. dword_45BA9C[5] = {0,-1,0,1,0} is a clean
// vertical cross; dword_45BAB0[] is a ONE-element array {-1}, so reading [1..4]
// runs PAST it into the adjacent .data — an original out-of-bounds read. Reading
// the shipped BM95.EXE bytes at 0x45BAB0..0x45BAC0 gives {-1,0,0,0,1} for those
// five dwords (the "garbage" after -1 happens to be 0,0,0,1). Combined with the
// Y table that yields a PERFECT 5-tile plus/cross centred on the AI: LEFT, UP,
// SELF(0,0), DOWN, RIGHT. We reproduce those exact constants (a faithful image
// of the OOB read, not a crash — it never faults in practice). Index 2 is the
// AI's own tile; the original excludes self by zeroing its actor +0 across the
// sub_421CB5 probe, which our scan mirrors by skipping the self slot.
constexpr int kEnemyScanX[5] = {-1, 0, 0, 0, 1};  // dword_45BAB0[0..4] (OOB tail)
constexpr int kEnemyScanY[5] = {0, -1, 0, 1, 0};  // dword_45BA9C[5]

}  // namespace

// ---------------------------------------------------------------------------
// Behaviour 0 — sub_40BD44, grab-glove drop/hold (docs/re/ai.md §3.0), the
// highest-priority behaviour. Only for a player holding Grab (+92):
//   - if already carrying a grabbed bomb (+148): write the bomb key UP (+56=0,
//     +54=0) and act (return 1), suppressing every lower behaviour. This is NOT
//     an indefinite hold: the mover's carried-bomb throw block fires on !+56, so
//     forcing the key up LOBS the bomb the very next tick — the grab-AI grabs
//     then throws forward (docs/re/ai.md §3.0 CORRECTION);
//   - else, if standing on its OWN resting bomb, press the bomb key on a 1/2
//     whim (rand()%2) to snatch it: player_turn's drop block sees a fresh
//     action1 edge over the own bomb underfoot and routes to try_grab.
// RNG (§8 row b0): the rand()%2 fires ONLY when grab && !carrying && own bomb
// underfoot; otherwise this behaviour draws nothing. Team note: the original's
// bomb.team==player.team reduces, in a no-team match, to "the bomb is mine"
// (bomb.owner==self) — the same reduction player_turn's own grab block uses.
// ---------------------------------------------------------------------------
bool AISystem::behave_grab_drop(int i, PlayerInput& out) {
    Player& p = s_.players[i];
    if (!p.grab) return false;  // sub_40BD44: !+92 -> not our behaviour

    if (p.carrying) {
        // Carrying a grabbed bomb (+148): write the bomb key up (the original
        // sets +56=0; +54=0). We leave action1=false — a release — so
        // player_turn's throw block (fires on !action1, the same !+56 gate)
        // LOBS the carried bomb forward next tick, exactly as the original does.
        out.action1 = false;
        return true;  // act, short-circuit the chain
    }

    // Own bomb underfoot? bomb_at at the AI's tile, owner == self. sub_422E48
    // (pseudo.c 25031-25051) matches a RESTING **or SLIDING** bomb (motion !=
    // flying(2) && != carried(3)) — grid::bomb_at already excludes flying/
    // carried, and the original does NOT additionally require the bomb to be
    // at rest (RESOLVED: an earlier `!under->moving` guard here was a real
    // deviation — it silently dropped the whole draw for a sliding own bomb,
    // where the original still rolls; try_grab already supports mid-slide
    // pickup, bombs.cpp "motion states 0 AND 1 both qualify").
    const Bomb* under = grid::bomb_at(s_, p.tile_x(), p.tile_y());
    const bool own = under != nullptr && under->owner == static_cast<std::uint8_t>(i);
    // rand()%2 TRUTHY (== 1) -> grab it: the branch condition at pseudo.c 11025
    // requires a bomb underfoot, its +62 owner word to equal the AI's own +62,
    // and rand()%2 to be NONZERO — the draw is used as-is, not negated
    // (RESOLVED: an earlier `== 0` here was an
    // inverted-polarity deviation from both the binary and ai.md §3.0's own
    // transcription, which already read it correctly as "and rand()%2").
    if (own && random_below(s_, 2) != 0) {
        press_bomb(out);  // fresh bomb-key edge -> try_grab in player_turn
        return true;
    }
    return false;  // pass down to behaviour 1/2/...
}

// ---------------------------------------------------------------------------
// Behaviour 2 — sub_40B20F, "walk the path" (docs/re/ai.md §3.2). The danger-
// present branch either paths to a held directed target (Stage 3, sub_4092A1) or
// flees to the safest reachable tile (Stage 2, sub_40970B); the danger-clear
// branch passes down unless boxed in. Returns true if it acted.
// ---------------------------------------------------------------------------
bool AISystem::behave_walk_path(int i, PlayerInput& out) {
    Player& p = s_.players[i];
    Brain& br = s_.brains[i];
    const int px = p.tile_x(), py = p.tile_y();

    if (danger_at(px, py) != 0) {
        // Standing in / near a threat. Two sub-modes (docs/re/ai.md §3.2):
        //   (a) a DIRECTED path target is held -> path to it (sub_4092A1), Stage 3;
        //   (b) no target -> FLEE to the safest reachable tile (sub_40970B), Stage 2.
        // Exactly ONE BFS runs, so exactly one BFS tie-break draw is taken this
        // tick (RNG contract §8 row b2) whichever sub-mode fires.

        // Stale-target invalidation: if a target is held whose captured cost was 0
        // (!cost, i.e. it was safe when chosen) but the tile is now dangerous, drop
        // it. Mirrors the original at line 10784: with the +2 target-held flag
        // set and the +8 captured cost zero, a dangerous target tile clears +2.
        if (br.has_path_target && br.path_target_cost == 0 &&
            danger_at(br.path_target_x, br.path_target_y) != 0) {
            br.has_path_target = false;
        }

        if (br.has_path_target) {
            // (a) Directed: walk one step toward the held goal (docs/re/ai.md
            // §3.2 directed branch). maxdist = 20 (the original's literal).
            int iters = 0;
            const int first = directed_bfs(px, py, br.path_target_x, br.path_target_y, 20, iters);
            if (first < 0) {
                // No path within depth -> drop the target and pass down (the
                // original clears +2 and returns 0).
                br.has_path_target = false;
                return false;
            }
            const int g = flame_veto(i, px, py, first);  // sub_40A76E veto
            write_move(out, g);
            // The original's return value here is the high half of the +44 field
            // compared against -1, i.e. the
            // godir word the veto may have just set to -1 (pseudo.c 10841-10842
            // calls the veto sub_40A76E and then returns that comparison) -- NOT an
            // unconditional 1. A flame-vetoed step makes behaviour 2 PASS DOWN
            // (return 0), giving 3-7 a turn (and their draws) this tick, rather
            // than stalling. RNG-order-critical: fixing a fall-through this
            // hard-coded `true` used to swallow (docs/re/ai.md §3.2 RESOLVED).
            return g != -1;
        }

        // (b) Flee: no directed goal, run to the lowest-danger reachable tile.
        int bx = 0, by = 0;
        const int first = flee_bfs(px, py, bx, by);  // the flee behaviour's RNG draw

        // Two distinct outcomes here — the 2026-07-12 "danger branch never
        // passes down" correction over-reached by folding them together
        // (re-pinned 2026-07-16 movement audit, pseudo.c 10816-10829):
        //
        //  1. FULLY BOXED IN (the flee search came back with no first direction
        //     at all): the
        //     original CLEARS the target flag and returns 0 — behaviours 3-7
        //     DO get this frame's turn (whims, drop gates, wander re-rolls:
        //     the trapped-in-danger fidget, with all its RNG draws).
        if (first < 0) {
            br.has_path_target = false;
            return false;
        }
        //  2. NO STRICTLY-SAFER TILE (own danger <= the best found, 10824-10829): latch the
        //     OWN tile as target, write godir -1 and return 1 — behaviours
        //     3-7 never run and draw NOTHING that frame.
        const std::int32_t here = danger_at(px, py);
        if (here <= danger_at(bx, by)) {
            br.has_path_target = true;
            br.path_target_x = static_cast<std::int16_t>(px);
            br.path_target_y = static_cast<std::int16_t>(py);
            // ai.md finding 2 (sub_40B20F 607-615, disasm 0x40B3EC-0x40B422):
            // this "can't improve, stand" branch writes ONLY the target tile
            // (+4/+6) and godir -1 — it does NOT touch path_target_cost (+8).
            // Leaving +8 stale is load-bearing: the top-of-branch stale-target
            // invalidation fires on `has_path_target && cost==0 && danger!=0`,
            // so a brain whose +8 is still 0 (never improved, or last improved
            // to full safety) re-enters the FLEE sub-branch next tick (a fresh
            // flee_bfs + its RNG draw) rather than the DIRECTED one. Writing
            // `cost = here` (always nonzero here) suppressed that, diverging the
            // BFS-call/RNG sequence in the trapped-in-persistent-danger case.
            write_move(out, -1);
            return true;
        }

        // Step toward the safer tile, but veto a step that lands on flame.
        br.has_path_target = true;
        br.path_target_x = static_cast<std::int16_t>(bx);
        br.path_target_y = static_cast<std::int16_t>(by);
        br.path_target_cost = danger_at(bx, by);
        const int g = flame_veto(i, px, py, first);
        write_move(out, g);
        // Same "+44's high half != -1" return as the directed branch above
        // (pseudo.c 10841-10842): a flame-vetoed flee step passes down instead
        // of stalling, letting 3-7 take this tick's turn (docs/re/ai.md §3.2
        // RESOLVED). The "can't improve" branch above (line ~569) is unaffected
        // -- the original returns 1 unconditionally there and never calls the
        // veto (it has no godir to veto: the step is already -1).
        return g != -1;
    }

    // Danger-clear (safe). The original: if trigger held (and no punch) roll
    // rand()%10 to detonate remote bombs (Stage 5 — the whim's draw lands here in
    // the §8 order, before the neighbour scan). The && short-circuits so the %10
    // draw is taken ONLY when trigger && !punch, matching the original: the
    // trigger flag +95 set, the punch flag +91 clear and rand()%10 coming up
    // zero together set the action2 byte +57.
    // Setting action2 routes to detonate_triggered in
    // player_turn (edge-gated on action2 && !prev_action2). Then check whether ANY
    // neighbour is walkable+safe; if boxed in with nowhere safe, "act" (stall) so
    // the chain stops, else pass down to let a lower behaviour (wander) drift.
    if (p.trigger && !p.punch && random_below(s_, 10) == 0) press_action(out);
    br.has_path_target = false;
    for (int g = 0; g < 4; ++g)
        if (safe_tile(px + kDX[g], py + kDY[g])) return false;  // a way out exists: pass down
    // Boxed in and nowhere safe to step: stall in place and stop the chain.
    write_move(out, -1);
    return true;
}

// ---------------------------------------------------------------------------
// Behaviour 3 — sub_40AD8D, blast bricks (docs/re/ai.md §3.3). Drops a bomb to
// open the board when standing next to a destroyable brick, gated on a 1-in-
// getvalue(915)=5 whim. Structure (byte-exact):
//   1. capacity guard: sub_4245DA(me) — my live-bomb count — must be < my
//      maxBombs(+86) (the disasm-confirmed comparand, §9.3 RESOLVED);
//   2. constipation (+134) blocks the drop;
//   3. count the orthogonally-adjacent BRICK tiles (cell type 2);
//   4. if none: clear state_flag 9->0 (situation resolved) and pass down;
//   5. if the standing tile is clear to drop on (sub_423188): roll rand()%5 —
//      on 0, press the bomb key (drop) and set state_flag = 9; else pass down.
// It does NOT flee here: state_flag=9 is a "committed to the drop" marker; the
// danger grid lights up under the new bomb next tick and behaviour 2 (higher
// priority) paths the AI out (docs/re/ai.md §3.3). state_flag=9 self-clears when
// there are no adjacent bricks / capacity fills — i.e. once the AI has moved.
// RNG (§8 row b3): the rand()%5 fires ONLY when the capacity/constipation/brick/
// clearance gates all pass; otherwise this behaviour draws nothing.
// ---------------------------------------------------------------------------
bool AISystem::behave_blast_bricks(int i, PlayerInput& out) {
    Player& p = s_.players[i];
    Brain& br = s_.brains[i];
    const int px = p.tile_x(), py = p.tile_y();

    // (1) Capacity guard (sub_4245DA(me) >= maxBombs(+86) -> pass down; §9.3
    // RESOLVED from the raw disasm — NOT a bombs-in-my-column rule). On the
    // "no spare slot" branch the original also clears a stale state_flag 9
    // before returning 0.
    if (out_of_bomb_slots(p)) {
        if (br.state_flag == 9) br.state_flag = 0;
        return false;
    }

    // (2) Constipation (+134): can't drop at all. (No state clear on this branch,
    // matching the original — its "+134 set -> return 0" test is inside the
    // capacity-ok block, before the brick count / state handling.)
    if (p.sick(Disease::Constipation)) return false;

    // (3) Count orthogonally-adjacent BRICK tiles (sub_425FB9 == 2 -> Cell::Brick).
    int bricks = 0;
    for (int d = 0; d < 4; ++d) {
        const int nx = px + kDX[d], ny = py + kDY[d];
        if (grid::in_grid(nx, ny) && s_.cells[ny][nx] == Cell::Brick) ++bricks;
    }

    // (4) No adjacent bricks: clear the commit flag and pass down.
    if (bricks == 0) {
        if (br.state_flag == 9) br.state_flag = 0;
        return false;
    }

    // (5) The standing tile must be clear to drop a bomb on (sub_423188). Not a
    // look-ahead escape check — behaviour 2 handles the flee next tick.
    if (!drop_tile_clear(px, py)) return false;

    // The 1-in-getvalue(915)=5 whim. r = max(1, getvalue(915)); rand()%r != 0 ->
    // pass down; == 0 -> DROP. ai_blast_chance is our getvalue(915) tunable.
    const auto r = static_cast<std::uint32_t>(std::max(1, s_.tuning.ai_blast_chance));
    if (random_below(s_, r) != 0) return false;

    // Drop: a fresh bomb-key edge routes to BombSystem::drop in player_turn (the
    // normal placement, with the same dud-gate RNG a human drop takes). Mark the
    // commit; behaviour 2 flees the resulting blast on the following tick(s).
    press_bomb(out);
    br.state_flag = 9;
    return true;
}

// ---------------------------------------------------------------------------
// Behaviour 5 — sub_40BAF5, seek a nearby powerup (docs/re/ai.md §3.5). On a
// 1/50 whim (only while NOT already latched) it BFS-scans (sub_409C1F) for a
// floor powerup within getvalue(920)=4 steps; if one is within range+1 steps it
// latches the target, then each tick paths toward it (sub_4092A1, maxdist =
// range+1) and steps one tile, giving up on a ~10-tick timeout, on loss of the
// powerup, or (50%) if it becomes unreachable. RNG order within this behaviour
// (§8 rows b5a..b5d): %50 acquire -> [scan tie-break if acquiring] -> path
// tie-break -> %2 give-up. Returns true if it acted (short-circuits the chain).
//
// Faithful quirk (docs/re/ai.md §3.5, verified in the decompile): the acquire
// resets the ENEMY-seek timer (+12) rather than the powerup-seek timer (+28) —
// an original field mix-up. We reproduce it (enemy_seek.timer = 0) for exact
// field parity; it is a no-op here since behaviour 6 (enemy-seek) is a Stage-5
// stub that never advances +12. The powerup timer (+28) is NOT reset on acquire.
// ---------------------------------------------------------------------------
bool AISystem::behave_seek_powerup(int i, PlayerInput& out) {
    Player& p = s_.players[i];
    Brain& br = s_.brains[i];
    const int px = p.tile_x(), py = p.tile_y();
    const int range = s_.tuning.ai_powerup_range;  // getvalue(920) = 4

    // Acquire: only when not already pursuing, on a 1/50 whim. The `&&` short-
    // circuits so the %50 draw is taken ONLY when !active (matches the original).
    if (!br.pow_seek.active && random_below(s_, 50) == 0) {
        int iters = 0, fx = 0, fy = 0;
        const int first = powerup_scan_bfs(px, py, range, iters, fx, fy);  // scan tie-break draw
        if (first >= 0) {
            // Found a reachable floor powerup; accept it only if within range+1
            // steps (the original: `getvalue(920)+1 >= nsteps`).
            if (range + 1 >= iters) {
                br.pow_seek.active = true;
                br.enemy_seek.timer = 0;  // the +12 write quirk (see header note)
                br.pow_seek.tile_x = static_cast<std::int16_t>(fx);
                br.pow_seek.tile_y = static_cast<std::int16_t>(fy);
            }
        }
    }

    if (!br.pow_seek.active) return false;  // no target: pass down to behaviour 6/7

    // Timeout after 500 ms of pursuit (original: `timer(+28) += frameDelta`
    // per frame; give up at `10 * [0x46494C]` = 10 × 50 ms. NO rand draw on
    // this timeout, unlike enemy-seek). The timer is wall-clock ms, accrued
    // per sub-frame — at the canonical 60 fps that is ~30 decides, exactly
    // the original's own count.
    br.pow_seek.timer += delta_ms_;
    if (br.pow_seek.timer >= 10 * kMsPerTick) {
        br.pow_seek.active = false;
        return false;
    }

    // Liveness: the original reloads the cell pointer and drops the target if it
    // is null or no longer a live powerup (*cell != 2). Our tile image: if the
    // floor powerup at the stored tile is gone (collected/blasted), give up.
    const int tx = br.pow_seek.tile_x, ty = br.pow_seek.tile_y;
    if (!grid::in_grid(tx, ty) || s_.floor[ty][tx] == PowerupType::None) {
        br.pow_seek.active = false;
        return false;
    }

    // Path toward the powerup (maxdist = range+1, i.e. the original's own
    // getvalue(920) range plus one). One BFS
    // tie-break draw. If unreachable (0 iters) give up 50% of the time BEFORE the
    // firstdir check, exactly as the original orders it (line 10989).
    int iters = 0;
    const int first = directed_bfs(px, py, tx, ty, range + 1, iters);
    if (iters == 0 && random_below(s_, 2) != 0) {
        br.pow_seek.active = false;
    }
    if (first < 0) {
        br.pow_seek.active = false;
        return false;
    }

    // Take the step, but only if the next tile is safe to stand on (sub_40A59D);
    // otherwise hold (godir -1). The original records the step dir at +36 first.
    br.pow_seek.step_dir = static_cast<std::int8_t>(first);
    const int nx = px + kDX[first & 3], ny = py + kDY[first & 3];
    write_move(out, safe_tile(nx, ny) ? first : -1);
    return true;  // acted
}

// ---------------------------------------------------------------------------
// Behaviour 1 — sub_40BE02, punch a bomb ahead (docs/re/ai.md §3.1). Byte-exact
// (0x40BE02): only for a player holding the punch glove (+91). On a 1-in-4 whim
// (rand()%4 != 0 -> pass) it scans the four orthogonally-adjacent tiles (the
// 4-element godir tables dword_45BECC/45BEDC) for a bomb; if one is found it
// faces that tile (+46 = i) and sets the action key edge (+57=1; +55=0) so
// player_turn's try_punch swings. Returns true if it acted.
//
// Port note: try_punch uses p.facing, and MovementSystem::move sets p.facing = d
// unconditionally (even when the step is blocked by the bomb), so writing the
// direction toward the bomb faces the AI at it before the action2 block runs.
// RNG (§8 row b1): the rand()%4 fires ONLY when the player holds punch; a punch
// with no bomb ahead still draws the %4 (it is above the bomb scan) but does not
// act. player_turn also gates try_punch on !action1 — the AI's action1 is false
// here (we only set action2), so the swing is not suppressed.
// ---------------------------------------------------------------------------
bool AISystem::behave_punch(int i, PlayerInput& out) {
    Player& p = s_.players[i];
    if (!p.punch) return false;                  // sub_40BE02: !+91 -> not our behaviour
    if (random_below(s_, 4) != 0) return false;  // rand()%4 != 0 -> consider it only 1-in-4

    const int px = p.tile_x(), py = p.tile_y();
    int face = -1;
    for (int g = 0; g < 4; ++g) {  // the 4-element godir cross (dword_45BECC/45BEDC)
        if (grid::bomb_at(s_, px + kDX[g], py + kDY[g]) != nullptr) {
            face = g;
            break;  // first bomb found, in godir order (Up,Right,Down,Left)
        }
    }
    if (face < 0) return false;  // no bomb orthogonally adjacent: pass down

    // Face the bomb (the direction flag sets p.facing in the mover) and set the
    // action2 edge so try_punch launches the bomb 3 tiles in that direction.
    write_move(out, face);
    press_action(out);
    return true;
}

// ---------------------------------------------------------------------------
// Behaviour 4 — sub_40ABED, drop a bomb next to an enemy (docs/re/ai.md §3.4).
// Byte-exact (0x40ABED). Structure:
//   1. capacity guard: sub_4245DA(me) — my live-bomb count — must be < my
//      maxBombs(+86), the same disasm-confirmed spare-slot gate as behaviour 3
//      (§9.3 RESOLVED);
//   2. a Manhattan gate = distance TRAVELLED since the actor's +20/+24 snapshot
//      (set at spawn and at warp step-on): abs(curTileX - snapX) + abs(curTileY
//      - snapY) >= 3. Near-constant FALSE right after a spawn/warp (distance 0)
//      and only true once the AI has net-displaced >= 3 tiles. We reuse
//      warp_to_x/y (the port's +20/+24 — seeded to the spawn tile at setup)
//      (ai.md finding 1, disasm-confirmed 0x40AC24-0x40AC6D — the summary in
//      ai.md §3.4/§9.4 that called this "near-always TRUE" is corrected);
//   3. scan the 5-tile plus/cross (kEnemyScanX/Y, the OOB tables) for a live
//      (active +0, not-dead +8) ENEMY player — sub_421CB5 at pseudo.c 24207
//      requires the candidate's leading +0 dword to be nonzero and its +8 dword
//      to be zero, where +8 is died-this-round, NOT the +58 stun — self excluded
//      (the original zeroes its own actor +0 across the probe; we skip the self);
//   4. per hit: in team mode skip a teammate (dword_464964 gate; same_team()
//      below — see docs/re/ai.md §3.4/§5.3, "our semantics" note at same_team's
//      definition for what counts as a team on an all-zero roster); the
//      standing tile must be clear to drop on (sub_423188); then drop on a 1-in-5
//      whim (rand()%5==0).
// It does NOT flee here — behaviour 2 (higher priority) paths the AI out of the
// new blast next tick. RNG (§8 row b4): the rand()%5 fires ONLY once a live enemy
// is found on the cross AND the clearance gate passes; otherwise no draw.
// ---------------------------------------------------------------------------
bool AISystem::behave_bomb_enemy(int i, PlayerInput& out) {
    Player& p = s_.players[i];
    const int px = p.tile_x(), py = p.tile_y();

    // (1) Capacity guard (sub_4245DA(me) >= maxBombs(+86) -> return 0; §9.3
    // RESOLVED), matching behaviour 3.
    if (out_of_bomb_slots(p)) return false;

    // (2) Manhattan gate (ai.md finding 1; sub_40ABED, disasm-confirmed
    // 0x40AC24-0x40AC6D): the original gate is the Manhattan DISTANCE the AI
    // has travelled since its last spawn/warp snapshot (+20/+24) >= 3 tiles —
    // abs(currentTileX - snapX) + abs(currentTileY - snapY) >= 3, with the
    // subtraction done before each absolute-value call in the raw code, NOT the
    // absolute-coordinate magnitude the old
    // port (and ai.md §3.4's summary) used. The snapshot lives in warp_to_x/y
    // (the port's +20/+24: set to the spawn tile at setup.cpp, to the warp exit
    // at start_warp). So the gate is near-constant FALSE right after a spawn or
    // a warp (distance 0) and only opens once the AI has net-displaced >= 3
    // tiles — a freshly-spawned/just-warped AI will NOT bomb a cornered enemy
    // until it has moved away. Changes WHEN behaviour 4 (and its rand()%5) is
    // considered -> RNG-order-relevant.
    const int dxs = px - p.warp_to_x, dys = py - p.warp_to_y;
    const int mx = dxs < 0 ? -dxs : dxs;
    const int my = dys < 0 ? -dys : dys;
    if (mx + my < 3) return false;

    // (3) Scan the 5-tile cross for a live enemy (sub_421CB5), self excluded.
    for (int k = 0; k < 5; ++k) {
        const int tx = px + kEnemyScanX[k], ty = py + kEnemyScanY[k];
        if (!grid::in_grid(tx, ty)) continue;
        int who = -1;
        for (int j = 0; j < kMaxPlayers; ++j) {
            const Player& q = s_.players[j];
            if (j == i) continue;  // self excluded (the original's +0-zeroing trick)
            // sub_421CB5 checks +0 (active) && !+8 (not dead); our present && alive.
            // NO +58/stun check — a stunned-but-alive enemy is still bombable.
            if (q.present && q.alive && q.tile_x() == tx && q.tile_y() == ty) {
                who = j;
                break;  // sub_421CB5 returns the first live (active, not-dead) player here
            }
        }
        if (who < 0) continue;

        // (4) Team gate (dword_464964 && me.team == cell.team -> skip; §3.4). A
        // same-team hit is not an enemy: the ORIGINAL returns 0 here, ending the
        // whole behaviour (it does NOT continue scanning the rest of the cross),
        // so we mirror that exactly. On an all-zero roster same_team() is always
        // false, so this never fires there (byte-identical to before). Then the
        // standing tile must be clear to drop on (sub_423188), and the 1-in-5 whim.
        if (same_team(i, who)) return false;
        if (!drop_tile_clear(px, py)) return false;  // matches the original's return 0
        if (random_below(s_, 5) != 0) return false;  // rand()%5 != 0 -> pass
        press_bomb(out);  // bomb-key edge -> BombSystem::drop in player_turn
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// A same-team player is not an enemy (docs/re/ai.md §5.3: "in team mode only
// if its team +84 differs"; the +84 wiring itself is the documented follow-up
// this change lands). Our semantics (not RE'd beyond the byte's existence,
// see Player::team): two ACTIVE players are teammates when their team values
// are equal, INCLUDING both-zero — so a fully-zeroed roster (every existing
// scenario) has i.team==j.team for every pair and would wrongly call every
// player a teammate of every other. To keep the untamed (all-zero) path
// byte-identical to "team mode off", we special-case team==0 as "no team" on
// both sides: zero never matches zero. Only a nonzero, shared value is a team.
bool AISystem::same_team(int a, int b) const {
    const std::uint8_t ta = s_.players[a].team, tb = s_.players[b].team;
    return ta != 0 && ta == tb;
}

// Enemy finder — sub_422718 (docs/re/ai.md §5.3). Picks a live opponent to
// pursue, starting the scan at a random slot so targeting is random (not
// nearest). Two passes, byte-exact:
//   pass 1: start = rand()%10, scan 10 slots forward (wrapping); accept the
//           first that is NOT self, present (+16 != 0), NOT another AI/computer
//           (+16 != 1), active (+0), NOT DEAD (the zero-test at pseudo.c 24741
//           is on the candidate's +8 dword, NOT the +58 stun), and (team mode)
//           not a teammate. This pass draws
//           ONE rand()%10 for its start index.
//   pass 2 (only if pass 1 finds nothing): start = a SECOND rand()%10; the same
//           scan but RELAXED to include other AI players (drops the +16 != 1
//           test). Returns the first live, non-teammate opponent, else -1.
// Returns the chosen slot index, or -1 if no live opponent exists. The team
// filter (same_team above) reduces to "never true" on an all-zero roster, so
// this is exactly "slot != self" there, byte-identical to before Player::team
// existed. Both rand()%10 draws are part of the RNG contract regardless of
// whether a hit is found (they sit inside behaviour 6's acquire, between its
// rand()%50 and the timer/BFS steps — §8 lists only the outer draws), so the
// team filter changes WHO is picked, never HOW MANY draws happen.
// ---------------------------------------------------------------------------
int AISystem::pick_live_enemy(int self) {
    // Pass 1: prefer a live human opponent (skip other AI, +16==1 -> ai==true).
    const int start1 = static_cast<int>(random_below(s_, 10));
    for (int n = 0; n < kMaxPlayers; ++n) {
        const int j = (start1 + n) % kMaxPlayers;
        if (j == self) continue;  // the original's self-vs-candidate identity test
        const Player& q = s_.players[j];
        if (!q.present) continue;          // !+16 (absent)
        if (q.ai) continue;                // +16 == 1 (another computer player)
        if (!q.alive) continue;            // !+0 (inactive) / +8 set (dead) — NOT +58 stun
        if (same_team(self, j)) continue;  // team mode: skip a teammate
        return j;  // the first live, non-teammate human opponent (slot != self)
    }
    // Pass 2: fall back to ANY live opponent (incl. other AI) — the relaxed scan.
    const int start2 = static_cast<int>(random_below(s_, 10));
    for (int n = 0; n < kMaxPlayers; ++n) {
        const int j = (start2 + n) % kMaxPlayers;
        if (j == self) continue;  // the original's self-vs-candidate identity test
        const Player& q = s_.players[j];
        if (!q.present) continue;          // !+16 (absent)
        if (!q.alive) continue;            // !+0 (inactive) / +8 set (dead) — NOT +58 stun
        if (same_team(self, j)) continue;  // team mode: skip a teammate
        return j;                          // any live, non-teammate opponent
    }
    return -1;  // no live opponent
}

// ---------------------------------------------------------------------------
// Behaviour 6 — sub_40B8C2, seek an enemy (docs/re/ai.md §3.6). Structurally the
// twin of behaviour 5 but targets a PLAYER (via pick_live_enemy / sub_422718),
// using the enemy-seek field trio and maxdist 20:
//   - acquire: only while NOT already latched, on a 1/50 whim -> pick a live
//     opponent, store its SLOT + reset the ~10-tick timer + set the latch;
//   - timeout: tick the timer; once it reaches 10, give up on a 1/50 roll (UNLIKE
//     powerup-seek, whose timeout is unconditional — this one keeps the target if
//     the roll fails and the timer keeps growing);
//   - liveness: drop the target if its slot is now gone / inactive / dead
//     (the original drops it when the reloaded actor pointer is null, when the
//     record's leading +0 dword is not 1, or when its +8 dword is set — +8 being
//     died-this-round = our !alive, NOT the +58 stun, so a stunned-but-alive foe
//     is still chased);
//   - path: directed BFS toward the target's tile (maxdist 20); if unreachable
//     (0 iters) give up 50% of the time BEFORE the firstdir check; then step one
//     tile if the next tile is safe (sub_40A59D), else hold.
// It does NOT drop bombs — the bombing of a cornered foe is behaviour 4, above it
// in priority. RNG order within this behaviour (§8 rows b6a..b6d + the inner
// pick_live_enemy draws): %50 acquire -> [pick_live_enemy rand()%10 (+ a 2nd
// %10 if pass 1 is empty) when acquiring] -> [%50 give-up if timed out] -> BFS
// tie-break -> [%2 give-up if unreachable]. Returns true if it acted.
// ---------------------------------------------------------------------------
bool AISystem::behave_seek_enemy(int i, PlayerInput& out) {
    Player& p = s_.players[i];
    Brain& br = s_.brains[i];
    const int px = p.tile_x(), py = p.tile_y();

    // Acquire: only when not already pursuing, on a 1/50 whim. The && short-
    // circuits so the %50 draw is taken ONLY when !active (matches the original).
    if (!br.enemy_seek.active && random_below(s_, 50) == 0) {
        const int slot = pick_live_enemy(i);  // sub_422718 (its own rand()%10 draws)
        if (slot >= 0) {
            br.enemy_seek.active = true;
            br.enemy_seek.timer = 0;
            br.enemy_seek.target_slot = static_cast<std::int8_t>(slot);
        }
    }

    if (!br.enemy_seek.active) return false;  // no target: pass down to behaviour 7

    // Timeout: accrue the frame delta, then at >= 500 ms give up on a 1/50
    // roll. The %50 is evaluated ONLY when the timer condition holds
    // (short-circuit &&), exactly as the original orders it: the "+12 has
    // reached 10 frame-times" test first, the zero-result rand()%50 draw
    // second — +12 accrues frameDelta per
    // displayed frame, so the threshold is 10 × 50 ms of wall clock.
    br.enemy_seek.timer += delta_ms_;
    if (br.enemy_seek.timer >= 10 * kMsPerTick && random_below(s_, 50) == 0) {
        br.enemy_seek.active = false;
        return false;
    }

    // Liveness: the original reloads the actor pointer and drops the target if
    // the pointer is null, if the record's leading +0 dword is not 1, or if its
    // +8 dword is set — +8 is died-this-round = our !alive, NOT the +58 stun; a
    // stunned-but-alive foe
    // is still pursued. Our slot image: give up if the slot is no longer a live
    // player.
    // bugprone-signed-char-misuse (NOLINT below) — target_slot (std::int8_t)
    // is a genuine signed small int; the negative-slot check right below
    // relies on its sign, so casting through unsigned char first would break it.
    const int ts = br.enemy_seek.target_slot;  // NOLINT(bugprone-signed-char-misuse)
    if (ts < 0 || ts >= kMaxPlayers) {
        br.enemy_seek.active = false;
        return false;
    }
    const Player& target = s_.players[ts];
    if (!target.present || !target.alive) {
        br.enemy_seek.active = false;
        return false;
    }

    // Path toward the target's tile (maxdist = 20, the original's literal). One
    // BFS tie-break draw. If unreachable (0 iters) give up 50% of the time BEFORE
    // the firstdir check, exactly as the original orders it (line 10914).
    int iters = 0;
    const int first = directed_bfs(px, py, target.tile_x(), target.tile_y(), 20, iters);
    if (iters == 0 && random_below(s_, 2) != 0) {
        br.enemy_seek.active = false;
    }
    if (first < 0) {
        br.enemy_seek.active = false;
        return false;
    }

    // Take the step, but only if the next tile is safe to stand on (sub_40A59D);
    // otherwise hold (godir -1). The original records the step dir at +20 first.
    br.enemy_seek.step_dir = static_cast<std::int8_t>(first);
    const int nx = px + kDX[first & 3], ny = py + kDY[first & 3];
    write_move(out, safe_tile(nx, ny) ? first : -1);
    return true;  // acted
}

// ---------------------------------------------------------------------------
// Behaviour 7 — sub_40A81F, wander fallback (docs/re/ai.md §3.7). RNG order:
// rand()%25 (pick a new turn?) -> rand()%2 (which +-90 turn) -> [step] ->
// rand()%4 (re-roll when blocked). Returns true if it acted (stepped).
// ---------------------------------------------------------------------------
bool AISystem::behave_wander(int i, PlayerInput& out) {
    Player& p = s_.players[i];
    Brain& br = s_.brains[i];
    const int px = p.tile_x(), py = p.tile_y();

    if (random_below(s_, 25) == 0) {
        // Turn +-90 off the current wander dir. sub_40A81F takes its base from
        // an aliased read of the brain: it loads the dword at brain +62 and
        // keeps its high half, and since that dword spans bytes 62-65 the half
        // it keeps IS wander_dir's own storage at +64. So the base genuinely is
        // the current wander_dir (via the alias, NOT via any "personality 0
        // seeds 0" reasoning), and the new dir is
        // (wander_dir + 2*(rand()%2) - 1) & 3.
        const int turn = (br.wander_dir + 2 * static_cast<int>(random_below(s_, 2)) - 1) & 3;
        // Adopt the new turn only if the CURRENT wander dir is itself safe to
        // step (mirrors the original's guard before overwriting wander_dir).
        if (safe_tile(px + kDX[br.wander_dir & 3], py + kDY[br.wander_dir & 3]))
            br.wander_dir = static_cast<std::int8_t>(turn);
    }

    const int wg = br.wander_dir & 3;
    if (safe_tile(px + kDX[wg], py + kDY[wg])) {
        write_move(out, wg);
        return true;  // step that way
    }
    // Blocked: re-roll the wander dir and pass down (the fallback of the
    // fallback — with nothing below, the AI simply holds still this tick).
    br.wander_dir = static_cast<std::int8_t>(random_below(s_, 4));
    return false;
}

}  // namespace bomber::sim
