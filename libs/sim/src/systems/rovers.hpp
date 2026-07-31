#pragma once

#include "bomber/sim/state.hpp"

// Campaign-mode rover/ghost autonomous hazard actors: spawn, per-tick wander
// mover, flame death, and the landing-tile player kill. A faithful port of
// sub_401AAE/sub_401B05 (spawn), sub_401F76 (drive loop), sub_401B5C (mover).
// See docs/re/campaign.md "Rover/ghost/AI roster" and "Per-tick mover".

namespace bomber::sim {

class RoverSystem {
public:
    explicit RoverSystem(State& s) : s_(s) {}

    // Spawns `count` rovers/ghosts at random walkable tiles MORE than 3 tiles
    // (Manhattan) from every player — a candidate at exactly 3 is rejected —
    // speed `speed` (the .CAM rover_speed/
    // ghost_speed field). Mirrors sub_401AAE/sub_401B05 -> sub_4019C2: up to
    // 200 placement attempts PER actor (2 RNG draws per attempt), silently
    // spawning fewer than `count` if the board has no room (the original
    // doesn't check sub_4019C2's return either). Called once per campaign
    // stage from the campaign layer (libs/game), never mid-round. No-op
    // (zero RNG draws) when count <= 0, so a non-campaign MatchConfig (the
    // default: campaign_rovers/campaign_ghosts both 0) never calls this at
    // all — see setup.cpp.
    void spawn(RoverKind kind, int count, std::int32_t speed);

    // Tick step: drive every live rover/ghost's mover one tick (sub_401F76 ->
    // sub_401B5C), then update the campaign "all hazards dead" grace timer
    // (Round pacing clause 3) and reap dead entries. See simulation.cpp for
    // the exact placement in the tick order and why.
    //
    // No RNG, no player kills, no events, no grace-timer accumulation for
    // every non-campaign scenario — gated on State::campaign_hazards_active
    // (set only by build_state, only when MatchConfig::campaign_rovers/
    // campaign_ghosts > 0), NOT on `s.rovers.empty()`, because a campaign
    // match's grace timer must keep counting even after the last hazard
    // dies and the vector empties (see the .cpp). The flag is tested before
    // anything observable happens — only the unhashed `hazards_just_cleared_`
    // scratch flag is reset above it — so this is provably a no-op for the
    // entire existing golden suite (every scenario leaves it false).
    void tick();

    // True on the exact tick every rover/ghost has been dead for
    // kHazardClearTicks ticks (docs/re/campaign.md "Round pacing" clause 3).
    // The campaign layer (not this system — libs/sim has no concept of
    // "campaign stage") uses this to flag "stage clear, pending" exactly
    // once per clear, mirroring dword_464894 = 1's edge.
    bool hazards_just_cleared() const { return hazards_just_cleared_; }

private:
    // One live rover/ghost's mover, one tick (sub_401B5C). rover_index is
    // its slot in State::rovers (for event attribution). Returns false if
    // the actor died this tick (flame) so tick() can reap it.
    bool step(Rover& r, int rover_index);

    // sub_4017FA: walkability test for a rover/ghost's tile, TYPE-DEPENDENT
    // (docs/re/campaign.md mover clause 1) — a ghost passes through bricks
    // (blocked only by solid, collision code 1); a rover is blocked by
    // solid AND brick (collision code != 0), same as grid::tile_open. Both
    // are always blocked by a grounded bomb.
    bool passable(RoverKind kind, int tx, int ty) const;

    State& s_;
    bool hazards_just_cleared_ = false;
};

}  // namespace bomber::sim
