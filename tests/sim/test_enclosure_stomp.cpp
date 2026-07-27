// Enclosure fidelity fix — F1: "Stomped Bombs Detonate" (wall_detonates) ON
// crushes only the FIRST grounded bomb found per drop event, not every bomb
// sharing the crushed tile.
//
// docs/re/audit/enclosure.md Finding 1: `sub_426818`'s bomb-crush loop
// (native/src/game/batch_0x42583B.cpp lines 769-820) queues the found bomb
// by calling sub_423209 on it with -1 as the second argument (pseudo.c 27262)
// and then jumping unconditionally to LABEL_47 (pseudo.c 27263) — there is no
// path back to the top of that search loop on the ON branch, so it runs at
// most once per drop
// event. `EnclosureSystem::drop_wall`'s ON branch used to have no `break`,
// so it queued EVERY matching bomb on the tile. Two grounded (non-flying)
// bombs sharing one tile is not reachable via this port's own placement/kick
// collision rules (docs/re/audit/enclosure.md Finding 1's own
// Medium-confidence-on-reachability note) — constructed here white-box,
// directly via State::bombs, to pin the control-flow fix regardless.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "helpers.hpp"

using namespace bomber::sim;
using namespace bomber::sim::test;

namespace {

// Arms the walls and lands the FIRST drop event exactly on tile (0,0),
// mirroring test_sim.cpp's "hurry walls spiral in" fixture: game_seconds=10
// (ticks_left starts at 200) + hurry_seconds=8 arms the walls at tick 121
// (remaining seconds <= 3, non-strict) and drops the first event at arm+5 =
// tick 126 (spiral().events[0] is always (0,0), the top-left corner).
MatchConfig stomp_config() {
    MatchConfig cfg = open_config();
    cfg.tuning.game_seconds = 10;
    cfg.tuning.hurry_seconds = 8;
    cfg.tuning.enclosement_depth = 1;
    return cfg;
}

Bomb grounded_bomb(int owner, int tx, int ty, std::int32_t fuse) {
    Bomb b;
    b.active = true;
    b.owner = static_cast<std::uint8_t>(owner);
    b.x = tx * kTileWF + kTileWF / 2;
    b.y = ty * kTileHF + kTileHF / 2;
    b.fuse = fuse;
    b.flame = 1;
    return b;
}

}  // namespace

TEST_CASE("wall_detonates ON queues only the FIRST of two grounded bombs sharing a tile") {
    MatchConfig cfg = stomp_config();
    cfg.tuning.wall_detonates = 1;
    Simulation s(cfg);
    State& st = s.state();

    st.bombs.push_back(grounded_bomb(0, 0, 0, 30000));
    st.bombs.push_back(grounded_bomb(1, 0, 0, 30000));
    st.players[0].bombs_placed = 1;
    st.players[1].bombs_placed = 1;

    run(s, 126);  // arm (tick 121) + one interval (5 ticks): first event lands on (0,0)
    REQUIRE(st.cells[0][0] == Cell::Solid);
    // sub_423209 only QUEUES a detonation (one-tick defer, docs/re/facts.md
    // "Chain-reaction timing") -- nothing is removed synchronously by the
    // drop itself, ON or OFF. (Whether the queued bomb actually detonates on
    // the FOLLOWING tick is bombs.cpp's fuse-drain, a separate system this
    // test deliberately does not exercise -- scoping the assertion to
    // exactly what drop_wall's bomb-crush loop itself does keeps this test
    // independent of that other system's own behaviour/tests.)
    REQUIRE(st.bombs.size() == 2);
    // Only the FIRST match (iteration order == insertion order here) gets
    // its fuse force-fired to 1; the second is left completely untouched by
    // this drop event -- its fuse only reflects 126 ticks of the bomb's OWN
    // ordinary countdown from 30000, nowhere near the forced value the first
    // bomb got.
    CHECK(st.bombs[0].fuse == 1);
    CHECK(st.bombs[1].fuse > 1000);
}

TEST_CASE("wall_detonates OFF still eats every bomb on the crushed tile (unchanged by this fix)") {
    // The OFF branch's loop-to-exhaustion (sub_424841's fall-through
    // re-search, with no jump out to LABEL_47) is untouched by this fix -- pinned here
    // alongside the ON case so a future edit can't silently break the
    // documented ON/OFF asymmetry.
    MatchConfig cfg = stomp_config();
    cfg.tuning.wall_detonates = 0;
    Simulation s(cfg);
    State& st = s.state();

    st.bombs.push_back(grounded_bomb(0, 0, 0, 30000));
    st.bombs.push_back(grounded_bomb(1, 0, 0, 30000));
    st.players[0].bombs_placed = 1;
    st.players[1].bombs_placed = 1;

    run(s, 126);
    REQUIRE(st.cells[0][0] == Cell::Solid);
    CHECK(st.bombs.empty());  // both silently eaten, no explosion
    CHECK(st.players[0].bombs_placed == 0);
    CHECK(st.players[1].bombs_placed == 0);
}
