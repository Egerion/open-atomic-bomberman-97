#pragma once

#include <cstdint>

// The glove (grab) sequence: which player pose the original picks, and where a
// carried bomb rides while it is held.
//
// Both decisions are pure integer logic lifted from two RE'd functions, so they
// live here (SDL-free, header-only) instead of inside Renderer::draw_world —
// the headless suite can then pin them (tests/game/test_anim.cpp).
//
//  * Pose choice: `sub_41F29B`'s name-build block (LABEL_155 onwards). It first
//    formats the BASE name from the walk/idle flag and the carried-bomb pointer
//    (+148) — "stand %s"/"walk %s", or "standbomb %s"/"walkbomb %s" while
//    carrying — and THEN lets the action-state word (+78) overwrite that whole
//    buffer with "kick %s" (state 1), "punch %s" (state 2) or "pickup %s"
//    (state 4). So an action pose BEATS the carry pose, not the other way
//    round, and the pickup pose is chosen by the state word ALONE: it keeps
//    playing after the held bomb has been thrown (the release in the
//    bomb-action block clears +148 but never touches +78). See
//    docs/re/facts.md "Player state machine (+78) — COMPLETE".
//
//  * Carried-bomb offset: `sub_42331C`'s bomb motion-state 3 ("carried")
//    branch. It reads the CARRIER's +78 and, only while that is 4 (the pickup
//    animation), walks the 4-point VALUELST pickup curve; in every other state
//    the bomb rides at a FIXED offset — 10 px along the facing direction and
//    40 px up, i.e. straight above the head.
//
//  * Body animation phase: the SAME state-3 branch also writes the carrier's
//    +48 (the body anim counter whose /3 is the displayed step) back to 0 —
//    every frame, for as long as the bomb is held. See `body_phase_next`.

