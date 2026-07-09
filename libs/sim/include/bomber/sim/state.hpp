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
    // Which player's bomb produced the flame (valid while flame > 0).
    std::array<std::array<std::uint8_t, kGridWidth>, kGridHeight> flame_owner{};
    // Remaining ticks of a brick crumbling (blocks until it reaches 0).
    std::array<std::array<std::uint8_t, kGridWidth>, kGridHeight> burning{};

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
};

}  // namespace bomber::sim
