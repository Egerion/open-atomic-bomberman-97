// Rovers/ghosts fidelity fix — F1: flame contact does not stop the mover's
// per-pixel loop or gate the same-tile landing-kill.
//
// docs/re/audit/tileregen_rovers.md Finding 1: `sub_401B5C` (raw disasm
// 0x401E24-0x401E82, confirmed via `native/tools/disasm.py 0x401B5C
// 0x401F76`) sets the dead flag on flame contact and falls STRAIGHT THROUGH
// into the same-tile landing-tile-kill check, then unconditionally `jmp`s
// (0x401ED3 -> 0x401c0f) back to the pixel-budget loop's own top, consuming
// the rest of the tick's move_budget — which can re-trigger the flame-death
// branch (re-awarding kill-score) on further pixels/tiles crossed later in
// the SAME tick. `RoverSystem::step()` used to `return false` immediately on
// the first flame hit, skipping both effects. See
// libs/sim/src/systems/rovers.cpp's flame-death branch for the fix.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "helpers.hpp"

using namespace bomber::sim;
using namespace bomber::sim::test;

namespace {

Fixed centre_x(int tx) {
    return tx * kTileWF + kTileWF / 2;
}
Fixed centre_y(int ty) {
    return ty * kTileHF + kTileHF / 2;
}

bool any_event(const Simulation& s, Event::Type t) {
    for (const auto& e : s.state().events)
        if (e.type == t) return true;
    return false;
}

int count_events(const Simulation& s, Event::Type t) {
    int n = 0;
    for (const auto& e : s.state().events)
        if (e.type == t) ++n;
    return n;
}

}  // namespace

TEST_CASE(
    "a rover crossing two flame tiles in one tick's budget re-arms the kill-score each hit "
    "(fall-through, not an early return)") {
    // Before the fix, step() returned on the FIRST flame contact, so a rover
    // ever dying to flame produced exactly one RoverDied event, no matter how
    // much move_budget remained. A fast rover given a long, uninterrupted,
    // flame-lit corridor should now produce SEVERAL — the original re-tests
    // flame presence every remaining per-pixel step of this same tick's
    // budget, not once.
    MatchConfig cfg = open_config();
    Simulation s(cfg);
    State& st = s.state();
    st.rovers.clear();
    st.campaign_hazards_active = true;

    for (int x = 5; x <= 12; ++x) st.cells[5][x] = Cell::Blank;  // clear the pillar row

    Rover r;
    r.alive = true;
    r.kind = RoverKind::Rover;
    r.x = centre_x(6);
    r.y = centre_y(5);
    r.dir = 1;         // East, straight down the flame-lit corridor
    r.speed = 20000;   // huge budget: guarantees many per-pixel steps land in
                        // flame this single tick (budget/100 px ~= speed+900)/100
    st.rovers.push_back(r);

    // Two adjacent flame tiles, each with a distinct owner, so a single-hit
    // implementation could only ever report ONE of them.
    st.flame[5][7] = 5;
    st.flame_owner[5][7] = 2;
    st.flame[5][8] = 5;
    st.flame_owner[5][8] = 4;

    s.tick(TickInputs{});

    CHECK(st.rovers.empty());  // died this tick, reaped same tick (next-call semantics)
    // Old (buggy) behaviour: always exactly 1. Fixed behaviour: several, one
    // per remaining per-pixel step that still lands on a lit flame tile.
    CHECK(count_events(s, Event::Type::RoverDied) > 1);
}

TEST_CASE(
    "a rover dying to flame on a player's own tile still kills that player (same-tile "
    "landing-kill, F1)") {
    // Before the fix, the early `return false` on flame contact skipped the
    // landing-tile-kill loop entirely for the tile the mover died on — a
    // human player standing on the exact tile a rover dies to flame on was
    // NOT killed by the rover's own landing-kill path (only, separately, by
    // the ordinary player-flame path, a different system). The original
    // kills via both paths on that tile; this pins the rover-landing-kill
    // half.
    MatchConfig cfg = open_config();
    Simulation s(cfg);
    State& st = s.state();
    st.rovers.clear();
    st.campaign_hazards_active = true;
    st.cells[5][7] = Cell::Blank;  // (7,5) is an odd,odd pillar in open_config(); clear it

    Player& victim = st.players[1];
    victim.present = true;
    victim.alive = true;
    victim.ai = false;
    victim.x = centre_x(7);
    victim.y = centre_y(5);

    Rover r;
    r.alive = true;
    r.kind = RoverKind::Rover;
    r.x = centre_x(6);
    r.y = centre_y(5);
    r.dir = 1;  // East, straight at the flame+victim tile
    r.speed = 2000;
    st.rovers.push_back(r);

    st.flame[5][7] = 5;
    st.flame_owner[5][7] = 3;

    bool saw_rover_died = false, saw_kill_event = false, saw_death_event = false;
    for (int i = 0; i < 5 && !st.rovers.empty(); ++i) {
        s.tick(TickInputs{});
        saw_rover_died = saw_rover_died || any_event(s, Event::Type::RoverDied);
        saw_kill_event = saw_kill_event || any_event(s, Event::Type::RoverKilledPlayer);
        saw_death_event = saw_death_event || any_event(s, Event::Type::PlayerDied);
    }

    CHECK(saw_rover_died);
    // FIX: previously the victim survived because step() returned before
    // ever reaching the landing-tile-kill loop on the death tile.
    CHECK_FALSE(victim.alive);
    CHECK(saw_kill_event);
    CHECK(saw_death_event);
}
