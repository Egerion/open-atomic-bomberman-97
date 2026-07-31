// Campaign rover/ghost hazard actors. Faithful port of sub_401AAE/sub_401B05
// (spawn), sub_401F76 (drive loop), sub_401B5C (mover). See
// docs/re/campaign.md "Rover/ghost/AI roster" and "Per-tick mover".

#include "systems/rovers.hpp"

#include <algorithm>
#include <cstdlib>

#include "bomber/sim/rng.hpp"
#include "grid.hpp"
#include "systems/powerups.hpp"

namespace bomber::sim {

// The godir unit vectors live in grid.hpp — see the one-definition note there.
using grid::kDx;
using grid::kDy;

namespace {

// sub_422351(candidateX, candidateY, 3): any player's OWN tile within Manhattan
// distance 3 of the CANDIDATE tile. CONFIRMED 2026-07-09 from raw disassembly
// (docs/re/campaign.md "Spawning") — Hex-Rays had lost the Watcom
// register-convention 3-argument signature (EAX/EDX/EBX) and showed a single
// ebx-bound arg, but the call site in sub_4019C2 and the prologue both confirm
// the candidate tile IS the distance operand, not a per-player self-check.
//
// DEVIATION (documented in campaign.md): the original's 10-slot loop has no
// presence/liveness guard and distance-checks every slot unconditionally; this
// filters to present+alive players. Negligible practical effect.
bool near_any_player(const State& s, int tx, int ty) {
    for (const auto& p : s.players) {
        if (!p.present || !p.alive) continue;
        if (std::abs(p.tile_x() - tx) + std::abs(p.tile_y() - ty) <= 3) return true;
    }
    return false;
}

}  // namespace

bool RoverSystem::passable(RoverKind kind, int tx, int ty) const {
    const State& s = s_;
    if (!grid::in_grid(tx, ty)) return false;  // sub_425FB9 returns 1 (blocked) OOB
    if (grid::bomb_at(s, tx, ty) != nullptr) return false;  // sub_422E48, both kinds
    // sub_4017FA: a ghost (dword_45E01C == 2) is passable unless the collision
    // code is 1 (solid), so bricks (code 2) do NOT block it; a rover needs code 0.
    if (kind == RoverKind::Ghost) return s.cells[ty][tx] != Cell::Solid;
    return s.cells[ty][tx] == Cell::Blank && s.burning[ty][tx] == 0;
}

void RoverSystem::place_one(RoverKind kind, std::int32_t speed) {
    State& s = s_;
    for (int attempt = 0; attempt < 200; ++attempt) {
        // sub_4019C2: two draws per attempt, ALWAYS both, before any test.
        const int tx = static_cast<int>(random_below(s, kGridWidth));
        const int ty = static_cast<int>(random_below(s, kGridHeight));
        // Spawn placement is NOT type-dependent, unlike the per-tick mover:
        // sub_425FB9(tx,ty) != 1 rejects SOLID only, so a brick tile is a legal
        // spawn for BOTH kinds (docs/re/campaign.md "Spawning").
        if (s.cells[ty][tx] == Cell::Solid) continue;
        if (grid::bomb_at(s, tx, ty) != nullptr) continue;
        if (near_any_player(s, tx, ty)) continue;

        Rover r;
        r.alive = true;
        r.kind = kind;
        r.x = grid::tile_center_x(tx);
        r.y = grid::tile_center_y(ty);
        r.speed = speed;
        s.rovers.push_back(r);
        s.events.push_back({Event::Type::RoverSpawned,
                            static_cast<std::int8_t>(s.rovers.size() - 1),
                            static_cast<std::int8_t>(tx), static_cast<std::int8_t>(ty),
                            static_cast<std::int8_t>(kind)});
        return;
    }
    // No room after 200 attempts: mirrors the original, which never checks
    // sub_4019C2's return before incrementing its loop — a full board just yields
    // fewer live actors than requested.
}

void RoverSystem::spawn(RoverKind kind, int count, std::int32_t speed) {
    for (int n = 0; n < count; ++n) place_one(kind, speed);
}

// The candidate step lands exactly on a tile centre, so decide whether and how
// to turn, probing the tile AHEAD OF THE CANDIDATE (one more step past the
// centre it is about to occupy). Ahead clear: a 1-in-N roll to turn anyway
// (N = max(1, getvalue(1200))), and only on a win a second draw for the ±90
// direction. Ahead blocked: ALWAYS turn — sub_401B5C reaches the rand()%2
// unconditionally once the "ahead passable" branch's own roll is skipped — so
// exactly ONE draw. Either way, re-test the (possibly new) ahead tile and stop
// for the rest of this tick's budget if it is still blocked, with no further
// draw and no overshoot into a wall.
void RoverSystem::turn_at_centre(Rover& r, int cand_tx, int cand_ty) {
    State& s = s_;
    const bool ahead_clear = passable(r.kind, cand_tx + kDx[r.dir], cand_ty + kDy[r.dir]);
    const std::int32_t n = s.tuning.rover_turn_chance >= 1 ? s.tuning.rover_turn_chance : 1;
    // Short-circuit order matters: a blocked-ahead actor must NOT draw the %n.
    const bool turning = !ahead_clear || random_below(s, static_cast<std::uint32_t>(n)) == 0;
    if (turning)
        r.dir = static_cast<std::uint8_t>((random_below(s, 2) ? r.dir + 1 : r.dir + 3) & 3);
    if (!passable(r.kind, cand_tx + kDx[r.dir], cand_ty + kDy[r.dir])) r.move_budget = 0;
}

// A live, non-COMPUTER player on the actor's new tile is killed outright — NOT a
// punch or stun (docs/re/campaign.md mover clause 4, the sub_41DE63 correction).
// AI-controlled players are immune. The PlayerDied owner is -1: no kill-tally
// credit for the rover/ghost itself, matching sub_41DCB2's a2 >= 0 guard.
void RoverSystem::kill_players_on(int tx, int ty, int rover_index) {
    State& s = s_;
    for (int i = 0; i < kMaxPlayers; ++i) {
        Player& victim = s.players[i];
        if (!victim.present || !victim.alive) continue;
        if (victim.tile_x() != tx || victim.tile_y() != ty) continue;
        if (victim.ai) continue;  // COMPUTER slots pass through unharmed
        // sub_41DE63, this kill's funnel, early-outs while the victim's state
        // word +78 is 5 (hop) or 6/7 (warp) — pseudo.c 21999-22006, the same
        // exemption as the enclosure crush and the flame kill.
        if (victim.bounce > 0 || victim.warp > 0) continue;
        victim.alive = false;
        if (victim.carrying) {
            victim.carrying = false;
            if (s.players[victim.carried_owner].bombs_placed > 0)
                --s.players[victim.carried_owner].bombs_placed;
        }
        // Same shared death funnel as a flame kill, so the victim scatters too
        // (facts.md "Death powerup scatter").
        PowerupSystem{s}.death_scatter(victim);
        s.events.push_back({Event::Type::RoverKilledPlayer, static_cast<std::int8_t>(i),
                            static_cast<std::int8_t>(tx), static_cast<std::int8_t>(ty),
                            static_cast<std::int8_t>(rover_index)});
        s.events.push_back({Event::Type::PlayerDied, static_cast<std::int8_t>(i),
                            static_cast<std::int8_t>(tx), static_cast<std::int8_t>(ty), -1});
    }
}

bool RoverSystem::step(Rover& r, int rover_index) {
    State& s = s_;
    // sub_401B5C: budget += speed * frameDelta/frameRef + 100, once per DISPLAYED
    // frame — kSubFrames accruals per 50 ms tick. The flat +100/frame term is a
    // confirmed asymmetry the player's own budget never gets (an actor always
    // advances at least one pixel per frame even at speed 0), and it multiplies
    // to +900/tick, which is why the original's rovers visibly outpace a
    // same-speed walker. Folding the nine frames into one accrual is EXACT, not
    // an approximation: the field is static during the rover pass and the
    // per-pixel turn logic is a pure function of the pixels crossed, so nine
    // small instalments and one summed instalment walk the identical sequence.
    for (int f = 0; f < kSubFrames; ++f)
        r.move_budget += frame_budget(r.speed, kSubFrameMs[f]) + 100;

    while (r.move_budget > 0) {
        r.move_budget -= 100;
        ++r.anim_step;

        // sub_401B5C computes the CANDIDATE next pixel position first (one pixel
        // forward in the current dir), tests ITS along-axis offset from the tile
        // centre, and runs the turn logic only when that offset is zero — i.e.
        // the candidate lands exactly on a centre pixel. Equivalent to "the
        // current position is one pixel before centre", but expressed on the
        // candidate so the ahead-tile probe can reuse the candidate tile.
        const Fixed cand_x = r.x + kDx[r.dir] * kScale;
        const Fixed cand_y = r.y + kDy[r.dir] * kScale;
        const int cand_tx = cand_x / kTileWF, cand_ty = cand_y / kTileHF;
        const int cpx = cand_x / kScale, cpy = cand_y / kScale;
        const int off_x = ((cpx % kTileW) + kTileW) % kTileW - kTileW / 2;
        const int off_y = ((cpy % kTileH) + kTileH) % kTileH - kTileH / 2;
        // The same sum the original forms from its two per-axis offsets, up to
        // the operand-order rewrite sub_426599/sub_4265EB already fold in — see
        // MovementSystem::move for the same rotation formula on the player's
        // per-pixel stepper.
        const int along = kDx[r.dir] * off_x + kDy[r.dir] * off_y;
        if (along == 0) turn_at_centre(r, cand_tx, cand_ty);

        // Commit the CANDIDATE, computed with the PRE-turn direction: a turn
        // taken this pixel only steers the NEXT step, it does not redirect the
        // pixel already in flight.
        r.x = cand_x;
        r.y = cand_y;

        // Flame death (sub_42708D + sub_421C71): the flame's OWNER, stamped by
        // FlameSystem the same tick, is awarded the kill-score VALUELST id. The
        // score value itself is exposed via Tuning for the campaign layer to
        // apply — libs/sim has no running "score" field of its own.
        //
        // sub_401B5C does NOT break out of the pixel-budget loop on flame contact
        // (verified over 0x401E24-0x401E82): it sets the dead flag, falls through
        // into the landing-kill check below, and jumps unconditionally from
        // 0x401ED3 back to the loop top at 0x401C0F, consuming the REST of this
        // tick's budget. That can re-enter this branch on a later tile crossed in
        // the same tick, re-awarding the kill-score each time — the dead flag is
        // never consulted inside the loop, only at the top of the NEXT call. So:
        // mark dead, push the event, do NOT return. docs/re/audit/
        // tileregen_rovers.md Finding 1.
        if (grid::in_grid(cand_tx, cand_ty) && s.flame[cand_ty][cand_tx] > 0) {
            s.events.push_back({Event::Type::RoverDied, static_cast<std::int8_t>(rover_index),
                                static_cast<std::int8_t>(cand_tx),
                                static_cast<std::int8_t>(cand_ty),
                                static_cast<std::int8_t>(s.flame_owner[cand_ty][cand_tx])});
            r.alive = false;
        }

        kill_players_on(cand_tx, cand_ty, rover_index);
    }
    // r.alive may have gone false mid-loop without stopping it, so return the
    // final state for tick()'s live-count and erase-if-dead.
    return r.alive;
}

void RoverSystem::tick() {
    State& s = s_;
    hazards_just_cleared_ = false;
    // A true no-op — zero draws, zero events, zero cost — for every non-campaign
    // scenario: the ENTIRE body below is gated here, and only build_state sets
    // this flag, only when campaign_rovers/campaign_ghosts > 0.
    if (!s.campaign_hazards_active) return;

    int live = 0;
    for (int i = 0; i < static_cast<int>(s.rovers.size()); ++i) {
        Rover& r = s.rovers[static_cast<std::size_t>(i)];
        if (r.alive && step(r, i)) ++live;
    }
    s.rovers.erase(
        std::remove_if(s.rovers.begin(), s.rovers.end(), [](const Rover& r) { return !r.alive; }),
        s.rovers.end());

    // Round pacing clause 3 (docs/re/campaign.md): the grace timer counts ticks
    // while every hazard is dead and is held at 0 while any is alive. It keeps
    // running once `rovers` is empty, mirroring dword_4646C0 continuing to
    // accumulate after the last monster dies.
    if (live > 0) {
        s.hazard_clear_timer = 0;
        return;
    }
    ++s.hazard_clear_timer;
    if (s.hazard_clear_timer == kHazardClearTicks) hazards_just_cleared_ = true;
}

}  // namespace bomber::sim
