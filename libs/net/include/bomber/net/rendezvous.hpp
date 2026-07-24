#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "bomber/net/udp_transport.hpp"

// The NAT hole-punch (ADR-0011 §3, docs/online-multiplayer-design.md §3): given
// the peer's candidate addresses (from the lobby's candidate exchange), open a
// direct UDP path by punching simultaneously — both peers send a PING out their
// own socket (which opens their NAT binding) so the other's PING/PONG is allowed
// back through. The first candidate whose PING→PONG round completes wins and is
// set as the transport's fixed peer; the round-trip is the first RTT sample the
// caller feeds to the rollback window.
//
// Pump-based like SeedHandshake, and CLOCK-INJECTED: step(now_ms) takes the
// caller's monotonic clock (SDL_GetTicks in the GUI, a synthetic counter in
// tests) so libs/net stays clock-free and the punch is deterministically
// testable. Data plane only — knows nothing of the signaling server; the caller
// (LobbyClient/Rendezvous glue) supplies the candidate list. IPv4.
namespace bomber::net {

class Rendezvous {
public:
    struct Candidate {
        std::string host;
        std::uint16_t port = 0;
    };

    // `transport` must already be bound(). `peer_candidates` are the addresses to
    // punch toward. `nonce` tags OUR pings (any nonzero value unique to this
    // attempt — e.g. from the lobby). `timeout_ms` gives up (→ failed()).
    Rendezvous(UdpTransport& transport, std::vector<Candidate> peer_candidates,
               std::uint32_t nonce, int timeout_ms = 3000);

    // Pump once against the caller's monotonic clock: (re)send pings on an
    // interval, drain inbound (echo pings as pongs, latch a pong to OUR nonce as
    // the winner), and time out. Safe to call every frame.
    void step(std::int64_t now_ms);

    bool done() const { return state_ != State::Punching; }
    bool connected() const { return state_ == State::Connected; }
    bool failed() const { return state_ == State::Failed; }
    // RTT of the winning path in ms (0 until connected()).
    int rtt_ms() const { return rtt_ms_; }
    // The chosen peer address (post-NAT source), or nullptr until connected().
    const Candidate* winner() const { return state_ == State::Connected ? &winner_ : nullptr; }

private:
    enum class State : std::uint8_t { Punching, Connected, Failed };

    void send_pings(std::int64_t now_ms);

    UdpTransport& transport_;
    std::vector<Candidate> candidates_;
    std::uint32_t nonce_;
    int timeout_ms_;
    State state_ = State::Punching;
    std::int64_t start_ms_ = -1;      // first step() timestamp
    std::int64_t last_ping_ms_ = -1;  // most recent ping round (for RTT + interval)
    int rtt_ms_ = 0;
    Candidate winner_;
};

}  // namespace bomber::net
