#pragma once

#include <cstdint>
#include <vector>

#include "bomber/net/transport.hpp"

// PATH VERIFICATION — the mutual half of the connect step (ADR-0011 §3/§4).
//
// A punch proves the path to whoever RECEIVED the pong and nothing more, so two
// peers can reach opposite conclusions and end up on different transports. That
// shipped, and it cost a live match; docs/online-multiplayer-design.md §4.1 has
// the incident and the convergence argument.
//
// THE PROTOCOL is one byte of state (protocol.hpp's ProbeFrame):
//
//   * receiving a probe             => peer->me works       (`peer_seen_`)
//   * receiving one with seen_peer  => me->peer works too   (`peer_saw_us_`)
//
// Neither end can derive the second fact alone, which is precisely the fact the
// punch never established. Both together are `verified()`.
//
// AND IT ANSWERS PUNCHES. While verifying, this echoes any hole-punch PING it
// sees, so a peer that is still punching keeps getting its PONGs instead of
// being starved by our silence.
//
// Transport-agnostic on purpose: the same object verifies a punched socket and a
// relay allocation, so `expired()` means "this path does not carry" whichever
// path it is, and the caller escalates. Pump-based and clock-injected.
namespace bomber::net {

// One decoded datagram (protocol.hpp), named here only by reference.
struct Message;

// Probe cadence. Fast enough that the exchange finishes inside a frame or two of
// a punched RTT, sparse enough to be invisible next to a 20 Hz input stream.
inline constexpr std::int64_t kProbeIntervalMs = 100;
// How long to keep probing AFTER mutual proof. The peer that completes the
// exchange last cannot know its own final probe arrived — the two-generals shape
// TCP answers with TIME_WAIT — so lingering is what makes the other end's
// completion independent of our leaving. A fifth of a second on a screen that
// already says "CONNECTING TO PLAYERS...".
inline constexpr std::int64_t kProbeLingerMs = 200;

class LinkProbe {
public:
    // `t` is BORROWED and must outlive this object. `local_nonce` tags our own
    // probes so a reflected copy is recognised (the star hub reflects) — use the
    // same per-seat nonce the punch used. `deadline_ms` is measured from the
    // FIRST step(): reaching it without mutual proof is expired().
    LinkProbe(Transport& t, std::uint32_t local_nonce, int deadline_ms);

    // Drain inbound, answer what needs answering, re-send on the interval, and
    // latch verified()/expired(). Safe to call every frame; a no-op once either
    // outcome has latched.
    void step(std::int64_t now_ms);

    // Both directions proven AND the linger served: the path carries and the
    // peer knows it. Handing the transport on is safe from here.
    bool verified() const { return verified_; }
    // The deadline passed with no mutual proof. The caller escalates (direct →
    // relay) or gives up (already relayed).
    bool expired() const { return expired_; }
    // Diagnostics: anything at all arrived from the peer on this path.
    bool peer_seen() const { return peer_seen_; }

private:
    void send_probe(std::int64_t now_ms);
    void on_message(const Message& msg, std::int64_t now_ms);
    // Decide verified()/expired() from this pump's evidence.
    void latch_outcome(std::int64_t now_ms);

    Transport* transport_;
    std::vector<std::uint8_t> buf_;   // reused drain buffer
    std::int64_t start_ms_ = -1;      // first step()
    std::int64_t last_send_ms_ = -1;  // most recent probe (interval base)
    std::int64_t mutual_ms_ = -1;     // when both directions were first proven
    std::uint32_t nonce_;
    int deadline_ms_;
    bool peer_seen_ = false;    // a probe from the peer arrived: peer→me works
    bool peer_saw_us_ = false;  // it said seen_peer: me→peer works
    bool verified_ = false;
    bool expired_ = false;
};

}  // namespace bomber::net
