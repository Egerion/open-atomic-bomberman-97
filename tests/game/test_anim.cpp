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
using bomber::game::stunned_stand_facing;

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

TEST_CASE("the pickup curve index lags the grab by one frame and saturates") {
    // k = clamp(+80 - 1, 0, 3) and nothing else: the CARRIED half of
    // sub_42331C (the a1=1 call, sub_42459A) runs AFTER sub_420F07, so the +80
    // it reads has already been advanced this frame. An earlier pass had the
    // two passes the other way round and paid for it with a second -1.
    CHECK(carry_arc_index(0) == 0);
    CHECK(carry_arc_index(1) == 0);
    CHECK(carry_arc_index(2) == 1);
    CHECK(carry_arc_index(3) == 2);
    CHECK(carry_arc_index(4) == 3);
    CHECK(carry_arc_index(5) == 3);
    CHECK(carry_arc_index(500) == 3);
}

TEST_CASE("the carry freezes the body, and the throw is what plays it") {
    using bomber::game::body_anim_step;
    using bomber::game::body_phase_next;
    constexpr int kFramesPerTick = 9;  // sim::kSubFrames, the canonical cadence

    // Walking into a grab: whatever the counter had, the carry pins it to 0 —
    // sub_42331C's state-3 branch writes the carrier's +48 = 0 every frame, so
    // "walkbomb <dir>" holds step 0 however far the carrier walks.
    std::uint32_t ph = 500;
    for (int t = 0; t < 6; ++t) {
        ph = body_phase_next(ph, /*carrying=*/true, /*carried_last=*/t > 0, 13, kFramesPerTick);
        CHECK(body_anim_step(ph) == 0);
    }
    // The release tick still draws step 0 (the original's release frame is
    // drawn before anything has added to +48 but the frame's own increments).
    ph = body_phase_next(ph, false, /*carried_last=*/true, 0, kFramesPerTick);
    CHECK(body_anim_step(ph) == 0);
    // From there the pickup remainder plays out. Standing still, +48 gains one
    // per displayed frame, so a tick is worth kSubFrames of it and the step
    // advances by three — PUP*.ANI's ten steps inside ~4 ticks.
    ph = body_phase_next(ph, false, false, 0, kFramesPerTick);
    CHECK(body_anim_step(ph) == 3);
    ph = body_phase_next(ph, false, false, 0, kFramesPerTick);
    CHECK(body_anim_step(ph) == 6);
    // Throwing mid-stride uses the walk budget instead, the same /3 the walk
    // cycle gets (13 px of attempted travel -> four steps).
    std::uint32_t walking = body_phase_next(0, false, false, 13, kFramesPerTick);
    CHECK(body_anim_step(walking) == 4);
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

// Direction indices below are `sim::Direction`: 0 Up, 1 Down, 2 Left, 3 Right
// (sequences.cpp names them north/south/west/east in that order). Spelled out
// rather than included, because this suite links nothing but the game headers.
constexpr int kUp = 0, kDown = 1, kLeft = 2, kRight = 3;

TEST_CASE("an unstunned player stands the way it faces") {
    // The mask is inside sub_41F29B's `+58 != 0` arm, so with no head-stun
    // running the idle pose is the plain facing — which is every frame of a
    // normal match, and why this change cannot move a visual pin.
    for (int facing = 0; facing < 4; ++facing) {
        CHECK(stunned_stand_facing(facing, 0, 16) == facing);
        CHECK(stunned_stand_facing(facing, -1, 16) == facing);
    }
}

TEST_CASE("a head-stunned player's stand pose cycles all four facings") {
    // sub_41F29B's idle branch formats `stand %s` from `+80 & 3` while +58 is
    // set. +80 is zeroed by sub_421F7E at the bonk and counts up one per
    // displayed frame; Player::stun is the same clock as a countdown from
    // Tuning::head_stun_frames, so elapsed = total - remaining.
    //
    // The FACING ARGUMENT IS IGNORED for the whole stun — the same elapsed
    // count gives the same sprite whichever way the player was pointing.
    for (int facing = 0; facing < 4; ++facing) {
        CAPTURE(facing);
        CHECK(stunned_stand_facing(facing, 16, 16) == kUp);     // elapsed 0
        CHECK(stunned_stand_facing(facing, 15, 16) == kRight);  // elapsed 1
        CHECK(stunned_stand_facing(facing, 14, 16) == kDown);   // elapsed 2
        CHECK(stunned_stand_facing(facing, 13, 16) == kLeft);   // elapsed 3
        CHECK(stunned_stand_facing(facing, 12, 16) == kUp);     // elapsed 4, wraps
    }
}

TEST_CASE("the stun spin turns CLOCKWISE, in the original's godir order") {
    // The load-bearing detail. The masked value indexes the ORIGINAL's name
    // table off_45BCC4 = {"north","east","south","west"} (sub_413AED,
    // docs/re/sequence-map.md), not our Direction enum. Feeding `& 3` straight
    // into our index would draw the same four sprites in the order
    // north, south, west, east — a spin the original never shows. This case is
    // the adapter's reason to exist; if it fails, the bomberman is spinning
    // wrong, not merely differently.
    const int expected[4] = {kUp, kRight, kDown, kLeft};  // N, E, S, W
    int visits[4] = {0, 0, 0, 0};
    for (int elapsed = 0; elapsed < 16; ++elapsed) {
        CAPTURE(elapsed);
        const int got = stunned_stand_facing(kDown, 16 - elapsed, 16);
        CHECK(got == expected[elapsed % 4]);
        for (int i = 0; i < 4; ++i)
            if (got == expected[i]) ++visits[i];
    }
    // Four full turns over the confirmed 16-frame stun (sub_421F7E's literal).
    // This used to read `CHECK(16 / 4 == 4)`, which is a statement about
    // arithmetic and not about the port — it could not fail. The claim the
    // comment actually makes is that the 16-frame window shows each of the four
    // sprites exactly four times, so assert that instead.
    for (int i = 0; i < 4; ++i) {
        CAPTURE(i);
        CHECK(visits[i] == 4);
    }
}

TEST_CASE("a nonsensical stun pair cannot index out of the table") {
    // Defensive only: nothing in the port sets stun above head_stun_frames, but
    // a negative elapsed would be an out-of-bounds read rather than a wrong
    // sprite, so it clamps to the first entry.
    CHECK(stunned_stand_facing(kLeft, 40, 16) == kUp);
    CHECK(stunned_stand_facing(kLeft, 1, 0) == kUp);
}
