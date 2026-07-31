#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "bomber/sim/bomb.hpp"
#include "bomber/sim/brain.hpp"
#include "bomber/sim/constants.hpp"
#include "bomber/sim/event.hpp"
#include "bomber/sim/player.hpp"
#include "bomber/sim/rover.hpp"
#include "bomber/sim/tuning.hpp"
#include "bomber/sim/types.hpp"

namespace bomber::sim {

// The complete deterministic gameplay state (ADR-0003). Value semantics: copying
// a State is a legal snapshot, state_hash() digests every gameplay field, and
// identical (State, inputs) sequences replay identically.
//
// Everything below is hashed EXCEPT the two derived per-tick outputs at the
// bottom (`events`, `sub_trace`) and the static per-match config `tuning` /
// `forbidden` — determinism contract rule 4.
struct State {
    std::uint64_t tick = 0;
    std::uint32_t rng = 0x12345678;
    std::int32_t ticks_left = 0;  // match countdown; 0 = time up (draw)
    // Round-start input freeze, in ticks (dword_4621E0): while nonzero,
    // sub_41F29B's acquisition gate skips BOTH the AI brain and the human input
    // read, so nobody moves or acts during the opening colour-shuffle second.
    // docs/re/player-turn.md §7.
    std::int32_t input_freeze = 0;
    bool hurry = false;  // walls are closing in
    std::int32_t enclose_index = 0;
    std::int32_t enclose_timer = 0;
    std::int32_t enclose_interval = 0;
    // Per-level tile regeneration countdown, ticks (facts.md "Per-level tile
    // regeneration", sub_426704's dword_464978): counts to 0, TileRegenSystem
    // makes ONE attempt, then it resets to regen_seconds x kTicksPerSecond. Only
    // level index 7 ("haunted house") has a nonzero cadence; everywhere else
    // TileRegenSystem never moves this field and draws no RNG.
    std::int32_t regen_timer = 0;
    // Next tick a dud roll may fire (the global rate limiter dword_464AF4, armed
    // at setup and re-armed on every open-gate placement).
    std::uint64_t dud_gate = 0;
    // Stable per-bomb identity (facts.md "Chain-reaction timing"): sub_423209's
    // pending queue must still find a queued bomb one tick later, but `bombs`
    // compacts dead entries EVERY tick, which would invalidate a raw index held
    // across that boundary. Assigned once at creation, never reused; 0 is invalid.
    std::uint32_t next_bomb_id = 1;
    Tuning tuning;
    // Per-scheme forbidden powerups (-P rows). Static per-match config like
    // tuning, so excluded from state_hash(). Consulted only by the Random
    // powerup's reroll (sub_41E21E case 0xC).
    std::array<bool, kPowerupKinds> forbidden{};

    std::array<std::array<Cell, kGridWidth>, kGridHeight> cells{};
    // Stage "extra" actors (EXTRA<N>.RES, docs/re/stage-actors.md): a static
    // per-match layer like cells — parsed at setup, never mutated by the sim —
    // but gameplay-affecting, so hashed. actor_dir is a godir and is meaningful
    // only where actor_type is Conveyor or DirArrow.
    std::array<std::array<ActorType, kGridWidth>, kGridHeight> actor_type{};
    std::array<std::array<std::uint8_t, kGridWidth>, kGridHeight> actor_dir{};
    // The warphole partner resolved by sub_405A81's idno/linkto scan, pre-computed
    // at setup so the sim draws no RNG for a warp (§5). Meaningless where
    // actor_type != Warphole.
    std::array<std::array<std::uint8_t, kGridWidth>, kGridHeight> warp_dest_x{};
    std::array<std::array<std::uint8_t, kGridWidth>, kGridHeight> warp_dest_y{};
    std::array<std::array<PowerupType, kGridWidth>, kGridHeight> hidden{};  // under a brick
    std::array<std::array<PowerupType, kGridWidth>, kGridHeight> floor{};   // revealed
    std::array<std::array<std::uint8_t, kGridWidth>, kGridHeight> flame{};  // ticks left, 0 = none
    // The KILL-CREDIT owner of the flame (record word +62), which a chain hit
    // rewrites to the chainer before the chained bomb explodes.
    std::array<std::array<std::uint8_t, kGridWidth>, kGridHeight> flame_owner{};
    // The colour the flame is DRAWN in: the igniting bomb's creation-time colour
    // (record byte +60), which unlike flame_owner never transfers on a chain — a
    // chained bomb's flames keep the original placer's colour (facts.md
    // "Bomb/flame colour is not the owner").
    std::array<std::array<std::uint8_t, kGridWidth>, kGridHeight> flame_colour{};
    // Which arm PIECE this cell shows, decided ONCE at ignition from the arm's
    // OWN geometry rather than re-derived from which neighbours happen to be lit
    // later, which was the presentation layer's earlier approximation (facts.md
    // "Flame arm-shape selection"; FlameKind's own comment in types.hpp carries
    // the sub_42331C/off_45BEA0 citation). A brick-burn cell (kind 9 in the
    // original) has no value here — that lifetime lives entirely in `burning`.
    std::array<std::array<FlameKind, kGridWidth>, kGridHeight> flame_kind{};
    // Ticks of a brick still crumbling; it blocks until this reaches 0.
    std::array<std::array<std::uint8_t, kGridWidth>, kGridHeight> burning{};

