// Initial match state construction (Simulation constructor backend). The RNG
// draw order here is part of the determinism contract: rovers/ghosts first (zero
// draws when both counts are 0), then the brick-hide scatter, then the dud gate.

#include <algorithm>
#include <array>
#include <cstddef>

#include "bomber/sim/rng.hpp"
#include "bomber/sim/simulation.hpp"
#include "grid.hpp"
#include "systems/powerups.hpp"
#include "systems/rovers.hpp"

namespace bomber::sim::detail {
namespace {

// PROVEN DIVERGENCE, not a missing citation (facts.md "Spawn-pocket clear",
// 2026-07-21 native probe): running the ORIGINAL's own sub_4260F5 fill on
// BASIC.SCH 4000x shows a spawn tile is a brick ~90% of the time — exactly the
// density, no exception — and forcing a brick onto a spawn then running the real
// sub_4214BC placement leaves it a brick. The original places a brick ON the
// spawn and NEVER clears it: a player spawns boxed in and bombs its way out.
//
// So this clear reproduces no original function. It is a deliberate workaround
// for the clean-room AI-flee logic failing at the boxed-in opening the
// original's AI survives, and the faithful fix is to repair that AI path and
// DELETE this (then recapture goldens B/C). The shape is the smallest that keeps
// the AI alive meanwhile: a radius-2 cross, because radius-1 let a flame-2 spawn
// bomb seal every reachable cell — hence the suicides.
void clear_spawn_pocket(State& s, int tx, int ty) {
    static constexpr std::array<int, 9> ndx = {0, 1, -1, 0, 0, 2, -2, 0, 0};
    static constexpr std::array<int, 9> ndy = {0, 0, 0, 1, -1, 0, 0, 2, -2};
    for (std::size_t n = 0; n < ndx.size(); ++n) {
        const int cx = tx + ndx[n], cy = ty + ndy[n];
        if (grid::in_grid(cx, cy) && s.cells[cy][cx] == Cell::Brick) s.cells[cy][cx] = Cell::Blank;
    }
}

void place_player(State& s, const MatchConfig& config, int i, PowerupSystem& powerups) {
    Player& p = s.players[i];
    p.present = true;
    p.alive = true;
    p.ai = config.ai[i];      // computer-driven slot (ADR-0005); default false
    p.team = config.team[i];  // setup-screen +84 byte (docs/re/setup-screens.md)
    // Ice/input-lag ring buffer (facts.md "Ice / input-lag"): -1 is the
    // original's own fresh-buffer godir, not a plain zero-init, which would read
    // as a phantom godir-0 "Up" for the first ceil(delay/50) ticks. sub_4214BC
    // (pseudo.c ~23880-23890) fills every player's 30 slots with (age 0, dir -1)
    // from the PER-ROUND match-setup sequence, so the buffer is not a stale
    // process-global (an earlier note wrongly claimed it was).
    p.ice_history.fill(-1);

    const int tx = std::clamp(config.spawns[i].x, 0, kGridWidth - 1);
    const int ty = std::clamp(config.spawns[i].y, 0, kGridHeight - 1);
    clear_spawn_pocket(s, tx, ty);
    p.x = grid::tile_center_x(tx);
    p.y = grid::tile_center_y(ty);
    // AI behaviour-4's reset snapshot is the original's actor +20/+24, which
    // sub_4214BC writes at spawn (batch_0x420D4E.cpp:402-403) and warp step-on
    // rewrites to the warp exit. warp_to_x/y IS that field, so seeding it to the
    // SPAWN TILE completes the dual use: behave_bomb_enemy gates on distance
    // travelled FROM this snapshot — 0 right after spawn or warp, so the gate is
    // FALSE — rather than on absolute map-coordinate magnitude (ai.md finding 1).
    // warp_to is overwritten before it is ever read for a real warp.
    p.warp_to_x = tx;
    p.warp_to_y = ty;

    // All 13 per-kind starting-inventory baselines come from VALUELST ids 50-62:
    // sub_4214BC loops j = 0..14 writing getvalue(50+j) into the inventory byte
    // at +86+j (batch_0x420D4E.cpp:427-428). It is an unconditional raw byte
    // write with no eviction and no clamp — counted kinds take the value, flag
    // kinds are set when the baseline is nonzero. Silent under the shipped
    // VALUELST (every kind but bomb=1/flame=2 is 0) but load-bearing for a custom
    // one setting Kick/Skate/... nonzero.
    //
    // This is ALSO the scheme's "-P born with" path: that field is a COUNT which
    // REPLACES VALUELST id 50+kind (sub_403EEE's tail calls the value-table
    // setter sub_4121BF — facts.md), so build_match_config folds it into
    // start_with. Replaying it through PowerupSystem::apply instead was additive
    // AND, because apply runs the pickup dispatcher's mutual-exclusion evictions,
    // could scatter a token and draw State::rng at setup, which sub_4214BC's raw
    // byte write never does.
    p.max_bombs = s.tuning.start_with[static_cast<int>(PowerupType::ExtraBomb)];
    p.flame = s.tuning.start_with[static_cast<int>(PowerupType::Flame)];
    p.skates = s.tuning.start_with[static_cast<int>(PowerupType::Skate)];
    p.kick = s.tuning.start_with[static_cast<int>(PowerupType::Kick)] > 0;
    p.punch = s.tuning.start_with[static_cast<int>(PowerupType::Punch)] > 0;
    p.grab = s.tuning.start_with[static_cast<int>(PowerupType::Grab)] > 0;
    p.spooge = s.tuning.start_with[static_cast<int>(PowerupType::Spooger)] > 0;
    p.goldflame = s.tuning.start_with[static_cast<int>(PowerupType::Goldflame)] > 0;
    p.trigger = s.tuning.start_with[static_cast<int>(PowerupType::Trigger)] > 0;
    p.jelly = s.tuning.start_with[static_cast<int>(PowerupType::Jelly)] > 0;
    // Skates fold into the walk speed: the original applies the skate factor
    // per-tick, the port bakes it into `speed`, matching PowerupSystem.
    p.speed = s.tuning.start_speed + p.skates * s.tuning.skate_speed_bonus;

    // Goldman wheel award (docs/re/goldman-roulette.md §4/§8) is one more
    // born-with unit, not a distinct grant mechanism — sub_4214BC simply
    // increments the inventory byte at +86 + prize — so it goes through apply,
    // AFTER the baseline above.
    for (int k = 0; k < kPowerupKinds; ++k)
        if (config.born_with_extra[i][k]) powerups.apply(p, static_cast<PowerupType>(k));
    // Clogs sit outside the kPowerupKinds/apply space (§9.2), so they fold into
    // speed directly, mirroring skates' term with the sign flipped —
    // sub_41F29B's per-tick speed expression subtracts clogs × penalty from the
    // running total (§9.1). Order matters: skates first (via apply), then clogs,
    // matching the original's single combined expression.
    p.clogs = config.born_with_clogs[i];
    p.speed -= p.clogs * s.tuning.clogs_speed_penalty;
}

// Hide powerups under randomly chosen bricks, faithful to sub_4258E5's
// non-network branch (setup.md finding 1; batch_0x42583B.cpp:222-255, pseudo.c
// 26647-26679): INDEPENDENT REJECTION SAMPLING per unit, not list-removal.
//
//   - a positive count latches the original's "no gate" flag and places every
//     unit unconditionally; a negative count -N attempts |N| units, each gated
//     by a 1-in-10 roll INTERLEAVED immediately before that unit's own scan (the
//     original ORs the latch with a zero-result rand()%10, so the gate draws only
//     on the negative path — not batched up front the way the old port did);
//   - each attempted unit draws a fresh (x, y): rand()%W then rand()%H, x FIRST,
//     retrying up to 200 times for a Brick with no powerup record yet. If all 200
//     miss, the unit is SILENTLY DROPPED — no side-list, no compensating retry.
//     That under-placement on a sparse or crowded board is the original's.
//
// This is setup-only reproducibility fidelity, not real-game bit-matching: the
// real game seeds its brick fill off the wall clock (docs/valuelst-map.md).
void hide_powerups_under_bricks(State& s, const MatchConfig& config) {
    for (int k = 0; k < kPowerupKinds; ++k) {
        // NO forbidden-skip: sub_4258E5 hides by COUNT alone and never consults
        // the per-powerup forbidden flag, which gates only the Random re-roll
        // (the 0xC case's dword_4647E0). A former `if (forbidden) continue;` here
        // skipped the draws the native makes for a forbidden-but-nonzero-count
        // kind — dormant on every shipped scheme, since they zero such a count,
        // but it would desync a user scheme that forbids a powerup and leaves its
        // count > 0.
        std::int32_t want = config.spawn_override[k] > MatchConfig::kNoOverride
                                ? config.spawn_override[k]
                                : s.tuning.spawn_counts[k];
        const bool always = want >= 0;  // the original's "positive count, no gate" latch
        if (want < 0) want = -want;
        for (std::int32_t m = 0; m < want; ++m) {
            if (!always && random_below(s, 10) != 0) continue;
            for (int n = 0; n < 200; ++n) {
                const int rx = static_cast<int>(random_below(s, kGridWidth));
                const int ry = static_cast<int>(random_below(s, kGridHeight));
                if (s.cells[ry][rx] == Cell::Brick && s.hidden[ry][rx] == PowerupType::None &&
                    s.floor[ry][rx] == PowerupType::None) {
                    s.hidden[ry][rx] = static_cast<PowerupType>(k);
                    break;
                }
            }
        }
    }
}

}  // namespace

State build_state(const MatchConfig& config) {
    State s;
    s.rng = config.seed;
    s.tuning = config.tuning;
    s.forbidden = config.forbidden;
    s.ticks_left = config.tuning.game_seconds * kTicksPerSecond;
    // Round-start input freeze: sub_4214BC arms dword_4621E0 with 50 x
    // getvalue(30) (see Tuning::input_freeze_ticks / State::input_freeze).
    s.input_freeze = config.tuning.input_freeze_ticks;
    s.cells = config.cells;
    for (auto& row : s.hidden) row.fill(PowerupType::None);
    for (auto& row : s.floor) row.fill(PowerupType::None);
    // Stage actors are a static per-match layer (docs/re/stage-actors.md),
    // copied verbatim from the parsed EXTRA<N>.RES layout and then hashed.
    s.actor_type = config.actor_type;
    s.actor_dir = config.actor_dir;
    s.warp_dest_x = config.warp_dest_x;  // pre-resolved warphole exits (no sim RNG)
    s.warp_dest_y = config.warp_dest_y;

    PowerupSystem powerups{s};
    for (int i = 0; i < config.player_count && i < kMaxPlayers; ++i) {
        if (i >= static_cast<int>(config.spawns.size())) continue;  // no spawn -> skip
        if (!config.active[i]) continue;                            // OFF slot (setup screen)
        place_player(s, config, i, powerups);
    }

    // Campaign rover/ghost hazards (docs/re/campaign.md "sub_40151B — the REAL
    // per-stage starter"): ghosts THEN rovers (sub_401B05 before sub_401AAE),
    // both AFTER players are placed — the spawn's distance-3-from-every-player
    // gate needs live positions — and BEFORE the brick-hide pass. A zero count
    // draws no RNG at all, so a non-campaign config leaves the stream untouched.
    s.campaign_hazards_active = config.campaign_rovers > 0 || config.campaign_ghosts > 0;
    RoverSystem rover_system{s};
    rover_system.spawn(RoverKind::Ghost, config.campaign_ghosts, config.campaign_ghost_speed);
    rover_system.spawn(RoverKind::Rover, config.campaign_rovers, config.campaign_rover_speed);

    hide_powerups_under_bricks(s, config);

    // Arm the dud gate once at match init (sub_422C7A -> sub_422C13): base +
    // rand(spread) SECONDS ahead, per VALUELST 320/321's own legend
    // ("minimum/additional random number of SECONDS between potential dud
    // bombs") — 3-6 minutes, converted to ticks. facts.md "Dud bombs".
    s.dud_gate =
        (static_cast<std::uint64_t>(s.tuning.dud_gate_base) +
         random_below(
             s, static_cast<std::uint32_t>(std::max<std::int32_t>(1, s.tuning.dud_gate_rand)))) *
        kTicksPerSecond;
    return s;
}

}  // namespace bomber::sim::detail
