// Focused checks for the conveyor stage actor (type 2). Faithful to the
// actor branches in sub_41F29B: a belt pushes a standing player along its
// direction and speeds/slows a walking one. See docs/re/stage-actors.md §3.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "helpers.hpp"

using namespace bomber::sim;
using namespace bomber::sim::test;

namespace {

// godir: 0=Up, 1=Right, 2=Down, 3=Left (matches actor_dir encoding).
constexpr std::uint8_t kEast = 1;
constexpr std::uint8_t kWest = 3;

// Lay an East-pointing conveyor along the whole of row 0.
void belt_row0_east(State& st) {
    for (int x = 0; x < kGridWidth; ++x) {
        st.actor_type[0][x] = ActorType::Conveyor;
        st.actor_dir[0][x] = kEast;
    }
}

}  // namespace

TEST_CASE("a standing player on a conveyor is pushed along the belt") {
    Simulation s(open_config());
    State& st = s.state();
    belt_row0_east(st);
    Player& p = st.players[0];
    p.x = kTileWF / 2;  // (0,0) dead centre
    p.y = kTileHF / 2;
    p.facing = Direction::Down;  // something other than the belt direction

    const int x0 = p.x;
    run(s, 10, TickInputs{});  // NO input: only the belt acts

    const int moved = (p.x - x0) / 100;
    // The belt adds conveyor_speed (250) to the move budget each tick, spent
    // 100/px — so ~250/100 = 2.5 px/tick, accumulating the remainder.
    long budget = 0, expected = 0;
    for (int t = 0; t < 10; ++t) {
        budget += st.tuning.conveyor_speed();
        while (budget > 0) { budget -= 100; ++expected; }
    }
    CHECK(moved == static_cast<int>(expected));
    CHECK(p.tile_y() == 0);                 // stayed on the belt lane
    CHECK(p.facing == Direction::Right);    // the belt forced its facing
    CHECK(st.tuning.conveyor_speed() == 250);
}

TEST_CASE("a conveyor push stalls against a wall and leaves facing untouched") {
    // Put an East belt on the last free column before a pillar so the push is
    // blocked; the original reverts the forced godir (no facing change).
    Simulation s(open_config());
    State& st = s.state();
    // Row 1 has pillars at odd x; place an East belt at (0,1) so the push aims
    // straight into the (1,1) pillar.
    st.actor_type[1][0] = ActorType::Conveyor;
    st.actor_dir[1][0] = kEast;
    Player& p = st.players[0];
    p.x = kTileWF / 2;
    p.y = kTileHF + kTileHF / 2;  // (0,1) dead centre, pillar at (1,1)
    p.facing = Direction::Up;

    run(s, 6, TickInputs{});

    CHECK(p.tile_x() == 0);
    CHECK(p.x == kTileWF / 2);            // clamped on the centre, not pushed through
    CHECK(p.facing == Direction::Up);    // forced facing reverted (blocked)
}

TEST_CASE("walking with the belt is faster than walking against it") {
    // Same input duration, opposite belts: with-belt covers more ground.
    auto walk_east_on = [](std::uint8_t belt_dir) {
        Simulation s(open_config());
        State& st = s.state();
        for (int x = 0; x < kGridWidth; ++x) {
            st.actor_type[0][x] = ActorType::Conveyor;
            st.actor_dir[0][x] = belt_dir;
        }
        Player& p = st.players[0];
        p.x = kTileWF / 2;
        p.y = kTileHF / 2;
        TickInputs east;
        east.players[0].right = true;
        const int x0 = p.x;
        run(s, 6, east);
        return (p.x - x0);
    };

    const int with_belt = walk_east_on(kEast);   // belt bonus
    const int against_belt = walk_east_on(kWest); // belt penalty

    // Plain walk (no belt) for reference.
    Simulation ref(open_config());
    {
        Player& p = ref.state().players[0];
        p.x = kTileWF / 2;
        p.y = kTileHF / 2;
    }
    TickInputs east;
    east.players[0].right = true;
    const int rx0 = ref.state().players[0].x;
    run(ref, 6, east);
    const int plain = ref.state().players[0].x - rx0;

    CHECK(with_belt > plain);
    CHECK(against_belt < plain);
    CHECK(with_belt > against_belt);
}

TEST_CASE("the actor layout is part of the hashed state") {
    // Two otherwise-identical matches must hash differently once one has a belt,
    // proving the layout is mixed into state_hash() (determinism contract).
    Simulation plain(open_config());
    Simulation belted(open_config());
    belt_row0_east(belted.state());
    CHECK(plain.hash() != belted.hash());
}