    // Bombs marked for forced detonation (sub_423209's 100-slot queue
    // dword_4621F8/FC), drained once per tick right after players act and before
    // bombs move. That placement is what makes a manual trigger press instant
    // while a chain reaction advances one LINK per tick: a press queued during
    // the player pass is caught by that same tick's drain, whereas a
    // flame-arm/slide/landing hit is queued during bomb processing, after the
    // drain already ran, and waits for the next tick's. skip_dir mirrors bomb+56
    // — an arm-triggered chain skips re-blasting back toward the flame that
    // triggered it (pseudo.c 25621/25645). facts.md "Chain-reaction timing".
    struct PendingChain {
        std::uint32_t bomb_id = 0;
        std::int8_t skip_dir = -1;
    };
    std::vector<PendingChain> pending_chain;

    std::array<Player, kMaxPlayers> players{};
    // Per-player computer-AI brains (ADR-0005 §3), indexed in lockstep with
    // `players` and filled by AISystem::decide before movement.
    std::array<Brain, kMaxPlayers> brains{};
    std::vector<Bomb> bombs;
    // Campaign-mode autonomous hazards (docs/re/campaign.md).
    std::vector<Rover> rovers;
    // True for the lifetime of a campaign match that spawned at least one hazard;
    // never cleared mid-match. It distinguishes "never had campaign hazards", where
    // RoverSystem::tick must stay a true no-op, from "had them, all now dead",
    // where the grace timer below must keep accumulating even though `rovers` is
    // empty — mirroring the original's dword_46489C.
    bool campaign_hazards_active = false;
    // The "all hazards dead" grace timer (campaign.md "Round pacing" clause 3,
    // dword_4646C0), in ticks: reset to 0 while any hazard lives, else
    // incremented until it reaches kHazardClearTicks.
    std::int32_t hazard_clear_timer = 0;

    // Cleared at the start of every tick; excluded from state_hash().
    std::vector<Event> events;

    // Per-sub-frame presentation trace: player i's position and facing at the END
    // of canonical sub-frame f of THIS tick. The original renders every DISPLAYED
    // frame at the player's live per-frame position, so its ~180 fps micro-zigzag
    // is visible on screen; a renderer that lerps only the 20 Hz tick endpoints
    // filters all of that out mathematically, since every direction change with a
    // period under 100 ms cancels. This lets the presentation play the tick's real
    // intra-tick motion back instead. A derived per-tick output EXACTLY like
    // `events`: rebuilt every tick, never hashed, never read back by the sim.
    struct SubSample {
        Fixed x = 0, y = 0;
        Direction facing = Direction::Down;
    };
    std::array<std::array<SubSample, kSubFrames>, kMaxPlayers> sub_trace{};
};

}  // namespace bomber::sim
