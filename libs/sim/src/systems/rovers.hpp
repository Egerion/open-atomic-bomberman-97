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

    // Spawns `count` actors at random tiles MORE than 3 tiles (Manhattan) from
    // every player — a candidate at exactly 3 is rejected. Called once per
    // campaign stage from libs/game, never mid-round, and draws ZERO RNG when
    // count <= 0, which is every non-campaign match.
    void spawn(RoverKind kind, int count, std::int32_t speed);

    // Tick step: drive every live actor's mover one tick, update the "all hazards
    // dead" grace timer, reap dead entries. Gated on
    // State::campaign_hazards_active and NOT on s.rovers.empty(), because a
    // campaign match's grace timer must keep counting after the last hazard dies
    // and the vector empties.
    void tick();

    // True on the exact tick every actor has been dead for kHazardClearTicks
    // (campaign.md "Round pacing" clause 3). The campaign layer — not this
    // system, which has no concept of a "stage" — uses it to flag "stage clear"
    // exactly once, mirroring dword_464894 = 1's edge.
    bool hazards_just_cleared() const { return hazards_just_cleared_; }

private:
    // One 200-attempt placement search (sub_4019C2), 2 draws per attempt.
    void place_one(RoverKind kind, std::int32_t speed);

    // One live actor's mover, one tick (sub_401B5C). `rover_index` is its slot in
    // State::rovers, for event attribution. False if it died this tick.
    bool step(Rover& r, int rover_index);

    // The turn taken when a step lands exactly on a tile centre. Draws RNG — see
    // the definition for the per-branch draw counts.
    void turn_at_centre(Rover& r, int cand_tx, int cand_ty);

    // sub_421CB5 + sub_41DE63: kill every eligible player on the tile just entered.
    void kill_players_on(int tx, int ty, int rover_index);

    // sub_4017FA, TYPE-DEPENDENT (campaign.md mover clause 1): a ghost passes
    // through bricks and is blocked only by solid; a rover is blocked by both.
    bool passable(RoverKind kind, int tx, int ty) const;

    State& s_;
    bool hazards_just_cleared_ = false;
};

}  // namespace bomber::sim
