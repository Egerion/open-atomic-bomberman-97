#pragma once

// THE BETWEEN-ROUNDS SEAM for an ONLINE match — "this outcome screen is
// dismissed by someone else", the round-rotation twin of `NetSetupLink`'s
// "this setup screen is driven by someone else".
//
// SHAPE, FROM THE ORIGINAL (docs/re/in-match-shell.md "The round-end shell").
// A match is a sequence of rounds: `sub_42A3F6` leaves its round loop, shows
// DRAW.PCX or the RESULTS tally, and — if nobody has clinched — calls
// `sub_410B6E()` again for the next round rather than returning to the menu.
// Both of those outcome screens are BESPOKE WAIT LOOPS, and both treat a
// network client differently from the machine driving the game: the accept keys
// (13/32) are joined by a network-only code (904 in the DRAW loop, 903 in the
// RESULTS loop) that a peer cannot type, and a client (`sub_40C06A() == 1`)
// that presses a key gets `sub_427961(40)` — the same "you can't do that here"
// buzz `sub_410F81`/`sub_406DDE` give a guest who touches a host-only control
// (docs/re/network-screens.md §7). So the HOST dismisses the outcome screen and
// the guests follow; they do not each dismiss their own.
//
// WHY THE PORT NEEDS AN EXPLICIT SEAM. Our peers are lockstep-deterministic, so
// they already agree on the round outcome, the tally and the clinch with no
// traffic at all (same sim, same inputs, same hash). What they cannot agree on
// unaided is WHEN to leave the screen and WHAT the next round's config is — and
// a peer that starts round N+1 while the other is still reading the scoreboard
// would sit at the prediction cap staring at a frozen frame. The gate makes the
// screen exit a two-peer event: the host's accept commits the next round, the
// guest's screen ends when that commitment arrives.
//
// The outcome screens stay OUTSIDE the ticked timeline entirely — no
// RollbackSession exists while a gate is up (the round's session is destroyed
// before the screen opens and the next round's is built after it closes), so
// nothing here can desync a simulation. The only shared state crossing the gate
// is the confirmed `sim::MatchConfig`, which is compared byte-for-byte by the
// existing checksum/ack path.
//
// A null gate is ordinary LOCAL play: every outcome screen behaves exactly as it
// did before online round rotation existed.

namespace bomber::game {

class NetRoundGate {
public:
    virtual ~NetRoundGate() = default;

    NetRoundGate(const NetRoundGate&) = delete;
    NetRoundGate& operator=(const NetRoundGate&) = delete;

    // Once per RENDERED FRAME while the screen is up. The screen owns the only
    // pump of the transport for its whole duration (setup_session.hpp's
    // one-pump-at-a-time rule: whichever session polls first eats the datagram,
    // and on a star that same poll is what reflects), so a screen that
    // stops calling this strands the link — and, incidentally, this is what
    // drains the socket of the round that just ended.
    virtual void pump() = 0;

    // True on a GUEST: `sub_40C06A() == 1`, may look but never dismiss. The
    // screen buzzes SFX 40 at an accept key instead of leaving.
    virtual bool readonly() const = 0;

    // HOST only: the local player pressed an accept key. Commits the next round
    // (the host's kind-32/903/904 equivalent). The screen keeps rendering until
    // ready() — the commitment still has to reach the peer.
    virtual void accept() = 0;

    // BOTH peers hold the next round: leave the screen now.
    virtual bool ready() const = 0;

    // The link died while waiting. The screen leaves and the caller ends the
    // match, rather than hanging on a peer that is never coming back.
    virtual bool failed() const = 0;

protected:
    NetRoundGate() = default;
};

}  // namespace bomber::game
