// AI behaviour chain: the eight behave_* behaviours (off_45BA78[8]) and the
// enemy finder (pick_live_enemy / sub_422718, same_team). See ai.hpp for the
// subsystem API and docs/re/ai.md for the RE facts; the dispatcher that runs the
// chain stays in ai.cpp.
//
// Each behaviour's RNG draws — which ones, and under which gates — are part of
// the determinism contract (docs/re/ai.md §8). Where a `&&` short-circuits a
// draw away, that is deliberate and mirrors the original's own ordering.

#include "systems/ai.hpp"

#include <algorithm>  // std::max for the blast-bricks getvalue(915) clamp
#include <array>

#include "bomber/sim/rng.hpp"
#include "grid.hpp"

namespace bomber::sim {

namespace {

// Behaviour 4's enemy-scan cross (sub_40ABED, docs/re/ai.md §3.4 / §9.4). The
// original walks i in 0..4 reading dword_45BAB0[i] for the X offset and
// dword_45BA9C[i] for the Y offset. dword_45BA9C[5] = {0,-1,0,1,0} is a clean
// vertical cross; dword_45BAB0[] is a ONE-element array {-1}, so reading [1..4]
// runs PAST it into the adjacent .data — an original out-of-bounds read. Reading
// the shipped BM95.EXE bytes at 0x45BAB0..0x45BAC0 gives {-1,0,0,0,1} for those
// five dwords. Combined with the Y table that yields a PERFECT 5-tile cross
// centred on the AI: LEFT, UP, SELF(0,0), DOWN, RIGHT. These constants are a
// faithful image of the OOB read, not a crash — it never faults in practice.
// Index 2 is the AI's own tile; the original excludes self by zeroing its actor
// +0 across the sub_421CB5 probe, which the scan below mirrors by skipping self.
constexpr std::array<int, 5> kEnemyScanX = {-1, 0, 0, 0, 1};  // dword_45BAB0[0..4] (OOB tail)
constexpr std::array<int, 5> kEnemyScanY = {0, -1, 0, 1, 0};  // dword_45BA9C[5]

// sub_421CB5 at pseudo.c 24207, for ONE tile: the first player standing there
// whose leading +0 dword is nonzero and whose +8 dword is zero — +8 is
// died-this-round, NOT the +58 stun, so a stunned-but-alive enemy still counts.
// Self is excluded. Returns the slot, or -1.
int live_player_at(const State& s, int self, int tx, int ty) {
    for (int j = 0; j < kMaxPlayers; ++j) {
        if (j == self) continue;
        const Player& q = s.players[j];
        if (q.present && q.alive && q.tile_x() == tx && q.tile_y() == ty) return j;
    }
    return -1;
}

}  // namespace

// Behaviour 0 — sub_40BD44, grab-glove drop/hold (docs/re/ai.md §3.0). Writing
// the bomb key UP while carrying is NOT an indefinite hold: the mover's throw
// block fires on !+56, so it LOBS the bomb the very next tick (§3.0 CORRECTION).
//
// RNG (§8 row b0): the rand()%2 fires ONLY when grab && !carrying && own bomb
// underfoot, and the draw is used AS-IS, not negated — pseudo.c 11025 wants a
// bomb underfoot, its +62 owner word equal to the AI's own, and rand()%2 NONZERO.
bool AISystem::behave_grab_drop(int i, PlayerInput& out) {
    const Player& p = s_.players[i];
    if (!p.grab) return false;  // sub_40BD44: !+92 -> not our behaviour

    if (p.carrying) {
        release_bomb(i, out);
        return true;  // act, short-circuit the chain
    }

    // sub_422E48 (pseudo.c 25031-25051) matches a RESTING **or SLIDING** bomb
    // (motion != flying(2) && != carried(3)), which is exactly what grid::bomb_at
    // already selects. The original does NOT additionally require the bomb to be
    // at rest: an earlier `!under->moving` guard here silently dropped the whole
    // draw for a sliding own bomb, where the original still rolls.
    const Bomb* under = grid::bomb_at(s_, p.tile_x(), p.tile_y());
    const bool own = under != nullptr && under->owner == static_cast<std::uint8_t>(i);
    if (own && random_below(s_, 2) != 0) {
        press_bomb(i, out);  // fresh bomb-key edge -> try_grab in player_turn
        return true;
    }
    return false;
}

// Behaviour 2 — sub_40B20F, "walk the path" (docs/re/ai.md §3.2). The
// danger-present branch either paths to a held directed target (Stage 3,
// sub_4092A1) or flees to the safest reachable tile (Stage 2, sub_40970B); the
// danger-clear branch passes down unless boxed in. Exactly ONE BFS runs either
// way, so exactly one BFS tie-break draw is taken this tick (§8 row b2).
bool AISystem::behave_walk_path(int i, PlayerInput& out) {
    const Player& p = s_.players[i];
    Brain& br = s_.brains[i];
    const int px = p.tile_x(), py = p.tile_y();

    if (danger_at(px, py) == 0) return walk_path_safe_branch(i, out);

    // Stale-target invalidation (line 10784): with the +2 target-held flag set
    // and the +8 captured cost zero, a now-dangerous target tile clears +2.
    if (br.has_path_target && br.path_target_cost == 0 &&
        danger_at(br.path_target_x, br.path_target_y) != 0) {
        br.has_path_target = false;
    }

    if (br.has_path_target) {
        // Directed: walk one step toward the held goal, maxdist 20 (the
        // original's literal). No path within depth -> the original clears +2
        // and returns 0.
        int iters = 0;
        const int first = directed_bfs(px, py, br.path_target_x, br.path_target_y, 20, iters);
        if (first < 0) {
            br.has_path_target = false;
            return false;
        }
        const int g = flame_veto(i, px, py, first);  // sub_40A76E veto
        write_move(out, g);
        // The original's return here is the high half of +44 compared against -1
        // — the godir word the veto may have just set to -1 (pseudo.c
        // 10841-10842) — NOT an unconditional 1. A flame-vetoed step makes
        // behaviour 2 PASS DOWN, giving 3-7 a turn (and their draws) this tick
        // rather than stalling. RNG-order-critical (§3.2 RESOLVED).
        return g != -1;
    }

    // Flee: no directed goal, run to the lowest-danger reachable tile.
    int bx = 0, by = 0;
    const int first = flee_bfs(px, py, bx, by);  // the flee behaviour's RNG draw

    // Two distinct outcomes, which the 2026-07-12 "danger branch never passes
    // down" correction wrongly folded together (re-pinned 2026-07-16 movement
    // audit, pseudo.c 10816-10829):
    //  1. FULLY BOXED IN (no first direction at all): the original CLEARS the
    //     target flag and returns 0, so behaviours 3-7 DO get this frame's turn
    //     — whims, drop gates and wander re-rolls, with all their RNG draws.
    if (first < 0) {
        br.has_path_target = false;
        return false;
    }
    //  2. NO STRICTLY-SAFER TILE (own danger <= the best found, 10824-10829):
    //     latch the OWN tile, write godir -1 and return 1 — behaviours 3-7 never
    //     run and draw NOTHING that frame.
    if (danger_at(px, py) <= danger_at(bx, by)) {
        br.has_path_target = true;
        br.path_target_x = static_cast<std::int16_t>(px);
        br.path_target_y = static_cast<std::int16_t>(py);
        // ai.md finding 2 (sub_40B20F 607-615, disasm 0x40B3EC-0x40B422): this
        // "can't improve, stand" branch writes ONLY the target tile (+4/+6) and
        // godir -1 — it does NOT touch path_target_cost (+8), and leaving +8
        // stale is load-bearing. The stale-target invalidation above fires on
        // `has_path_target && cost==0 && danger!=0`, so a brain whose +8 is still
        // 0 re-enters the FLEE sub-branch next tick (a fresh flee_bfs and its RNG
        // draw) rather than the DIRECTED one. Writing `cost = here` (always
        // nonzero here) suppressed that, diverging the BFS/RNG sequence in the
        // trapped-in-persistent-danger case. It also never calls the veto — the
        // step is already -1 — so it returns 1 unconditionally, unlike the two
        // stepping branches.
        write_move(out, -1);
        return true;
    }

    br.has_path_target = true;
    br.path_target_x = static_cast<std::int16_t>(bx);
    br.path_target_y = static_cast<std::int16_t>(by);
    br.path_target_cost = danger_at(bx, by);
    const int g = flame_veto(i, px, py, first);
    write_move(out, g);
    return g != -1;  // same "+44's high half != -1" return as the directed branch
}

// Behaviour 2's danger-clear branch. Trigger held and no punch rolls rand()%10
// to detonate remote bombs (Stage 5); the && short-circuits so the draw is taken
// ONLY under both flags, matching the original. NOTE the deliberate asymmetry:
// this is the ONE AI key write the original does not pair with a previous-frame
// clear (sub_40B20F 631-632 sets +57 alone), hence press_action_sustained.
bool AISystem::walk_path_safe_branch(int i, PlayerInput& out) {
    const Player& p = s_.players[i];
    const int px = p.tile_x(), py = p.tile_y();
    if (p.trigger && !p.punch && random_below(s_, 10) == 0) press_action_sustained(out);
    s_.brains[i].has_path_target = false;
    for (int g = 0; g < 4; ++g)
        if (safe_tile(px + grid::kDx[g], py + grid::kDy[g])) return false;  // a way out: pass down
    write_move(out, -1);  // boxed in with nowhere safe: stall and stop the chain
    return true;
}

// Behaviour 3 — sub_40AD8D, blast bricks (docs/re/ai.md §3.3). It does NOT flee
// here: state_flag 9 is a "committed to the drop" marker, and the danger grid
// lights up under the new bomb next tick so behaviour 2 paths the AI out. RNG
// (§8 row b3): the whim fires ONLY once every gate below has passed.
bool AISystem::behave_blast_bricks(int i, PlayerInput& out) {
    const Player& p = s_.players[i];
    Brain& br = s_.brains[i];
    const int px = p.tile_x(), py = p.tile_y();

    // (1) Capacity: sub_4245DA(me) — my live-bomb count — must be < my
    // maxBombs(+86), the disasm-confirmed comparand (§9.3 RESOLVED; NOT a
    // bombs-in-my-column rule). This branch also clears a stale state_flag 9.
    if (out_of_bomb_slots(p)) {
        if (br.state_flag == 9) br.state_flag = 0;
        return false;
    }

    // (2) Constipation (+134). No state clear on this branch, matching the
    // original — its "+134 set -> return 0" test is inside the capacity-ok
    // block, before the brick count and state handling.
    if (p.sick(Disease::Constipation)) return false;

    // (3) Count orthogonally-adjacent BRICK tiles (sub_425FB9 == 2).
    int bricks = 0;
    for (int d = 0; d < 4; ++d) {
        const int nx = px + grid::kDx[d], ny = py + grid::kDy[d];
        if (grid::in_grid(nx, ny) && s_.cells[ny][nx] == Cell::Brick) ++bricks;
    }
    if (bricks == 0) {
        if (br.state_flag == 9) br.state_flag = 0;
        return false;
    }

    // (5) The standing tile must be clear to drop on (sub_423188). Not a
    // look-ahead escape check — behaviour 2 handles the flee next tick.
    if (!drop_tile_clear(px, py)) return false;

    // r = max(1, getvalue(915)); rand()%r != 0 -> pass down, == 0 -> DROP.
    const auto r = static_cast<std::uint32_t>(std::max(1, s_.tuning.ai_blast_chance));
    if (random_below(s_, r) != 0) return false;

    press_bomb(i, out);  // fresh edge -> BombSystem::drop, same dud-gate RNG as a human drop
    br.state_flag = 9;
    return true;
}

// Behaviour 5 — sub_40BAF5, seek a nearby powerup (docs/re/ai.md §3.5). RNG
// order (§8 rows b5a..b5d): %50 acquire -> [scan tie-break if acquiring] -> path
// tie-break -> %2 give-up.
//
// Faithful quirk (§3.5, verified in the decompile): the acquire resets the
// ENEMY-seek timer (+12) rather than the powerup-seek timer (+28) — an original
// field mix-up. NOT inert: behaviour 6 accrues +12 every frame, so this write
// really does postpone an already-active enemy pursuit's give-up roll.
bool AISystem::behave_seek_powerup(int i, PlayerInput& out) {
    const Player& p = s_.players[i];
    Brain& br = s_.brains[i];
    const int px = p.tile_x(), py = p.tile_y();
    const int range = s_.tuning.ai_powerup_range;  // getvalue(920) = 4

    // The && short-circuits so the %50 draw is taken ONLY when !active.
    if (!br.pow_seek.active && random_below(s_, 50) == 0) {
        int iters = 0, fx = 0, fy = 0;
        const int first = powerup_scan_bfs(px, py, range, iters, fx, fy);  // scan tie-break draw
        // Accept only within range+1 steps (the original: `getvalue(920)+1 >= nsteps`).
        if (first >= 0 && range + 1 >= iters) {
            br.pow_seek.active = true;
            br.enemy_seek.timer = 0;  // the +12 write quirk (see header note)
            br.pow_seek.tile_x = static_cast<std::int16_t>(fx);
            br.pow_seek.tile_y = static_cast<std::int16_t>(fy);
        }
    }

    if (!br.pow_seek.active) return false;  // no target: pass down to behaviour 6/7

    // Timeout after 500 ms of pursuit (original: `timer(+28) += frameDelta` per
    // frame; give up at `10 * [0x46494C]`). NO rand draw on this timeout, unlike
    // enemy-seek. The timer is wall-clock ms accrued per sub-frame.
    br.pow_seek.timer += delta_ms_;
    if (br.pow_seek.timer >= 10 * kMsPerTick) {
        br.pow_seek.active = false;
        return false;
    }

    // Liveness: the original reloads the cell pointer and drops the target if it
    // is null or no longer a live powerup (*cell != 2).
    const int tx = br.pow_seek.tile_x, ty = br.pow_seek.tile_y;
    if (!grid::in_grid(tx, ty) || s_.floor[ty][tx] == PowerupType::None) {
        br.pow_seek.active = false;
        return false;
    }

    // Path toward the powerup (maxdist = range+1). One BFS tie-break draw. If
    // unreachable (0 iters) give up 50% of the time BEFORE the firstdir check,
    // exactly as the original orders it (line 10989).
    int iters = 0;
    const int first = directed_bfs(px, py, tx, ty, range + 1, iters);
    if (iters == 0 && random_below(s_, 2) != 0) {
        br.pow_seek.active = false;
    }
    if (first < 0) {
        br.pow_seek.active = false;
        return false;
    }

    // Step, but only if the next tile is safe to stand on (sub_40A59D); else
    // hold. The original records the step dir at +36 first.
    br.pow_seek.step_dir = static_cast<std::int8_t>(first);
    const int nx = px + grid::kDx[first & 3], ny = py + grid::kDy[first & 3];
    write_move(out, safe_tile(nx, ny) ? first : -1);
    return true;
}

// Behaviour 1 — sub_40BE02, punch a bomb ahead (docs/re/ai.md §3.1). try_punch
// uses p.facing, and MovementSystem::move sets facing unconditionally even when
// the step is blocked by the bomb, so writing the direction toward the bomb faces
// the AI at it before the action2 block runs.
//
// RNG (§8 row b1): the rand()%4 sits ABOVE the bomb scan, so a punch-holder with
// no bomb ahead still draws it without acting.
bool AISystem::behave_punch(int i, PlayerInput& out) {
    const Player& p = s_.players[i];
    if (!p.punch) return false;                  // sub_40BE02: !+91 -> not our behaviour
    if (random_below(s_, 4) != 0) return false;  // consider it only 1-in-4

    const int px = p.tile_x(), py = p.tile_y();
    int face = -1;
    for (int g = 0; g < 4; ++g) {  // the 4-element godir cross
        if (grid::bomb_at(s_, px + grid::kDx[g], py + grid::kDy[g]) != nullptr) {
            face = g;
            break;  // first bomb found, in godir order (Up,Right,Down,Left)
        }
    }
    if (face < 0) return false;

    write_move(out, face);
    press_action(i, out);
    return true;
}

// Behaviour 4 — sub_40ABED, drop a bomb next to an enemy (docs/re/ai.md §3.4).
// Byte-exact (0x40ABED): capacity guard, then a Manhattan gate, then a scan of
// the 5-tile cross for a live enemy, then the team/clearance gates and a 1-in-5
// whim. It does NOT flee — behaviour 2 paths the AI out of the new blast next
// tick. RNG (§8 row b4): the rand()%5 fires ONLY once a live enemy is on the
// cross AND the clearance gate passes.
bool AISystem::behave_bomb_enemy(int i, PlayerInput& out) {
    const Player& p = s_.players[i];
    const int px = p.tile_x(), py = p.tile_y();

    // (1) sub_4245DA(me) >= maxBombs(+86) -> return 0, as in behaviour 3.
    if (out_of_bomb_slots(p)) return false;

    // (2) Manhattan gate (ai.md finding 1; disasm-confirmed 0x40AC24-0x40AC6D):
    // the DISTANCE TRAVELLED since the +20/+24 snapshot must be >= 3, with the
    // subtraction done before each abs() call — NOT the absolute-coordinate
    // magnitude the old port and ai.md §3.4's summary used. So the gate is
    // near-constant FALSE right after a spawn or warp, and a freshly-spawned AI
    // will not bomb a cornered enemy until it has moved away. This changes WHEN
    // the rand()%5 is considered, so it is RNG-order-relevant.
    const int dxs = px - p.warp_to_x, dys = py - p.warp_to_y;
    const int mx = dxs < 0 ? -dxs : dxs;
    const int my = dys < 0 ? -dys : dys;
    if (mx + my < 3) return false;

    for (int k = 0; k < 5; ++k) {
        const int tx = px + kEnemyScanX[k], ty = py + kEnemyScanY[k];
        if (!grid::in_grid(tx, ty)) continue;
        const int who = live_player_at(s_, i, tx, ty);
        if (who < 0) continue;

        // Team gate (dword_464964 && me.team == cell.team -> skip; §3.4). A
        // same-team hit is not an enemy, and the ORIGINAL returns 0 here, ending
        // the whole behaviour — it does NOT continue scanning the rest of the
        // cross. On an all-zero roster same_team() is always false. Then the
        // standing tile must be clear to drop on (sub_423188), and the whim.
        if (same_team(i, who)) return false;
        if (!drop_tile_clear(px, py)) return false;  // matches the original's return 0
        if (random_below(s_, 5) != 0) return false;
        press_bomb(i, out);  // bomb-key edge -> BombSystem::drop in player_turn
        return true;
    }
    return false;
}

// A same-team player is not an enemy (docs/re/ai.md §5.3: "in team mode only if
// its team +84 differs"). Our semantics, not RE'd beyond the byte's existence:
// team 0 is "no team" on both sides, so zero never matches zero. Without that
// carve-out a fully-zeroed roster — every existing scenario — would call every
// player a teammate of every other.
bool AISystem::same_team(int a, int b) const {
    const std::uint8_t ta = s_.players[a].team, tb = s_.players[b].team;
    return ta != 0 && ta == tb;
}

// Enemy finder — sub_422718 (docs/re/ai.md §5.3). Picks a live opponent to
// pursue, starting the scan at a random slot so targeting is random, not
// nearest. Two passes, byte-exact: pass 1 skips other computer players (+16 !=
// 1); pass 2 runs only if pass 1 found nothing and drops that test. Each pass
// draws ONE rand()%10 for its start index, and BOTH draws are part of the RNG
// contract regardless of whether a hit is found — so the team filter changes WHO
// is picked, never HOW MANY draws happen.
int AISystem::pick_live_enemy(int self) {
    for (int pass = 0; pass < 2; ++pass) {
        const int start = static_cast<int>(random_below(s_, 10));
        for (int n = 0; n < kMaxPlayers; ++n) {
            const int j = (start + n) % kMaxPlayers;
            if (j == self) continue;  // the original's self-vs-candidate identity test
            const Player& q = s_.players[j];
            if (!q.present) continue;          // !+16 (absent)
            if (pass == 0 && q.ai) continue;   // +16 == 1: pass 1 prefers a human
            if (!q.alive) continue;            // !+0 / +8 set (dead) — NOT the +58 stun
            if (same_team(self, j)) continue;  // team mode: skip a teammate
            return j;
        }
    }
    return -1;  // no live opponent
}

// Behaviour 6 — sub_40B8C2, seek an enemy (docs/re/ai.md §3.6). The twin of
// behaviour 5 but targeting a PLAYER, with ONE deliberate difference: this
// timeout is CONDITIONAL — it gives up on a 1/50 roll and keeps the target when
// the roll fails, while the timer keeps growing.
//
// RNG order (§8 rows b6a..b6d plus pick_live_enemy's own draws): %50 acquire ->
// [pick_live_enemy's %10 draws when acquiring] -> [%50 give-up if timed out] ->
// BFS tie-break -> [%2 give-up if unreachable].
bool AISystem::behave_seek_enemy(int i, PlayerInput& out) {
    const Player& p = s_.players[i];
    Brain& br = s_.brains[i];
    const int px = p.tile_x(), py = p.tile_y();

    // The && short-circuits so the %50 draw is taken ONLY when !active.
    if (!br.enemy_seek.active && random_below(s_, 50) == 0) {
        const int slot = pick_live_enemy(i);  // sub_422718 (its own rand()%10 draws)
        if (slot >= 0) {
            br.enemy_seek.active = true;
            br.enemy_seek.timer = 0;
            br.enemy_seek.target_slot = static_cast<std::int8_t>(slot);
        }
    }

    if (!br.enemy_seek.active) return false;  // no target: pass down to behaviour 7

    // The %50 is evaluated ONLY when the timer condition holds (short-circuit
    // &&), exactly as the original orders it: the "+12 has reached 10
    // frame-times" test first, the zero-result rand()%50 second. +12 accrues
    // frameDelta per displayed frame, so the threshold is 10 x 50 ms of wall
    // clock.
    br.enemy_seek.timer += delta_ms_;
    if (br.enemy_seek.timer >= 10 * kMsPerTick && random_below(s_, 50) == 0) {
        br.enemy_seek.active = false;
        return false;
    }

    // Liveness: the original drops the target when the reloaded actor pointer is
    // null, when the record's leading +0 dword is not 1, or when its +8 dword is
    // set — +8 is died-this-round = our !alive, NOT the +58 stun, so a
    // stunned-but-alive foe is still chased.
    // bugprone-signed-char-misuse (NOLINT below) — target_slot (std::int8_t) is a
    // genuine signed small int and the negative-slot check relies on its sign, so
    // casting through unsigned char first would break it.
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

    // Path toward the target's tile (maxdist 20, the original's literal). One BFS
    // tie-break draw. If unreachable (0 iters) give up 50% of the time BEFORE the
    // firstdir check, exactly as the original orders it (line 10914).
    int iters = 0;
    const int first = directed_bfs(px, py, target.tile_x(), target.tile_y(), 20, iters);
    if (iters == 0 && random_below(s_, 2) != 0) {
        br.enemy_seek.active = false;
    }
    if (first < 0) {
        br.enemy_seek.active = false;
        return false;
    }

    // Step, but only if the next tile is safe to stand on (sub_40A59D); else
    // hold. The original records the step dir at +20 first.
    br.enemy_seek.step_dir = static_cast<std::int8_t>(first);
    const int nx = px + grid::kDx[first & 3], ny = py + grid::kDy[first & 3];
    write_move(out, safe_tile(nx, ny) ? first : -1);
    return true;
}

// Behaviour 7 — sub_40A81F, wander fallback (docs/re/ai.md §3.7). RNG order:
// rand()%25 (pick a new turn?) -> rand()%2 (which +-90 turn) -> [step] ->
// rand()%4 (re-roll when blocked).
bool AISystem::behave_wander(int i, PlayerInput& out) {
    const Player& p = s_.players[i];
    Brain& br = s_.brains[i];
    const int px = p.tile_x(), py = p.tile_y();

    if (random_below(s_, 25) == 0) {
        // sub_40A81F takes its base from an ALIASED read of the brain: it loads
        // the dword at brain +62 and keeps its high half, and since that dword
        // spans bytes 62-65 the half it keeps IS wander_dir's own storage at +64.
        // So the base genuinely is the current wander_dir — via the alias, not
        // via any "personality 0 seeds 0" reasoning.
        const int turn = (br.wander_dir + 2 * static_cast<int>(random_below(s_, 2)) - 1) & 3;
        // Adopt the new turn only if the CURRENT wander dir is itself safe to
        // step (mirrors the original's guard before overwriting wander_dir).
        if (safe_tile(px + grid::kDx[br.wander_dir & 3], py + grid::kDy[br.wander_dir & 3]))
            br.wander_dir = static_cast<std::int8_t>(turn);
    }

    const int wg = br.wander_dir & 3;
    if (safe_tile(px + grid::kDx[wg], py + grid::kDy[wg])) {
        write_move(out, wg);
        return true;
    }
    // Blocked: re-roll and pass down — the fallback of the fallback, with nothing
    // below it, so the AI simply holds still this tick.
    br.wander_dir = static_cast<std::int8_t>(random_below(s_, 4));
    return false;
}

}  // namespace bomber::sim
