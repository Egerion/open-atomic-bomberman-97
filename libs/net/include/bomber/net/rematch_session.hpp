#pragma once

#include <cstdint>

#include "bomber/net/transport.hpp"

// THE POST-MATCH DOOR — "the match is decided; both of us walk back to the setup
// screens over the SAME transport" (wire v8, design §10).
//
// The rematch needs no new mechanism, only this door: both peers reach "the
// match is over" with no traffic at all (same sim, same inputs, same clinch),
// and SetupSession needs nothing but a connected Transport. What they cannot
// agree on unaided is WHEN to leave the outcome screens — and getting that wrong
// is not cosmetic, because the guest's next SetupSession starts a liveness
// timeout the moment it is built, so a host still reading the scoreboard is
// indistinguishable from a host that quit and the guest reports THE HOST LEFT
// THE GAME. So the host announces the transition, exactly as it announces
// everything else on these screens (docs/re/network-screens.md §7).
//
// THE CONTROL PLANE IS GONE BY NOW, AND THAT IS FINE: the matchmaker reaps a
// lobby about 30 s into a match, so anything here that needed the server would
// work in a test and fail in a real game. Every byte crosses the P2P Transport.
//
// SELF-HEALING, because this is UDP: the host re-sends on an interval, and the
// guest ALSO opens on any inbound SETUP datagram — so if every copy of Rematch
// is lost, the host's first preview says the same thing implicitly.
//
// Pump-based and clock-injected, and the SAME one-pump-at-a-time obligation as
// SetupSession applies: they all drain one Transport.

namespace bomber::net {

// Re-send interval for the host's liveness/handoff announcements, in the
// caller's monotonic milliseconds. Comfortably inside any sane timeout while
// costing 6 bytes a shot.
inline constexpr int kRematchResendMs = 250;

class RematchSession {
public:
    // `t` is BORROWED and must outlive the session — the caller owns the socket
    // and hands the SAME transport to the SetupSession that follows.
    //
    // `timeout_ms` guards ONE wait: hearing nothing at all from the peer while
    // waiting for it. Both roles run it (a host whose guest quit under the
    // victory screen should not sit forever either), and both re-send inside it,
    // so silence really does mean silence.
    RematchSession(Transport& t, bool is_host, int timeout_ms = 30000);

    // HOST only: the local player dismissed the last outcome screen. From here
    // the transition is announced every pump until we stop pumping. A guest's
    // call is ignored — a guest cannot decide this any more than it can decide a
    // round's end.
    void accept();

    // Drain inbound, then (re)send whatever this role owes. Safe every frame.
    void step(std::int64_t now_ms);

    // Leave the outcome screens now. True on the host as soon as it accepts (it
    // keeps announcing while it walks into the setup screens, and its first
    // preview is a second, implicit announcement); true on a guest once the
    // host's transition — or the setup traffic that supersedes it — arrives.
    bool ready() const { return ready_; }

    // Nothing at all from the peer for `timeout_ms`. The caller ends the session
    // rather than hanging on a peer that is never coming back.
    bool failed() const { return failed_; }

    bool is_host() const { return is_host_; }

private:
    void drain(std::int64_t now_ms);

    Transport* transport_;
    std::int64_t last_send_ms_ = -1;  // -1 = nothing sent yet, so step() sends at once
    std::int64_t last_rx_ms_ = -1;    // -1 = the liveness clock has not started
    int timeout_ms_;
    bool is_host_;
    bool accepted_ = false;
    bool ready_ = false;
    bool failed_ = false;
};

}  // namespace bomber::net
