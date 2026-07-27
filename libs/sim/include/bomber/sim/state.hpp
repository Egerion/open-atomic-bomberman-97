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

// The complete deterministic gameplay state (ADR-0003). Value semantics:
// copying a State is a legal snapshot, state_hash() digests every gameplay
// field, and identical (State, inputs) sequences replay identically.
struct State {
    std::uint64_t tick = 0;
    std::uint32_t rng = 0x12345678;
    std::int32_t ticks_left = 0;  // match countdown; 0 = time up (draw)
    // Round-start input freeze, in ticks (dword_4621E0: armed to 50ms ×
    // getvalue(30) ≈ 1 s by round init sub_4214BC, counted down at the top
    // of every player pass, and while nonzero sub_41F29B's acquisition gate —
    // which needs its new-input flag set AND dword_4621E0 zero — skips BOTH the
    // AI brain and the human input read —
    // nobody moves or acts during the opening colour-shuffle second). Armed
    // by build_state from Tuning::input_freeze_ticks; 0 on raw test states.
    // docs/re/facts.md "Round-start input freeze".
    std::int32_t input_freeze = 0;
    bool hurry = false;           // walls are closing in
    std::int32_t enclose_index = 0;
    std::int32_t enclose_timer = 0;
    std::int32_t enclose_interval = 0;
    // Per-level tile regeneration countdown, ticks (docs/re/facts.md "Per-
    // level tile regeneration", sub_426704's dword_464978). Counts down to 0,
    // then TileRegenSystem makes ONE regen attempt and resets it to the
    // current level's regen_seconds*kTicksPerSecond. Only non-zero cadence on
    // level index 7 ("haunted house"); TileRegenSystem is a no-op (this field
    // never moves, draws no RNG) whenever tuning.regen_seconds[level] <= 0 —
    // every other level/scenario, so this is a fixed mix(0) there.
    std::int32_t regen_timer = 0;
    // Next tick a dud roll may fire (global rate limiter, dword_464AF4 in
    // the original — armed at setup, re-armed on every open-gate placement).
    std::uint64_t dud_gate = 0;
    // Stable per-bomb identity (docs/re/facts.md "Chain-reaction timing"):
    // sub_423209's pending-detonation queue must still find a queued bomb one
    // tick later, but `bombs` (below) compacts dead entries EVERY tick
    // (step 7), which would invalidate a raw index held across that
    // boundary. Assigned once at creation (BombSystem::place/throw_carried),
    // never reused; 0 is not a valid id.
    std::uint32_t next_bomb_id = 1;
    Tuning tuning;
    // Per-scheme forbidden powerups (-P rows). Static per-match config like
    // tuning — excluded from state_hash(). The Random powerup consults it
    // when rerolling (sub_41E21E case 0xC).
    std::array<bool, kPowerupKinds> forbidden{};

    std::array<std::array<Cell, kGridWidth>, kGridHeight> cells{};
    // Stage "extra" actors (EXTRA<N>.RES → docs/re/stage-actors.md). A static
    // per-match layer like cells: parsed at setup, never mutated by the sim,
    // but gameplay-affecting (conveyors push, trampolines bounce) so it IS
    // mixed into state_hash(). actor_dir is a godir (0=Up,1=Right,2=Down,
    // 3=Left) and is only meaningful where actor_type is Conveyor/DirArrow.
    std::array<std::array<ActorType, kGridWidth>, kGridHeight> actor_type{};
    std::array<std::array<std::uint8_t, kGridWidth>, kGridHeight> actor_dir{};
    // Warphole exit tile, one entry per Warphole cell (docs/re/stage-actors.md
    // §5): the partner resolved by sub_405A81's idno/linkto scan, pre-computed
    // at setup so the sim draws no RNG for a warp. A static, hashed per-match
    // input like actor_type; meaningless (0) where actor_type != Warphole.
    std::array<std::array<std::uint8_t, kGridWidth>, kGridHeight> warp_dest_x{};
    std::array<std::array<std::uint8_t, kGridWidth>, kGridHeight> warp_dest_y{};
    // Powerup hidden under a brick (revealed when the brick burns away).
    std::array<std::array<PowerupType, kGridWidth>, kGridHeight> hidden{};
    // Powerup lying revealed on the floor.
    std::array<std::array<PowerupType, kGridWidth>, kGridHeight> floor{};
    // Remaining ticks of flame in a cell (0 = none).
    std::array<std::array<std::uint8_t, kGridWidth>, kGridHeight> flame{};
    // Which player's bomb produced the flame (valid while flame > 0) — the
    // KILL-CREDIT owner (flame record word +62), which a chain hit rewrites
    // to the chainer before the chained bomb explodes.
    std::array<std::array<std::uint8_t, kGridWidth>, kGridHeight> flame_owner{};
    // Which player's COLOUR the flame is drawn in (valid while flame > 0) —
    // the igniting bomb's creation-time colour (Bomb::colour, the original's
    // flame record byte +60), which unlike flame_owner never transfers on a
    // chain: a chained bomb's flames keep the original placer's colour.
    // docs/re/facts.md "Bomb/flame colour is not the owner".
    std::array<std::array<std::uint8_t, kGridWidth>, kGridHeight> flame_colour{};
    // Which flame-arm PIECE this cell shows (valid while flame > 0), decided
    // ONCE at ignition (FlameKind — see its own doc comment in types.hpp for
    // the full sub_42331C/off_45BEA0 citation). The epicentre is always
    // Center; an extending arm's tile is a TIP of its own cast direction only
    // if it is the LAST tile of the bomb's FULL configured reach, else a MID
    // of that same direction — fixed at cast time from the arm's OWN
    // geometry, not re-derived from which neighbours happen to be lit later
    // (that was the presentation layer's previous approximation; see
    // docs/re/facts.md "Flame arm-shape selection"). A brick-burn cell (kind
    // 9 in the original) has no corresponding value here — that lifetime
    // lives entirely in `burning`.
    std::array<std::array<FlameKind, kGridWidth>, kGridHeight> flame_kind{};
    // Remaining ticks of a brick crumbling (blocks until it reaches 0).
    std::array<std::array<std::uint8_t, kGridWidth>, kGridHeight> burning{};

