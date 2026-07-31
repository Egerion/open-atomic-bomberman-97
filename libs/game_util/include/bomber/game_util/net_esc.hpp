#pragma once

#include <cstdint>

// THE ONLINE ESC RULE. The two acts a press can mean have OPPOSITE authority
// models: STOP the round is HOST ONLY, because it changes what both machines
// simulate; LEAVE the match is EVERYONE, because it removes only yourself and so
// must never depend on a peer answering. The wire-v8 match-shell change removed
// the second by accident — Esc became "ask the host to end the round", and with
// the peer stalled the answer never came, so a broken match could not be left.
//
// A pure object rather than an `if` in the event loop because the case that
// matters is a peer that has stopped responding, and everything around it in
// MatchRunner is SDL — so the dead-peer case is a headless test instead of
// something only reproducible with two machines and a broken network.

namespace bomber::game {

enum class EscPress : std::uint8_t {
    // First press. On a host this also asks the session to stop the round; on a
    // guest it only raises the prompt, because a guest has no say.
    Arm,
    // Second press inside the window: tear down and go to the main menu —
    // local, immediate, nothing sent, nothing waited on.
    Leave,
};

// Long enough to press twice deliberately, short enough that a stray Esc does
// not leave the player one keystroke from quitting minutes later. Never hidden
// state: the prompt is on screen for exactly this span.
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
    // press leaves. ONE predicate for both, so the screen cannot promise
    // something the key handler will not honour.
    //
    // The `stop_outstanding` half is load-bearing: a stop completes only when
    // every peer reaches the agreed tick, so against a peer that has stopped
    // responding it NEVER completes. Arming on a timer alone would close the
    // window three seconds into exactly the situation it exists for.
    bool armed(std::uint64_t now_ms, bool stop_outstanding) const {
        return now_ms < armed_until_ms_ || stop_outstanding;
    }

private:
    std::uint64_t armed_until_ms_ = 0;
};

}  // namespace bomber::game
