// Initial match state construction (Simulation constructor backend).

#include <algorithm>

#include "bomber/sim/rng.hpp"
#include "bomber/sim/simulation.hpp"
#include "grid.hpp"
#include "systems/powerups.hpp"
#include "systems/rovers.hpp"

namespace bomber::sim::detail {

State build_state(const MatchConfig& config) {
    State s;
    s.rng = config.seed;
    s.tuning = config.tuning;
    s.forbidden = config.forbidden;
    s.ticks_left = config.tuning.game_seconds * kTicksPerSecond;
    // Round-start input freeze (sub_4214BC arms dword_4621E0 with 50 × getvalue(30)
    // — see Tuning::input_freeze_ticks / State::input_freeze).
    s.input_freeze = config.tuning.input_freeze_ticks;
    s.cells = config.cells;
    for (auto& row : s.hidden) row.fill(PowerupType::None);
    for (auto& row : s.floor) row.fill(PowerupType::None);
    // Stage actors are a static per-match layer (docs/re/stage-actors.md):
    // copied verbatim from the parsed EXTRA<N>.RES layout, then hashed. None
    // (255) is the empty sentinel, so start every tile empty then overlay.
    for (auto& row : s.actor_type) row.fill(ActorType::None);
    s.actor_dir = config.actor_dir;
    s.warp_dest_x = config.warp_dest_x;  // pre-resolved warphole exits (no sim RNG)
    s.warp_dest_y = config.warp_dest_y;
    for (int y = 0; y < kGridHeight; ++y)
        for (int x = 0; x < kGridWidth; ++x) s.actor_type[y][x] = config.actor_type[y][x];

    PowerupSystem powerups{s};

    for (int i = 0; i < config.player_count && i < kMaxPlayers; ++i) {
        if (i >= static_cast<int>(config.spawns.size())) continue;  // no spawn -> skip
        if (!config.active[i]) continue;                            // OFF slot (setup screen)
        Player& p = s.players[i];
        p.present = true;
        p.alive = true;
        p.ai = config.ai[i];      // computer-driven slot (ADR-0005); default false
        p.team = config.team[i];  // setup-screen +84 byte (docs/re/setup-screens.md); default 0
        // Ice/input-lag ring buffer (docs/re/facts.md "Ice / input-lag"):
        // reset to "no direction" at match setup, FAITHFUL to the original's
        // own per-round reset. sub_4214BC (pseudo.c ~23880-23890) fills every
        // player's 30 slots with (age 0, dir -1) and runs from the per-round
        // match-setup sequence (~14788, right after the level index
        // dword_46499C is resolved) — the buffer is NOT a stale process-global
        // (an earlier note wrongly claimed so). -1 is therefore the original's
        // own fresh-buffer godir, not a plain zero-init (which would read as a
        // phantom godir-0 "Up" for the first ceil(delay/50) ticks).
        p.ice_history.fill(-1);
        int tx = std::clamp(config.spawns[i].x, 0, kGridWidth - 1);
        int ty = std::clamp(config.spawns[i].y, 0, kGridHeight - 1);
        // Spawn-pocket clear: force the landing tile and a 2-tile orthogonal
        // "plus" around it to Blank, regardless of the scheme's random brick
        // roll, so nobody starts a match already sealed inside their own
        // opening bomb.
        //
        // NOT PINNED to a specific original function, despite a real search
        // effort. The board's tile array (`dword_46222C`) has exactly ONE
        // writer in BM95.EXE, `sub_425E36` (pseudo.c ~26781), reached only
        // through its three thin wrappers `sub_425E9B`/`sub_425EFC`/
        // `sub_425F79` or directly. Every one of that family's ~19 call
        // sites was read: bomb-flame burn-through (pseudo.c 7252, 7266,
        // 25669, 27412), netplay tile-sync replication (12149, 12189,
        // 12483, 12637, 22406 — remote-position desync correction, gated on
        // `dword_460058`'s netplay flag), the warphole neighbour clear
        // (26537/26541, already ported — see the warphole comment
        // elsewhere in this codebase), and the HURRY wall drop (27235).
        // None run at match setup or reference the spawn-coordinate arrays
        // (`dword_46460C`/`dword_46465C`). The round-init sequence itself
        // (`sub_410B6E`, pseudo.c ~14689: `sub_4260F5` board build ->
        // `sub_4214BC` player placement -> `sub_4258E5` powerup scatter ->
        // `sub_40551F` rovers -> `sub_40151B` campaign hazards) and the
        // .SCH loader (`sub_403EEE`) were read in full: `sub_4214BC` only
        // stores each player's coordinates (see the ice_history comment
        // above) and clears no cell. `DATA/SCHEMES/BASIC.SCH`'s raw grid is
        // a uniform ':'-candidate field with no blank cells authored near
        // any `-S` spawn, so the shape is not scheme-baked either. No
        // VALUELST id documents a "spawn safe radius"
        // (docs/valuelst-map.md, docs/re/id-audit.md) — the nearest
        // relative is id 695, the UNRELATED tile-*regeneration* clear
        // radius (`sub_422351`, docs/re/facts.md "Per-level tile
        // regeneration"), which gates brick REGROWTH near live players,
        // not initial spawn placement.
        //
        // PROVEN DIVERGENCE (2026-07-21 native probe, facts.md "Spawn-pocket
        // clear"): running the ORIGINAL's own `sub_4260F5` fill on BASIC.SCH
        // 4000x shows every spawn tile is a brick ~90% of the time (== the
        // density, no exception), and forcing a brick onto a spawn then running
        // the real `sub_4214BC` placement leaves it a brick. The original
        // places a brick ON the spawn and NEVER clears it — a player spawns
        // boxed in and bombs its way out (classic high-density opening). So
        // this clear reproduces NO original function; it is a deliberate
        // workaround. The real bug it masks is the clean-room AI-flee logic
        // failing at the boxed-in opening the original's AI survives; the
        // faithful fix is to repair that AI path and DELETE this clear (then
        // recapture goldens B/C). The shape below is the smallest that keeps
        // the AI alive meanwhile: a radius-2 cross so a flame-2 spawn bomb
        // cannot seal every reachable cell (radius-1 did, hence the suicides).
        static constexpr int ndx[] = {0, 1, -1, 0, 0, 2, -2, 0, 0};
        static constexpr int ndy[] = {0, 0, 0, 1, -1, 0, 0, 2, -2};
        for (int n = 0; n < 9; ++n) {
            int cx2 = tx + ndx[n], cy2 = ty + ndy[n];
            if (cx2 >= 0 && cx2 < kGridWidth && cy2 >= 0 && cy2 < kGridHeight &&
                s.cells[cy2][cx2] == Cell::Brick)
                s.cells[cy2][cx2] = Cell::Blank;
        }
        p.x = grid::tile_center_x(tx);
        p.y = grid::tile_center_y(ty);
        // AI behaviour-4 reset snapshot (ai.md finding 1; the original's actor
        // +20/+24, which sub_4214BC writes at spawn — dwords 5 and 6 of the
        // actor record take the spawn tile's X and Y, batch_0x420D4E.cpp:402-403
        // — and which warp step-on rewrites to the warp exit).
        // The port already stores the warp exit in warp_to_x/y at start_warp —
        // it IS the +20/+24 field — so seed it to the SPAWN TILE here to
        // complete the dual use: behave_bomb_enemy gates on distance travelled
        // FROM this snapshot, which is 0 right after spawn/warp (gate FALSE),
        // not the absolute map-coordinate magnitude the old port used. warp_to
        // is overwritten before it is ever read for an actual warp, so this
        // spawn seed does not affect warp relocation.
        p.warp_to_x = tx;
        p.warp_to_y = ty;
        // Seed ALL 13 per-kind starting-inventory baselines from VALUELST ids
        // 50-62 (setup.md finding 2 / powerups.md finding 1; sub_4214BC loops
        // j = 0..14 and writes getvalue(50+j) into the inventory byte at +86+j,
        // batch_0x420D4E.cpp:427-428), not just ExtraBomb/Flame. The original's
        // is an unconditional raw byte write with no eviction/clamp (that only
        // runs for the Goldman-wheel bonus and the scheme born_with overlay
        // below): counted kinds take the value, flag kinds are set when the
        // baseline is nonzero. Silent under the shipped VALUELST (every kind but
        // bomb=1/flame=2 is 0, matching the C++ defaults) but a real gap for a
        // custom VALUELST setting Kick/Skate/... (ids 52-62) nonzero — those 11
        // kinds previously had NO baseline path and silently started at 0.
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
        // skates fold into the walk speed (the original applies the skate factor
        // per-tick; the port bakes it into `speed`, matching PowerupSystem).
        p.speed = s.tuning.start_speed + p.skates * s.tuning.skate_speed_bonus;
        for (int k = 0; k < kPowerupKinds; ++k)
            if (config.born_with[k]) powerups.apply(p, static_cast<PowerupType>(k));
        // Goldman wheel award (docs/re/goldman-roulette.md §4/§8): a per-
        // player overlay applied AFTER the global born_with loop, through the
        // same PowerupSystem::apply path — sub_4214BC simply increments the
        // inventory byte at +86 + prize, which is exactly one more born-with
        // unit, not a distinct grant
        // mechanism. Default all-false, so this is a no-op for every
        // existing config (golden hashes unaffected).
        for (int k = 0; k < kPowerupKinds; ++k)
            if (config.born_with_extra[i][k]) powerups.apply(p, static_cast<PowerupType>(k));
        // Goldman wheel clogs award (docs/re/goldman-roulette.md §9): outside
        // the kPowerupKinds/PowerupSystem::apply space (§9.2), so folded into
        // speed directly here, mirroring skates' own `start_speed +
        // skates*bonus` term with clogs SUBTRACTED (sub_41F29B's per-tick speed
        // expression subtracts the clog count times the clog penalty from the
        // running total, §9.1). Order: skates first (via powerups.apply above),
        // then clogs, matching the original's single combined expression.
        // Default 0 -> no-op, golden hashes unaffected.
        p.clogs = config.born_with_clogs[i];
        p.speed -= p.clogs * s.tuning.clogs_speed_penalty;
    }

    // Campaign rover/ghost hazards (docs/re/campaign.md "sub_40151B — the
    // REAL per-stage starter"): the original seeds ghosts THEN rovers
    // (sub_401B05 before sub_401AAE in sub_40151B's own call order), both
    // AFTER players are placed (the spawn's distance-3-from-every-player
    // gate needs live player positions) and BEFORE the hidden-powerup RNG
    // loop below (sub_40151B runs at stage-start, well before the normal
    // per-round powerup-hide pass). Zero count -> RoverSystem::spawn draws
    // no RNG at all, so every non-campaign MatchConfig (both counts default
    // 0) leaves the RNG stream byte-identical to before this code existed.
    s.campaign_hazards_active = config.campaign_rovers > 0 || config.campaign_ghosts > 0;
    RoverSystem rover_system{s};
    rover_system.spawn(RoverKind::Ghost, config.campaign_ghosts, config.campaign_ghost_speed);
    rover_system.spawn(RoverKind::Rover, config.campaign_rovers, config.campaign_rover_speed);

    // Hide powerups under randomly chosen bricks (seeded RNG — deterministic).
    // Faithful to sub_4258E5's non-network branch (setup.md finding 1;
    // batch_0x42583B.cpp:222-255, pseudo.c 26647-26679): INDEPENDENT REJECTION
    // SAMPLING per unit, NOT list-removal. For each kind k in 0..12 (the loop
    // reads getvalue(k+400) = our spawn_counts[k], scheme-overridable):
    //   - positive count (the original latches a "no gate" flag): place every
    //     unit unconditionally. Negative
    //     count -N: attempt |N| units, each gated by a 1-in-10 roll — the
    //     original ORs that latched flag with a zero-result rand()%10, so the
    //     gate draws ONLY on the negative path,
    //     INTERLEAVED immediately before the unit's own scan — not batched up
    //     front the way the old port pre-rolled all |N| gates).
    //   - each attempted unit draws a fresh random (x, y) — rand()%W then
    //     rand()%H, x FIRST — retrying up to 200 times and taking the first
    //     cell that is a Brick with no powerup record yet (sub_425FB9==2 &&
    //     !sub_42542D). If all 200 tries miss, the unit is SILENTLY DROPPED
    //     (no candidate side-list, no compensating retry) — the original's own
    //     under-placement behaviour on a sparse/crowded board.
    // The draw order/count now mirrors the original draw-for-draw (2 per try,
    // <=200 tries/unit, plus one 1-in-10 gate per negative-N unit), replacing
    // the old single-draw-per-placement list-removal. This shifts the setup RNG
    // stream for every scheme that has bricks AND a nonzero count (golden B/C
    // recaptured 2026-07-20; D/E zero all spawn_counts, so their scatter draws
    // nothing either way and stays byte-identical). Setup-only reproducibility
    // fidelity, not real-game bit-matching — the real game seeds brick fill off
    // the wall clock (docs/valuelst-map.md "brick fill").
    for (int k = 0; k < kPowerupKinds; ++k) {
        // NO forbidden-skip here: sub_4258E5 (batch_0x42583B.cpp) hides a powerup
        // under bricks based ONLY on its count (getvalue(400+k) / the scheme
        // override), with NO check of the per-powerup `forbidden` flag — that
        // flag gates only the RANDOM-powerup re-roll (the 0xC case's s.forbidden
        // == dword_4647E0), never the brick-hide. A former `if (forbidden)
        // continue;` here SKIPPED the RNG draws the native makes for a
        // forbidden-but-nonzero-count kind — an RNG-order divergence. Dormant on
        // every shipped scheme (they zero a forbidden kind's count), so goldens
        // are unmoved, but it would desync a user scheme that forbids a powerup
        // while leaving its count > 0. (W3-C powerup audit.)
        std::int32_t want = config.spawn_override[k] > MatchConfig::kNoOverride
                                ? config.spawn_override[k]
                                : s.tuning.spawn_counts[k];
        bool always = true;  // v22
        if (want < 0) {
            always = false;
            want = -want;
        }
        for (std::int32_t m = 0; m < want; ++m) {
            // v22 || !(rand()%10): the gate draws (and can reject) only on the
            // negative-N path; a positive count short-circuits with no draw.
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

    // Arm the dud gate (the original arms it once at match init, sub_422C7A
    // -> sub_422C13): base + rand(spread) SECONDS ahead — VALUELST 320/321's
    // own legend ("minimum/additional random number of SECONDS between
    // potential dud bombs"), i.e. 3-6 minutes, converted to ticks. facts.md
    // "Dud bombs" (units corrected 2026-07-09).
    s.dud_gate =
        (static_cast<std::uint64_t>(s.tuning.dud_gate_base) +
         random_below(
             s, static_cast<std::uint32_t>(std::max<std::int32_t>(1, s.tuning.dud_gate_rand)))) *
        kTicksPerSecond;
    return s;
}

}  // namespace bomber::sim::detail