    // A bomb a flame arm, a landing flying bomb, a sliding bomb entering
    // flame, or a trigger-button press marked for forced detonation
    // (sub_423209's queue: dword_4621F8/FC, 100 slots). Drained once per
    // tick, right after players act and before bombs move
    // (FlameSystem::drain_chain_queue) — mirroring the once-per-tick
    // drain guard at the top of sub_42331C (which fires only while the queue
    // stamp dword_462210 still differs from the tick counter dword_464994),
    // which likewise follows the whole player pass. A trigger-button press
    // (queued DURING the player pass) is therefore caught by THAT SAME
    // tick's drain; a flame-arm/slide/landing hit (queued DURING bomb
    // processing, after the drain already ran) is NOT — it waits for the
    // NEXT tick's drain. So a chain reaction is genuinely one tick per link,
    // while a manual trigger detonation is instant. skip_dir (-1 = none,
    // else a godir 0-3) mirrors bomb+56: an arm-triggered chain skips
    // re-blasting back toward the flame that triggered it (pseudo.c
    // 25621/25645). docs/re/facts.md "Chain-reaction timing".
    struct PendingChain {
        std::uint32_t bomb_id = 0;
        std::int8_t skip_dir = -1;
    };
    std::vector<PendingChain> pending_chain;

    std::array<Player, kMaxPlayers> players{};
    // Per-player computer-AI brains (ADR-0005 §3), one slot per player, indexed
    // in lockstep with `players`. All zero on a non-AI/absent player, so hashing
    // them is a no-op for non-AI scenarios (golden unchanged apart from the
    // one-time hash-layout growth). Filled by AISystem::decide before movement.
    std::array<Brain, kMaxPlayers> brains{};
    std::vector<Bomb> bombs;
    // Campaign-mode autonomous hazard actors (docs/re/campaign.md "Rover/
    // ghost/AI roster", "Per-tick mover"). Empty on every non-campaign match
    // (MatchConfig::rovers/ghosts default to 0), so this vector stays empty
    // and RoverSystem::tick draws zero RNG for every existing scenario — a
    // ONE-TIME hash-layout growth (CLAUDE.md determinism contract rule 5),
    // not a behaviour change, on every scenario with no rovers/ghosts.
    std::vector<Rover> rovers;
    // True for the lifetime of a campaign match that spawned at least one
    // rover/ghost (set once by build_state when MatchConfig::campaign_rovers/
    // campaign_ghosts > 0; never cleared mid-match). Distinguishes "never had
    // campaign hazards" (RoverSystem::tick must stay a true no-op) from "had
    // them, all now dead" (the grace timer below must still accumulate even
    // though `rovers` is empty) — mirrors the original's dword_46489C
    // campaign-active flag gating sub_4016DA's own logic. False (0) for
    // every existing scenario, so this is mix(0) in the golden hash.
    bool campaign_hazards_active = false;
    // Campaign-only "all hazards dead" grace timer (docs/re/campaign.md
    // "Round pacing" clause 3, `dword_4646C0`). Ticks (not wall-clock ms —
    // see RoverSystem::tick's note on the ms->tick simplification), reset to
    // 0 while any rover/ghost is alive, else incremented until it reaches
    // kHazardClearTicks. Always 0 while campaign_hazards_active is false, so
    // this field is mix(0) for every existing golden scenario.
    std::int32_t hazard_clear_timer = 0;

    // Cleared at the start of every tick; excluded from state_hash().
    std::vector<Event> events;

    // Per-sub-frame presentation trace: player i's position + facing at the
    // END of canonical sub-frame f of THIS tick (constants.hpp kSubFrames).
    // The original renders every DISPLAYED frame at the player's live
    // per-frame position, so its ~180 fps micro-zigzag (AI re-decides and
    // the mover steps once per frame) is visible on screen; a renderer that
    // lerps only the 20 Hz tick endpoints filters all of that out
    // mathematically (every direction change with period < 100 ms cancels).
    // This trace lets the presentation play the tick's real intra-tick
    // motion back instead. Derived per-tick output EXACTLY like `events`:
    // rebuilt every tick, never hashed, never read back by the sim
    // (CLAUDE.md determinism contract rule 4). docs/re/facts.md "Canonical
    // frame cadence" (sub-frame trace addendum).
    struct SubSample {
        Fixed x = 0, y = 0;
        Direction facing = Direction::Down;
    };
    std::array<std::array<SubSample, kSubFrames>, kMaxPlayers> sub_trace{};
};

}  // namespace bomber::sim
