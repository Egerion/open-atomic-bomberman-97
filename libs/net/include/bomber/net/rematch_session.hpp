#pragma once

#include <cstdint>

#include "bomber/net/transport.hpp"

// THE POST-MATCH RENDEZVOUS — "the match is decided; both of us walk back to the
// setup screens over the SAME transport" (wire v8, MatchCtlKind::RematchWait /
// Rematch).
//
// THE BUG IT CLOSES. An online match used to END at the VICTORY screen: the
// caller returned, the UdpTransport left scope, and the socket both players had
// punched a path for was gone. Two people who had just finished a game and
// wanted another one had to go back through the lobby, re-punch, and hope. But
// the transport is perfectly alive at that moment, and SetupSession — the
// host-authoritative roster/map exchange — needs nothing but a connected
// Transport. So the fix is not a new mechanism: it is running SetupSession again
// over the link that is already there. This class is only the door between the
// two.
//
// WHY A DOOR IS NEEDED AT ALL. Both peers reach "the match is over" with no
// traffic (same sim, same inputs, same clinch), so that part needs no message.
// What they cannot agree on unaided is WHEN to leave the outcome screens, and
// getting it wrong is not cosmetic: the guest's next SetupSession starts a
// liveness timeout the moment it is built, and a host still reading the
// scoreboard is indistinguishable from a host that quit. The guest would time
// out and report THE HOST LEFT THE GAME — the very disconnect this change
// exists to remove, moved thirty seconds later. So the host announces the
// transition, exactly as it announces everything else on these screens
// (docs/re/network-screens.md §7: the machine driving the game dismisses the
// shared screens, a `sub_40C06A() == 1` client follows).
//
// THE CONTROL PLANE IS GONE BY NOW, AND THAT IS FINE. The matchmaker reaps a
// lobby about 30 s into a match (HeartbeatInterval 10 x HeartbeatMiss 3), so
// anything here that needed the server would work in a test and fail in a real
// game. Nothing here does: every byte crosses the peer-to-peer Transport.
//
// SELF-HEALING, because this is UDP. The host re-sends on an interval, and the
// guest ALSO opens on any inbound SETUP datagram — if every copy of Rematch is
// lost, the host's first SetupSession preview says the same thing implicitly and
// the guest follows that instead. Consuming one preview costs nothing:
// SetupSession re-broadcasts every kSetupPreviewResendMs.
//
// Pump-based and CLOCK-INJECTED like SetupSession / Rendezvous / LobbyFlow:
// step(now_ms) takes the caller's monotonic clock, so libs/net stays clock-free
// and the whole flow is deterministically testable. The SAME one-pump-at-a-time
// obligation applies: this and the setup/match sessions all drain one Transport,
// so only one of them may be running.

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
