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

namespace {

// Unit vectors in the original's godir order (0=Up,1=Right,2=Down,3=Left),
// matching MovementSystem's own table and grid::dir_dx/dir_dy.
constexpr int kDx[4] = {0, 1, 0, -1};
constexpr int kDy[4] = {-1, 0, 1, 0};

}  // namespace

bool RoverSystem::passable(RoverKind kind, int tx, int ty) const {
    const State& s = s_;
    if (!grid::in_grid(tx, ty)) return false;  // sub_425FB9 returns 1 (blocked) OOB
    // Always blocked by a grounded bomb (sub_422E48), both kinds.
    if (grid::bomb_at(s, tx, ty) != nullptr) return false;
    // sub_4017FA: ghost (dword_45E01C==2) passable unless collision code == 1
    // (solid) -- bricks (code 2) do NOT block a ghost. Rover (and anything
    // else) requires collision code == 0 (fully blank) -- bricks DO block a
    // rover, same as grid::tile_open. docs/re/campaign.md mover clause 1.
    if (kind == RoverKind::Ghost) return s.cells[ty][tx] != Cell::Solid;
    return s.cells[ty][tx] == Cell::Blank && s.burning[ty][tx] == 0;
}

void RoverSystem::spawn(RoverKind kind, int count, std::int32_t speed) {
    State& s = s_;
    for (int n = 0; n < count; ++n) {
        bool placed = false;
        for (int attempt = 0; attempt < 200 && !placed; ++attempt) {
            // sub_4019C2: two draws per attempt, ALWAYS both, before any test.
            const int tx = static_cast<int>(random_below(s, kGridWidth));
            const int ty = static_cast<int>(random_below(s, kGridHeight));
            // Spawn placement is NOT type-dependent (unlike the per-tick
            // mover): sub_425FB9(tx,ty) != 1 rejects SOLID only, so a brick
            // tile is a legal spawn for BOTH kinds. docs/re/campaign.md
            // "Spawning".
            if (s.cells[ty][tx] == Cell::Solid) continue;
            if (grid::bomb_at(s, tx, ty) != nullptr) continue;
            // sub_422351(candidateX, candidateY, 3): reject if any player's
            // OWN tile is within Manhattan distance 3 of the CANDIDATE tile.
            // CONFIRMED 2026-07-09 from raw disassembly (docs/re/campaign.md
            // "Spawning" section) -- Hex-Rays had lost the Watcom
            // register-convention 3-argument signature (EAX=candidateX,
            // EDX=candidateY, EBX=threshold) and showed only a single
            // ebx-bound arg, but the call site in sub_4019C2 and the
            // function's own prologue both confirm the candidate tile IS the
            // distance operand, not a per-player self-check. NOTE: the
            // original's 10-slot loop has no presence/liveness guard (every
            // slot is distance-checked unconditionally); this port filters
            // to present+alive players, a deliberate, documented deviation
            // (see campaign.md) with negligible practical effect.
            bool near_player = false;
            for (const auto& p : s.players) {
                if (!p.present || !p.alive) continue;
                const int d = std::abs(p.tile_x() - tx) + std::abs(p.tile_y() - ty);
                if (d <= 3) {
                    near_player = true;
                    break;
                }
            }
            if (near_player) continue;

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
            placed = true;
        }
        // No room found after 200 attempts: mirrors the original, which
        // never checks sub_4019C2's return before incrementing its loop --
        // a full board just yields fewer live actors than requested.
    }
}

