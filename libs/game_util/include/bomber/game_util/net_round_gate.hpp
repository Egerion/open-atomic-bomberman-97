#pragma once

// THE BETWEEN-ROUNDS SEAM for an ONLINE match — "this outcome screen is
// dismissed by someone else", the round-rotation twin of `NetSetupLink`.
//
// FROM THE ORIGINAL (docs/re/in-match-shell.md "The round-end shell"):
// `sub_42A3F6`'s two outcome screens are bespoke wait loops that treat a network
// client differently. The accept keys (13/32) are joined by a network-only code
// (904 in DRAW, 903 in RESULTS) that a peer cannot type, and a client
// (`sub_40C06A() == 1`) pressing a key gets `sub_427961(40)` — the "you can't do
// that here" buzz any host-only control gives a guest (network-screens.md §7).
// The HOST dismisses; the guests follow.
//
// Our peers already agree on the outcome, the tally and the clinch with no
// traffic at all. What they cannot agree on unaided is WHEN to leave the screen
// and WHAT the next round's config is — and a peer starting round N+1 while the
// other reads the scoreboard would sit at the prediction cap on a frozen frame.
//
// Nothing here can desync a simulation: no RollbackSession exists while a gate
// is up (the round's session is destroyed before the screen opens and the next
// built after it closes), and the only shared state crossing is the confirmed
// `sim::MatchConfig`, byte-compared by the existing checksum/ack path. A null
// gate is ordinary LOCAL play.

namespace bomber::game {

class NetRoundGate {
public:
    virtual ~NetRoundGate() = default;

    NetRoundGate(const NetRoundGate&) = delete;
    NetRoundGate& operator=(const NetRoundGate&) = delete;

    // Once per RENDERED FRAME while the screen is up. The screen owns the ONLY
    // pump of the transport for its whole duration (setup_session.hpp's
    // one-pump-at-a-time rule: whichever session polls first eats the datagram,
    // and on a star that same poll is what reflects), so a screen that stops
    // calling this strands the link.
    virtual void pump() = 0;

    // True on a GUEST (`sub_40C06A() == 1`): may look, never dismiss. The screen
    // buzzes SFX 40 at an accept key instead of leaving.
    virtual bool readonly() const = 0;

    // HOST only: an accept key was pressed. Commits the next round. The screen
    // keeps rendering until ready() — the commitment still has to reach the peer.
    virtual void accept() = 0;

    // BOTH peers hold the next round: leave the screen now.
    virtual bool ready() const = 0;

    // The link died while waiting: leave, and let the caller end the match
    // rather than hang on a peer that is never coming back.
    virtual bool failed() const = 0;

protected:
    NetRoundGate() = default;
};

}  // namespace bomber::game