namespace bomber::game {

// Which sequence family the body is drawn from this frame. Ordered like
// sub_41F29B's own branches: the four base poses, then the state overrides.
enum class PlayerPose : std::uint8_t {
    Stand,       // "stand <dir>"      — idle
    Walk,        // "walk <dir>"       — moving
    StandBomb,   // "standbomb <dir>"  — idle, carrying (+148)
    WalkBomb,    // "walkbomb <dir>"   — moving, carrying (+148)
    Kick,        // "kick <dir>"       — state 1
    Punch,       // "punch <dir>"      — state 2
    Pickup,      // "pickup <dir>"     — state 4
    Cornerhead,  // "cornerhead <n>"   — states 20-39 (boxed-in fidget)
    Spin,        // "spin"             — states 6/7 (warp)
};

// The per-frame flags the pose choice depends on. `moving` is the original's
// godir != -1 (a direction reached the mover this frame, displacement or not).
struct PoseFlags {
    bool moving = false;
    bool carrying = false;    // +148, the carried-bomb pointer
    bool kick = false;        // state 1 anim running
    bool punch = false;       // state 2 anim running
    bool pickup = false;      // state 4 anim running
    bool cornerhead = false;  // states 20-39
    bool warping = false;     // states 6/7
};

// The original's states are ONE word, so at most one override can be live; the
// order below only decides ties our presentation-side timers can produce, and
// follows the switch's own order (1, 2, 4, then the >4 branch).
constexpr PlayerPose select_player_pose(const PoseFlags& f) {
    if (f.warping) return PlayerPose::Spin;
    if (f.kick) return PlayerPose::Kick;
    if (f.punch) return PlayerPose::Punch;
    if (f.pickup) return PlayerPose::Pickup;
    if (f.cornerhead) return PlayerPose::Cornerhead;
    if (f.carrying) return f.moving ? PlayerPose::WalkBomb : PlayerPose::StandBomb;
    return f.moving ? PlayerPose::Walk : PlayerPose::Stand;
}

// Where the held bomb sits relative to the carrier's ground anchor:
//   x = carrier_x + dir_dx * forward
//   y = carrier_y + dir_dy * 10 - lift
// (`sub_42331C` applies the curve's forward reach to X only, and its vertical
// column as a plain subtraction — the 10 px nudge is what carries into Y.)
struct CarryOffset {
    int forward = 10;
    int lift = 40;
};

// The 4-point pickup curve is indexed by the carrier's +80 (frames elapsed in
// state 4) MINUS ONE, clamped to 0..3 — `v60 = (+80 >> 16) - 1`.
//
// `frames_since_grab` counts our own ticks with 0 on the grab tick, and +80 is
// zeroed by the grab itself, so this is a straight `-1` and NOT the `-2` a
// previous pass used. That extra -1 was derived from the claim that the bomb
// pass runs BEFORE the player pass, which is inverted: `sub_42331C` is called
// TWICE per frame off the +148 (carry-link) filter in its own first `if`, and
// the CARRIED half is the second call —
//
//     sub_4245B9 -> sub_42331C(0, ..)   // +148 == 0: every un-carried bomb
//     sub_420F07 -> sub_41F29B          // the players: move, then draw
//     sub_42459A -> sub_42331C(1, ..)   // +148 != 0: the carried bombs
//
// (the frame loop at pseudo.c ~29522-29527). So a carried bomb reads a +80 the
// player pass has ALREADY advanced this frame, not last frame's value.
constexpr int carry_arc_index(int frames_since_grab) {
    int k = frames_since_grab - 1;
    if (k < 0) k = 0;
    if (k > 3) k = 3;
    return k;
}

// While a bomb is held, the carrier's BODY IS A STILL IMAGE.
//
// The displayed step of every body pose that reaches `sub_41F29B`'s shared draw
// tail — stand, walk, standbomb, walkbomb AND pickup — is the player's +48
// counter divided by 3 (`sub_41DAA7(seq, (u16)player[+48] / 3)`, pseudo.c
// 23410; kick and punch escape it by `goto LABEL_239` with their own +80 frame).
// +48 advances once per PIXEL stepped inside the mover (`sub_41EC84`, 22718) and
// once per FRAME in the idle branch (23084).
//
// The carried-bomb pass then writes it straight back to zero — `*(_WORD *)
// (carrier + 48) = 0`, the first statement of `sub_42331C`'s state-3 branch
// (~25488) — every frame, for as long as +148 links the two. Because that pass
// runs after the player pass (see above), the value the next frame's draw sees
// is only what THAT frame added: at the original's ~180 fps a walker manages
// well under one pixel and the idle branch adds exactly 1, so `/3` is 0 either
// way. The carry therefore holds "walkbomb <dir>" step 0 / "standbomb <dir>"
// (a 1-step sequence anyway) and, while state 4 lasts, "pickup <dir>" step 0.
//
// The release is what starts the animation: it clears +148, the zeroing stops,
// +48 accumulates from 0 again, and any state-4 remainder plays PUP*.ANI out
// from its first step — arms sweeping up and away. That playout is the closest
// thing the original has to a throw animation, and it is invisible in a port
// that lets the carry pose animate instead.

// One tick's worth of +48. `walk_px` is the mover's per-tick pixel budget (0
// when idle, the same number `Event::PlayerWalking` carries); `frames_per_tick`
// is how many DISPLAYED frames a tick covers, because the idle branch adds one
// per frame and not per tick.
constexpr std::uint32_t body_phase_step(int walk_px, int frames_per_tick) {
    if (walk_px > 0) return static_cast<std::uint32_t>(walk_px);
    return frames_per_tick > 0 ? static_cast<std::uint32_t>(frames_per_tick) : 0u;
}

// +48 after this tick. Pinned to 0 for the whole carry AND for the release tick
// itself: the original's release frame is drawn before anything clears the
// link, so it still shows only that one frame's own increments — step 0.
constexpr std::uint32_t body_phase_next(std::uint32_t phase, bool carrying, bool carried_last,
                                        int walk_px, int frames_per_tick) {
    if (carrying || carried_last) return 0;
    return phase + body_phase_step(walk_px, frames_per_tick);
}

// The displayed step, before the caller's `% statecnt`: the original's
// `(u16)player[+48] / 3`.
constexpr std::uint32_t body_anim_step(std::uint32_t phase) {
    return phase / 3u;
}

// `in_pickup_state` is the carrier's +78 == 4 test. arc_x/arc_y are the curve
// point (VALUELST 500+2k / 501+2k) for `carry_arc_index`'s k; they are ignored
// outside the pickup state, where the original hardcodes 0 / 40.
constexpr CarryOffset carried_bomb_offset(bool in_pickup_state, int arc_x, int arc_y) {
    if (!in_pickup_state) return CarryOffset{10, 40};
    return CarryOffset{10 + arc_x, arc_y};
}

}  // namespace bomber::game
