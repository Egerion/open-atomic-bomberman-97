// Focused checks for the trampoline stage actor (type 3). Faithful to the
// step-on branch in sub_41EC84 (+ the state-5 guard in sub_41DE63): landing on
// a trampoline centre launches an in-place hop that ignores input and cannot be
// pushed until it ends. See docs/re/stage-actors.md §4.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "helpers.hpp"

using namespace bomber::sim;
using namespace bomber::sim::test;

namespace {

bool saw_bounce(const Simulation& s, int player) {
    for (const auto& e : s.state().events)
        if (e.type == Event::Type::TrampolineBounce && e.player == player) return true;
    return false;
}

}  // namespace

TEST_CASE("stepping onto a trampoline centre launches a bounce") {
    Simulation s(open_config());
    State& st = s.state();
    st.actor_type[0][0] = ActorType::Trampoline;  // (0,0)
    Player& p = st.players[0];
    p.x = kTileWF / 2;  // already centred on the trampoline
    p.y = kTileHF / 2;

    s.tick(TickInputs{});  // settle on the tile

    CHECK(p.bounce == st.tuning.trampoline_bounce_frames);
    CHECK(saw_bounce(s, 0));
}

TEST_CASE("a bouncing player ignores movement input") {
    Simulation s(open_config());
    State& st = s.state();
    st.actor_type[0][0] = ActorType::Trampoline;
    Player& p = st.players[0];
    p.x = kTileWF / 2;
    p.y = kTileHF / 2;

    s.tick(TickInputs{});               // launch
    REQUIRE(p.bounce > 0);
    const int bx = p.x, by = p.y;

    TickInputs east;
    east.players[0].right = true;
    run(s, 3, east);                    // try to walk away mid-bounce

    CHECK(p.x == bx);                   // pinned in place during the hop
    CHECK(p.y == by);
    CHECK(p.tile_x() == 0);
}

TEST_CASE("the bounce ends after its duration and movement resumes") {
    Simulation s(open_config());
    State& st = s.state();
    st.actor_type[0][0] = ActorType::Trampoline;
    st.tuning.trampoline_bounce_frames = 5;  // shorten for the test
    Player& p = st.players[0];
    p.x = kTileWF / 2;
    p.y = kTileHF / 2;

    s.tick(TickInputs{});                // launch (bounce = 5)
    run(s, 5, TickInputs{});             // tick the hop down to 0

    CHECK(p.bounce == 0);

    // Re-launch is possible once the hop is over and we are still centred; move
    // off first to prove walking resumed, then it can re-trigger.
    const int x0 = p.x;
    TickInputs east;
    east.players[0].right = true;
    s.tick(east);
    CHECK(p.x > x0);                     // moved: no longer state-gated
}

TEST_CASE("a trampoline does not fire until the player is centred") {
    Simulation s(open_config());
    State& st = s.state();
    st.actor_type[0][0] = ActorType::Trampoline;
    Player& p = st.players[0];
    // Off-centre on the trampoline tile: gliding across, not settled.
    p.x = kTileWF / 2 - 700;  // 7px left of centre, still tile (0,0)
    p.y = kTileHF / 2;

    s.tick(TickInputs{});

    CHECK(p.bounce == 0);       // not launched yet
    CHECK_FALSE(saw_bounce(s, 0));
}