bool RoverSystem::step(Rover& r, int rover_index) {
    State& s = s_;
    // sub_401B5C: budget += speed * frameDelta/frameRef + 100. At our locked
    // 20 Hz frameDelta/frameRef == 1 (docs/re/facts.md "Player movement"), so
    // this is speed + 100 1/100-px units per tick -- NOTE the extra flat
    // +100 the rover/ghost budget gets that the player's own speed term does
    // NOT (sub_41F29B never adds a flat +100 to a player's budget): this is
    // a genuine, confirmed asymmetry, not a port bug -- rovers/ghosts always
    // advance at least one pixel per tick even at speed 0.
    r.move_budget += r.speed + 100;

    while (r.move_budget > 0) {
        r.move_budget -= 100;
        ++r.anim_step;

        // sub_401B5C computes the CANDIDATE next pixel position first (one
        // pixel forward in the current dir -- dword_45BECC/BEDC are unit
        // deltas), tests ITS along-axis offset from tile centre (v31), and
        // only runs the turn logic when that candidate lands EXACTLY on a
        // tile centre pixel (v31==0) -- equivalent to "current position is
        // one pixel before centre", but expressed on the candidate so the
        // ahead-tile probe below can reuse the same candidate tile.
        const Fixed cand_x = r.x + kDx[r.dir] * kScale;
        const Fixed cand_y = r.y + kDy[r.dir] * kScale;
        const int cand_tx = cand_x / kTileWF, cand_ty = cand_y / kTileHF;
        const int cpx = cand_x / kScale, cpy = cand_y / kScale;
        const int off_x = ((cpx % kTileW) + kTileW) % kTileW - kTileW / 2;
        const int off_y = ((cpy % kTileH) + kTileH) % kTileH - kTileH / 2;
        // along = dx*offX + dy*offY (matches v31's dy*v34+dx*v33 up to the
        // operand-order rewrite the raw axis helpers sub_426599/sub_4265EB
        // already fold in -- see MovementSystem::move's own along/perp for
        // the same rotation formula on the player's per-pixel stepper).
        const int along = kDx[r.dir] * off_x + kDy[r.dir] * off_y;

        if (along == 0) {
            // Candidate lands exactly on a tile centre: decide whether/how
            // to turn, probing the tile AHEAD OF THE CANDIDATE (one more
            // step past the centre it's about to occupy).
            const int ahead_x = cand_tx + kDx[r.dir], ahead_y = cand_ty + kDy[r.dir];
            if (passable(r.kind, ahead_x, ahead_y)) {
                // Ahead is clear: still a chance to turn anyway (1-in-N,
                // N = max(1,getvalue(1200))). ONE draw.
                const std::int32_t n =
                    s.tuning.rover_turn_chance >= 1 ? s.tuning.rover_turn_chance : 1;
                if (random_below(s, static_cast<std::uint32_t>(n)) == 0) {
                    // SECOND draw: the turn direction.
                    r.dir =
                        static_cast<std::uint8_t>((random_below(s, 2) ? r.dir + 1 : r.dir + 3) & 3);
                }
            } else {
                // Blocked ahead: ALWAYS turn (no roll for the fact of
                // turning, only its +-90 direction -- matches sub_401B5C,
                // which reaches the rand()%2 turn unconditionally once the
                // "ahead passable" branch's own roll is skipped). ONE draw.
                r.dir = static_cast<std::uint8_t>((random_below(s, 2) ? r.dir + 1 : r.dir + 3) & 3);
            }
            // Re-test the (possibly new) ahead-of-candidate tile; if STILL
            // blocked, stop moving for the rest of THIS tick's budget (no
            // overshoot into a wall) -- no further draw.
            if (!passable(r.kind, cand_tx + kDx[r.dir], cand_ty + kDy[r.dir])) r.move_budget = 0;
        }

        // Commit: the original always writes v29/v30 (the ORIGINAL
        // candidate, computed with the PRE-turn direction) as the new
        // position -- a turn taken this pixel only steers the NEXT step, it
        // does not redirect the pixel already in flight. So commit cand_x/
        // cand_y, not a recompute with the (possibly just-changed) r.dir.
        r.x = cand_x;
        r.y = cand_y;

        const int ntx = cand_tx, nty = cand_ty;
        // Flame death (sub_42708D + sub_421C71): dies stepping into an
        // active flame tile; the flame's OWNER (State::flame_owner, stamped
        // by FlameSystem the same tick the flame is present) is awarded the
        // rover/ghost kill-score VALUELST id. The score value itself is
        // exposed via Tuning (rover_kill_score/ghost_kill_score, ids
        // 1310/1320) for the campaign layer to apply -- libs/sim has no
        // running "score" field of its own to add to (docs/re/campaign.md
        // port status: campaign scoring lives above the sim, like the
        // AI-kill-score id 1300 already does).
        if (grid::in_grid(ntx, nty) && s.flame[nty][ntx] > 0) {
            const std::uint8_t owner = s.flame_owner[nty][ntx];
            s.events.push_back({Event::Type::RoverDied, static_cast<std::int8_t>(rover_index),
                                static_cast<std::int8_t>(ntx), static_cast<std::int8_t>(nty),
                                static_cast<std::int8_t>(owner)});
            r.alive = false;
            return false;
        }

        // Landing-tile kill (sub_421CB5 + sub_41DE63): a live, non-COMPUTER
        // player found on the actor's new tile is killed outright -- NOT a
        // punch/stun (docs/re/campaign.md mover clause 4, the sub_41DE63
        // correction). AI-controlled players are immune. Owner == -1 (no
        // kill-tally credit for the rover/ghost itself, matching
        // sub_41DCB2's a2>=0 guard).
        for (int i = 0; i < kMaxPlayers; ++i) {
            Player& victim = s.players[i];
            if (!victim.present || !victim.alive) continue;
            if (victim.tile_x() != ntx || victim.tile_y() != nty) continue;
            if (victim.ai) continue;  // COMPUTER slots pass through unharmed
            victim.alive = false;
            if (victim.carrying) {
                victim.carrying = false;
                if (s.players[victim.carried_owner].bombs_placed > 0)
                    --s.players[victim.carried_owner].bombs_placed;
            }
            // Rover/ghost landing kill routes through the same shared death
            // funnel (sub_41DE63 -> anim -> sub_41DBFE), so the victim scatters
            // its powerups too (docs/re/facts.md "Death powerup scatter").
            // Campaign-only path — inert in every non-campaign golden scenario.
            PowerupSystem{s}.death_scatter(victim);
            s.events.push_back({Event::Type::RoverKilledPlayer, static_cast<std::int8_t>(i),
                                static_cast<std::int8_t>(ntx), static_cast<std::int8_t>(nty),
                                static_cast<std::int8_t>(rover_index)});
            s.events.push_back({Event::Type::PlayerDied, static_cast<std::int8_t>(i),
                                static_cast<std::int8_t>(ntx), static_cast<std::int8_t>(nty), -1});
        }
    }
    return true;
}

