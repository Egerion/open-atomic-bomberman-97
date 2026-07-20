#pragma once

#include "bomber/sim/state.hpp"

namespace bomber::sim {

// The nine skull diseases — assignment, contagion, cure, and expiry.
// Reverse-engineered from sub_41DFB6 / sub_41E21E / sub_41F29B; see
// docs/re/facts.md "Disease system".
class DiseaseSystem {
public:
    explicit DiseaseSystem(State& s) : s_(s) {}

    // Cures a player completely (sub_41DF4C).
    static void clear(Player& p);

    // Applies one specific disease to player `idx`. Swap teleport-swaps with a
    // random other live player and leaves no flag; the rest set their flag and
    // (re)start the shared countdown. `announce` mirrors the original's sound
    // flag — only the first disease of a batch plays a voice line.
    void give(int idx, Disease d, bool announce);

    // The skull powerup: `count` random diseases (rand()%9). A Swap roll with
    // nobody to swap with is lost (no teleport) but STILL announces, matching
    // sub_41DFB6's announce-before-target-scan order (diseases.md finding 1).
    // Skull = 1, purple "super" skull = 3 (sub_41E21E cases 2 / 0xB).
    void assign_random(int idx, int count);

    // The original rolls a cure before applying ANY powerup pickup
    // (sub_41E21E top): 1-in-N chance when curable and currently sick.
    void maybe_cure_on_pickup(Player& p);

    // Tick step: contagion on overlap, then age freshness gates and expire
    // finished diseases.
    void spread_and_age();

private:
    State& s_;
};

}  // namespace bomber::sim
