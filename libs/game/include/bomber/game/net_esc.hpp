#pragma once

#include <cstdint>

// THE ONLINE ESC RULE — what a press means, and when a second one leaves.
//
// Esc in a netplay round is two different acts depending on how many times you
// press it, and the two have opposite authority models:
//
//   STOP the round — HOST ONLY. It changes what BOTH machines simulate (they
//     stop at one agreed tick and the round becomes a draw), so it needs one
//     authority or the peers diverge. A guest's press does nothing to the match.
//
//   LEAVE the match — EVERYONE, host and guest alike. It removes only yourself,
//     so it must never depend on a peer answering. This is the escape hatch the
//     wire-v8 match-shell change removed by accident: Esc became "ask the host to
//     end the round", and with the peer stalled the answer never came, so from
//     the player's side Esc was simply dead and a broken match could not be left
//     at all. That is the bug that started this whole thread.
//
// WHY THIS IS A PURE OBJECT AND NOT AN `if` IN THE EVENT LOOP. The case that
// matters is a peer that has stopped responding, and everything around it in
// MatchRunner is SDL — a window, an event queue, a renderer. Keeping the rule
// here means the dead-peer case is a headless test (tests/game/test_net_esc.cpp)
// instead of something only reproducible by having two machines and breaking one.
//
// A NOTE ON WHY THE LOOP WAS NEVER THE PROBLEM. Esc looked dead, but
// SDL_PollEvent runs at the top of every frame and a session held at the
// prediction cap returns from advance() immediately, so the key was always being
// read. It simply had nothing to do but wait. The fix is therefore not to revive
// the loop but to give the key a meaning that needs no peer.

namespace bomber::game {

// What a press of Esc means right now.
enum class EscPress : std::uint8_t {
    // First press. On a host this also asks the session to stop the round; on a
    // guest it does nothing but raise the prompt, because a guest has no say.
    Arm,
    // Second press inside the window. Tear down and go to the main menu — local,
    // immediate, nothing sent, nothing waited on.
    Leave,
};

// How long a first press leaves the "press again to leave" window open. Long
// enough to press twice deliberately, short enough that a stray Esc does not
// leave the player one keystroke from quitting minutes later. It is never a
// hidden state: the prompt is on screen for exactly this span.
inline constexpr std::uint64_t kEscLeaveWindowMs = 3000;

class NetEscState {
public:
    // `now_ms` is a monotonic millisecond clock. `stop_outstanding` is "this peer
    // has asked for the round to stop and it has not happened yet" —
    // RollbackSession::end_round_scheduled() && !round_ended().
    EscPress press(std::uint64_t now_ms, bool stop_outstanding) {
        if (armed(now_ms, stop_outstanding)) return EscPress::Leave;
        armed_until_ms_ = now_ms + kEscLeaveWindowMs;
        return EscPress::Arm;
    }

    // Whether the prompt is up — and, by construction, exactly when a second
    // press leaves. One predicate for both, so the screen cannot promise
    // something the key handler will not honour.
    //
    // The `stop_outstanding` half is the load-bearing one. A stop only completes
    // when every peer reaches the agreed tick, so against a peer that has stopped
    // responding it NEVER completes — and that is precisely when the player needs
    // the way out. Arming on a timer alone would close the window three seconds
    // into exactly the situation it exists for.
    bool armed(std::uint64_t now_ms, bool stop_outstanding) const {
        return now_ms < armed_until_ms_ || stop_outstanding;
    }

private:
    std::uint64_t armed_until_ms_ = 0;
};

}  // namespace bomber::game