void RoverSystem::tick() {
    State& s = s_;
    hazards_just_cleared_ = false;
    // True no-op (zero RNG draws, zero events, zero cost) for every
    // non-campaign scenario (every existing golden scenario): the ENTIRE
    // function body below is gated on campaign_hazards_active, which only
    // build_state sets, and only when MatchConfig::campaign_rovers/
    // campaign_ghosts > 0.
    if (!s.campaign_hazards_active) return;

    int live = 0;
    if (!s.rovers.empty()) {
        for (int i = 0; i < static_cast<int>(s.rovers.size()); ++i) {
            Rover& r = s.rovers[static_cast<std::size_t>(i)];
            if (!r.alive) continue;
            if (step(r, i)) ++live;
        }
        s.rovers.erase(std::remove_if(s.rovers.begin(), s.rovers.end(),
                                      [](const Rover& r) { return !r.alive; }),
                       s.rovers.end());
    }

    // Round pacing clause 3 (docs/re/campaign.md): grace timer counts ticks
    // (not wall-clock ms -- kHazardClearTicks is the ms->tick simplification)
    // while every hazard is dead; held at 0 while any is alive. Runs even
    // once `rovers` is empty (all hazards dead) as long as this WAS a
    // campaign match with hazards -- mirrors dword_4646C0 continuing to
    // accumulate in the original after the last monster dies.
    if (live > 0) {
        s.hazard_clear_timer = 0;
    } else {
        ++s.hazard_clear_timer;
        if (s.hazard_clear_timer == kHazardClearTicks) hazards_just_cleared_ = true;
    }
}

}  // namespace bomber::sim
