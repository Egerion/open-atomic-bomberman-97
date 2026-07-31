#pragma once

#include <cstdint>

// The glove (grab) sequence: which player pose the original picks, and where a
// carried bomb rides while it is held. Pure integer logic lifted from two RE'd
// functions, so it lives here rather than inside Renderer::draw_world.
//
//  * Pose choice: `sub_41F29B`'s name-build block (LABEL_155 onwards). It
//    formats the BASE name from the walk/idle flag and the carried-bomb pointer
//    (+148) — "stand %s"/"walk %s", or "standbomb %s"/"walkbomb %s" — and THEN
//    lets the action-state word (+78) OVERWRITE that whole buffer with "kick %s"
//    (state 1), "punch %s" (2) or "pickup %s" (4). So an action pose BEATS the
//    carry pose, and the pickup pose is chosen by the state word ALONE: it keeps
//    playing after the held bomb has been thrown, because the release clears
//    +148 but never touches +78. See facts.md "Player state machine (+78)".
//  * Carried-bomb offset: `sub_42331C`'s motion-state 3 branch. It reads the
//    CARRIER's +78 and walks the 4-point VALUELST pickup curve only while that
//    is 4; in every other state the bomb rides at a FIXED 10 px along the facing
//    direction and 40 px up.

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

// `moving` is the original's godir != -1 (a direction reached the mover this
// frame, displacement or not).
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
// A straight `-1`, NOT the `-2` a previous pass used. That extra -1 came from
// the claim that the bomb pass runs BEFORE the player pass, which is inverted:
// `sub_42331C` is called TWICE per frame off the +148 filter in its own first
// `if`, and the CARRIED half is the SECOND (the frame loop at pseudo.c
// ~29522-29527 runs sub_4245B9 -> sub_42331C(0), then sub_420F07 -> sub_41F29B,
// then sub_42459A -> sub_42331C(1)). A carried bomb therefore reads a +80 the
// player pass has ALREADY advanced this frame.
constexpr int carry_arc_index(int frames_since_grab) {
    int k = frames_since_grab - 1;
    if (k < 0) k = 0;
    if (k > 3) k = 3;
    return k;
}

// WHILE A BOMB IS HELD, THE CARRIER'S BODY IS A STILL IMAGE. The displayed step
// of every pose reaching `sub_41F29B`'s shared draw tail — stand, walk,
// standbomb, walkbomb AND pickup — is +48 divided by 3 (`sub_41DAA7(seq, (u16)
// player[+48] / 3)`, pseudo.c 23410; kick and punch escape via `goto
// LABEL_239`), and the carried-bomb pass writes +48 straight back to zero every
// frame the link holds (`*(_WORD *)(carrier + 48) = 0`, ~25488). Because that
// pass runs AFTER the player pass, the next draw sees only that frame's own
// increments: at ~180 fps a walker manages well under one pixel and the idle
// branch adds exactly 1, so `/3` is 0 either way.
//
// The RELEASE is what starts the animation: +148 clears, the zeroing stops, +48
// accumulates from 0, and any state-4 remainder plays PUP*.ANI from its first
// step — the closest thing the original has to a throw animation, and invisible
// in a port that lets the carry pose animate.

// One tick's worth of +48, which advances once per PIXEL stepped inside the
// mover (sub_41EC84, 22718) and once per FRAME idle (23084) — hence
// `frames_per_tick`, since a tick covers several displayed frames.
constexpr std::uint32_t body_phase_step(int walk_px, int frames_per_tick) {
    if (walk_px > 0) return static_cast<std::uint32_t>(walk_px);
    return frames_per_tick > 0 ? static_cast<std::uint32_t>(frames_per_tick) : 0u;
}

// +48 after this tick. Pinned to 0 for the whole carry AND for the release tick
// itself: the original's release frame is drawn before anything clears the link.
constexpr std::uint32_t body_phase_next(std::uint32_t phase, bool carrying, bool carried_last,
                                        int walk_px, int frames_per_tick) {
    if (carrying || carried_last) return 0;
    return phase + body_phase_step(walk_px, frames_per_tick);
}

// The displayed step, before the caller's `% statecnt`.
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

// THE HEAD-STUNNED IDLE POSE SPINS. `sub_41F29B`'s idle branch normally builds
// `stand %s` from the player's own facing; while the head-stun word +58 is
// non-zero it formats the state's elapsed-frame counter masked to two bits,
// `+80 & 3` (facts.md's "Stun does NOT gate flame-death or pickup" noted this at
// ~23086 without unpacking it). A bonked bomberman turns on the spot for the
// whole 16-frame stun — the visual tell that a hit landed, which the port lacked.
//
// THE MASKED VALUE IS A `godir`, NOT ONE OF OUR `Direction`s. The original
// formats through `off_45BCC4[godir & 3] = {north, east, south, west}`
// (sub_413AED, docs/re/sequence-map.md), i.e. compass-CLOCKWISE, while
// sequences.cpp maps our {Up, Down, Left, Right} to {north, south, west, east}.
// Feeding `& 3` straight in would spin N, S, W, E — the same four frames in an
// order the original never shows.
inline constexpr int kGodirToDirection[4] = {0, 3, 1, 2};  // N, E, S, W -> Up, Right, Down, Left

// `stun_remaining` is `Player::stun`, the port's +58 — a COUNTDOWN, where the
// original's +80 counts UP from the zeroing `sub_421F7E` does at the bonk. So
// `stun_total - remaining` is the elapsed count +80 holds.
constexpr int stunned_stand_facing(int facing, int stun_remaining, int stun_total) {
    if (stun_remaining <= 0) return facing;
    const int elapsed = stun_total - stun_remaining;
    return kGodirToDirection[(elapsed > 0 ? elapsed : 0) & 3];
}

}  // namespace bomber::game
