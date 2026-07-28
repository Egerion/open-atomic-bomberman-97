#pragma once

#include <cstdint>
#include <vector>

#include "bomber/net/transport.hpp"

// PATH VERIFICATION — the mutual half of the connect step (ADR-0011 §3/§4).
//
// WHAT WENT WRONG WITHOUT IT. `Rendezvous`'s 2-peer form declares Connected on
// receiving the PONG for its own PING. That is a real round-trip proof FOR THE
// PEER THAT RECEIVED IT and nothing more: it says nothing about whether the
// other peer completed its own round trip before kPunchTimeoutMs expired. Worse,
// the winner then goes SILENT — LobbyFlow stops pumping the punch once it is
// connected and Rendezvous::step() returns early when it is not Punching — so
// the peer's still-in-flight PINGs stop being echoed and its failure becomes
// self-fulfilling. One peer then played direct while the other asked the
// matchmaker for a relay allocation the first peer never made, and every
// datagram it pushed into the relay was discarded for want of a destination
// (`drop_unknown_dst` in services/matchmaker/internal/relay/relay.go). The match
// connected and delivered nothing.
//
// The multi-peer form of Rendezvous already required BOTH halves (a peer's own
// PING seen, plus a PONG back from that same address). The 2-peer form's
// single-sided latch was the anomaly, and this class is the general answer to
// it: whatever named the path, the two ends must AGREE on it before the match
// layer is handed anything.
//
// THE PROTOCOL is one byte of state (protocol.hpp's ProbeFrame):
//
//   * receiving a probe            ⇒ peer→me works        (`peer_seen_`)
//   * receiving one with seen_peer ⇒ me→peer works too    (`peer_saw_us_`)
//
// Neither end can derive the second fact alone, which is precisely the fact the
// punch never established. Both together are `verified()`.
//
// AND IT ANSWERS PUNCHES. While verifying the direct path this echoes any
// hole-punch PING it sees, so a peer that is still punching keeps getting its
// PONGs instead of being starved by our silence. That alone converts most of the
// production failure into a plain successful punch on both sides.
//
// LINGER. The peer that completes the exchange last cannot know its own final
// probe arrived — the two-generals shape TCP answers with TIME_WAIT. So after
// mutual proof this keeps probing for kProbeLingerMs before reporting
// verified(), which is what makes the OTHER end's completion independent of our
// leaving. It costs a fifth of a second on a screen that already says
// "CONNECTING TO PLAYERS...", and it is the only cost this file adds to a match
// that punches cleanly.
//
// Pump-based and CLOCK-INJECTED like Rendezvous / StunClient / LobbyFlow, and
// transport-agnostic on purpose: the same object verifies a punched socket and a
// relay allocation, so `expired()` means "this path does not carry" whichever
// path it is, and the caller escalates. Rides the abstract Transport, so nothing
// about WHICH transport delivers reaches the session above it.
namespace bomber::net {

// Probe cadence. Fast enough that the exchange finishes inside a frame or two of
// a punched RTT, sparse enough to be invisible next to a 20 Hz input stream.
inline constexpr std::int64_t kProbeIntervalMs = 100;
// How long to keep probing after mutual proof, so the peer's own completion does
// not depend on our silence (see LINGER above).
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
