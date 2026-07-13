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
    // The belt adds conveyor_speed to the move budget each tick, spent 100/px,
    // accumulating the remainder — the SAME budget units as walking (id 42).
    long budget = 0, expected = 0;
    for (int t = 0; t < 10; ++t) {
        budget += st.tuning.conveyor_speed();
        while (budget > 0) { budget -= 100; ++expected; }
    }
    CHECK(moved == static_cast<int>(expected));
    CHECK(p.tile_y() == 0);                 // stayed on the belt lane
    CHECK(p.facing == Direction::Right);    // the belt forced its facing
    // The conveyor-speed OPTION defaults to the binary's hardcoded 1 (medium =
    // getvalue(191) = 350), NOT the low tier — the "too fast/slow" fix. The
    // per-tick belt budget shares the same 1/100-px units as walking speed.
    CHECK(st.tuning.conveyor_speed_index == 1);
    CHECK(st.tuning.conveyor_speed() == 350);
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

TEST_CASE("a head-stunned player on a conveyor is still carried by the belt") {
    // sub_41F29B decrements the +58 stun countdown every tick (~22982), but the
    // gate it drives (v113, ~23028) blocks ONLY new-input acquisition — NOT the
    // mover. With the new-direction word +46 left at its per-tick -1 reset
    // (22980) a stunned player takes the IDLE movement branch (23413), where a
    // conveyor under it still forces +46 to the belt direction and runs the
    // per-pixel stepper (23417-23423) — identical to a keyless standing player.
    // The prior port's full early-return froze a stunned player solid even on a
    // belt — the divergence this pins. docs/re/facts.md "Head hit" (stunned
    // movement RESOLVED 2026-07-10).
    Simulation s(open_config());
    State& st = s.state();
    belt_row0_east(st);
    Player& p = st.players[0];
    p.x = kTileWF / 2;  // (0,0) dead centre
    p.y = kTileHF / 2;
    p.facing = Direction::Down;
    p.stun = 16;  // the head-hit value (sub_421F7E hardcodes a1[29] = 16)

    const int x0 = p.x;
    run(s, 10, TickInputs{});  // stunned throughout (16 > 10); no key input

    // Carried EXACTLY as far as the un-stunned standing player in the first
    // test: the same belt budget, spent 100/px, remainder carried.
    const int moved = (p.x - x0) / 100;
    long budget = 0, expected = 0;
    for (int t = 0; t < 10; ++t) {
        budget += st.tuning.conveyor_speed();
        while (budget > 0) { budget -= 100; ++expected; }
    }
    CHECK(moved == static_cast<int>(expected));  // the belt kept carrying it
    CHECK(p.tile_y() == 0);                      // along the belt lane
    CHECK(p.facing == Direction::Right);         // belt-forced facing, as when idle
    // The +58 countdown burns once per canonical FRAME (3/tick — facts.md
    // "Canonical frame cadence"): 16 frames are gone within ceil(16/3) = 6
    // ticks. With no key held the belt behaviour is identical either way, so
    // the movement checks above are unaffected by the early expiry.
    CHECK(p.stun == 0);
}

TEST_CASE("a stunned player takes no new input and does not coast; input resumes after") {
    // While +58 > 0 the v113 gate skips sub_41E61E entirely, so a HELD key never
    // becomes a direction: +46 stays -1 and the mover's keyed branch (23430) is
    // unreachable. There is no momentum to coast on either — the original resets
    // +46 every tick (22980) and the budget loop drains to <= 0 within the tick
    // that granted it (sub_41EC84 22568), so a mid-walk head-hit stops the walk
    // dead until the countdown clears. Off a belt, a stunned player stands still.
    Simulation s(open_config());
    State& st = s.state();
    Player& p = st.players[0];
    p.x = kTileWF / 2;  // (0,0) centre, plain floor — no actor involved
    p.y = kTileHF / 2;

    TickInputs east;
    east.players[0].right = true;

    run(s, 4, east);  // walk east at full speed first (mid-movement...)
    const int x_at_stun = p.x;
    CHECK(x_at_stun > kTileWF / 2);  // the pre-stun walk really moved

    // 9 frames = exactly 3 ticks of block at the canonical 3-frames-per-tick
    // stun cadence (facts.md "Canonical frame cadence").
    p.stun = 9;       // ...then a stun lands (white-box, as the AI suite does)
    run(s, 3, east);  // the key is HELD the whole time
    CHECK(p.x == x_at_stun);  // no coasting, no new input: frozen in place
    CHECK(p.stun == 0);       // 9 frames burned across the 3 blocked ticks

    run(s, 3, east);          // stun over: the held key moves the player again
    CHECK(p.x > x_at_stun);
}
