// Locks the reverse-engineered ANI pacing model (sub_41DAA7): the displayed
// step is counter % statecnt, and the STAT HEAD timing u16 (head0, 0x001E /
// 0xFFFF) is inert — it can never change which frame is shown. See
// docs/re/facts.md "ANI per-step timing (STAT HEAD u16) — CONFIRMED INERT".

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "bomber/game/anim_pace.hpp"
#include "bomber/game/carry_pose.hpp"

using bomber::game::anim_step_index;
using bomber::game::carried_bomb_offset;
using bomber::game::carry_arc_index;
using bomber::game::PlayerPose;
using bomber::game::PoseFlags;
using bomber::game::select_player_pose;

TEST_CASE("frame selection wraps modulo the step count") {
    // A 4-step looping sequence, exactly like the original's counter % statecnt.
    CHECK(anim_step_index(0, 4) == 0);
    CHECK(anim_step_index(3, 4) == 3);
    CHECK(anim_step_index(4, 4) == 0);   // wrap
    CHECK(anim_step_index(9, 4) == 1);   // second cycle
    CHECK(anim_step_index(4000, 4) == 0);
}

TEST_CASE("a single-step sequence always shows step 0") {
    for (std::size_t c = 0; c < 100; ++c) CHECK(anim_step_index(c, 1) == 0);
}

TEST_CASE("an empty animation is safe (no modulo-by-zero)") {
    CHECK(anim_step_index(0, 0) == 0);
    CHECK(anim_step_index(12345, 0) == 0);
}

TEST_CASE("the STAT HEAD timing field cannot affect frame selection") {
    // Model a sequence whose per-step head0 values are the two the field ever
    // takes (0x001E / 0xFFFF), interleaved. The frame shown must depend only on
    // (counter, statecnt); head0 is not even reachable from the selector. This
    // is the whole CONFIRMED-INERT finding, encoded as a regression guard.
    const std::vector<std::uint16_t> head0 = {0x001E, 0xFFFF, 0x001E, 0xFFFF, 0x001E};
    const std::size_t n = head0.size();
    for (std::size_t counter = 0; counter < 5 * n; ++counter) {
        const std::size_t shown = anim_step_index(counter, n);
        CHECK(shown == counter % n);          // pure wrap
        CHECK(shown < n);                     // always in range
        // Flipping any step's timing value would change nothing: the selector
        // never consults head0, so the mapping above is total and stable.
        (void)head0[shown];
    }
}

// ---------------------------------------------------------------------------
// The glove (grab / carry / throw) sequence — carry_pose.hpp. See that header
// for the sub_41F29B / sub_42331C citations behind each expectation.
// ---------------------------------------------------------------------------

TEST_CASE("the base pose keys off walking and the carried-bomb pointer") {
    PoseFlags f;
    CHECK(select_player_pose(f) == PlayerPose::Stand);
    f.moving = true;
    CHECK(select_player_pose(f) == PlayerPose::Walk);
    f.carrying = true;
    CHECK(select_player_pose(f) == PlayerPose::WalkBomb);
    f.moving = false;
    CHECK(select_player_pose(f) == PlayerPose::StandBomb);
}

TEST_CASE("an action state overwrites the carry pose, not the other way round") {
    // sub_41F29B formats "walkbomb %s" first and then lets the +78 switch
    // overwrite the buffer with "kick %s" — a carrying player who kicks a bomb
    // shows the KICK pose. The port had this precedence inverted.
    PoseFlags f;
    f.moving = true;
    f.carrying = true;
    f.kick = true;
    CHECK(select_player_pose(f) == PlayerPose::Kick);
    f.kick = false;
    f.punch = true;
    CHECK(select_player_pose(f) == PlayerPose::Punch);
    f.punch = false;
    f.pickup = true;
    CHECK(select_player_pose(f) == PlayerPose::Pickup);
}

TEST_CASE("the pickup pose survives the throw") {
    // State 4 is exited by its own animation length; the release clears +148
    // (carrying) and never touches +78. Grab, throw one tick later, and the
    // remaining "pickup <dir>" frames still play — empty-handed.
    PoseFlags f;
    f.pickup = true;
    f.carrying = false;
    CHECK(select_player_pose(f) == PlayerPose::Pickup);
    f.moving = true;
    CHECK(select_player_pose(f) == PlayerPose::Pickup);
}

TEST_CASE("warp and the boxed-in fidget sit at the ends of the precedence") {
    PoseFlags f;
    f.carrying = true;
    f.cornerhead = true;
    CHECK(select_player_pose(f) == PlayerPose::Cornerhead);  // beats the carry pose
    f.pickup = true;
    CHECK(select_player_pose(f) == PlayerPose::Pickup);  // state 0 is required to fidget
    f.warping = true;
    CHECK(select_player_pose(f) == PlayerPose::Spin);  // states 6/7 gate everything
}

TEST_CASE("the pickup curve index lags the grab by two frames and saturates") {
    // k = clamp(+80 - 1, 0, 3), and the bomb pass reads +80 one frame stale
    // (sub_4245B9 runs before sub_420F07), so k = clamp(frames - 2, 0, 3).
    CHECK(carry_arc_index(0) == 0);
    CHECK(carry_arc_index(1) == 0);
    CHECK(carry_arc_index(2) == 0);
    CHECK(carry_arc_index(3) == 1);
    CHECK(carry_arc_index(4) == 2);
    CHECK(carry_arc_index(5) == 3);
    CHECK(carry_arc_index(500) == 3);
}

TEST_CASE("the held bomb leaves the curve and rides above the head") {
    // The shipped curve (VALUELST 500/502/504/506) is (12,10)/(25,20)/(25,30)/
    // (12,40); the forward reach is the curve's X PLUS a flat 10 px nudge.
    CHECK(carried_bomb_offset(true, 12, 10).forward == 22);
    CHECK(carried_bomb_offset(true, 12, 10).lift == 10);
    CHECK(carried_bomb_offset(true, 25, 30).forward == 35);
    CHECK(carried_bomb_offset(true, 25, 30).lift == 30);
    // Once the pickup animation ends, sub_42331C's else branch drops the curve
    // entirely: 10 px forward, 40 px up, whatever the curve said. Feeding it
    // the last curve point proves the X term really is gone (22 -> 10) while
    // the lift happens to agree.
    CHECK(carried_bomb_offset(false, 12, 40).forward == 10);
    CHECK(carried_bomb_offset(false, 12, 40).lift == 40);
    CHECK(carried_bomb_offset(false, 25, 30).forward == 10);
    CHECK(carried_bomb_offset(false, 25, 30).lift == 40);
}
