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

    // One peer to punch toward, for the MULTI-peer form below: the seat it owns,
    // the nonce ITS pings carry (derived identically on every peer from the shared
    // seed, so an inbound ping identifies its sender), and its candidate
    // addresses.
    struct PeerSpec {
        int seat = -1;
        std::uint32_t nonce = 0;
        std::vector<Candidate> candidates;
    };

    // A completed path to one peer.
    struct Winner {
        int seat = -1;
        Candidate addr;  // the peer's post-NAT source address
        int rtt_ms = 0;
    };

    // 2-PEER form (the direct pair). `transport` must already be bound().
    // `peer_candidates` are the addresses to punch toward. `nonce` tags OUR pings
    // (any nonzero value unique to this attempt). `timeout_ms` gives up
    // (→ failed()). On success the transport's fixed peer is set for you.
    Rendezvous(UdpTransport& transport, std::vector<Candidate> peer_candidates,
               std::uint32_t nonce, int timeout_ms = 3000);

    // MULTI-PEER form — the star hub, which must open a path to EVERY guest over
    // the ONE socket whose NAT binding the guests punched (ADR-0011 decisions 2+4).
    // Running several 2-peer Rendezvous on one socket would be wrong: each polls
    // the same socket and would consume — and drop — datagrams meant for the
    // others. So one object tracks them all.
    //
    // A peer counts as connected only when BOTH halves of the round are seen:
    // its PING arrived (which reveals its post-NAT address AND, via the nonce,
    // WHICH SEAT sent it) and a PONG carrying OUR nonce came back from that same
    // address (proving our own direction works). connected() means every peer is
    // done; winners() then names each seat's address. The transport's fixed peer
    // is NOT set in this form — the caller wraps the socket in a
    // StarHubTransport built from winners() instead.
    Rendezvous(UdpTransport& transport, std::vector<PeerSpec> peers, std::uint32_t local_nonce,
               int timeout_ms);

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
    // 2-peer form; for the multi-peer form use winners().
    const Candidate* winner() const { return state_ == State::Connected ? &winner_ : nullptr; }

    // Every completed path, one per peer (multi-peer form). Empty until
    // connected(); in the 2-peer form it holds the single winner.
    const std::vector<Winner>& winners() const { return winners_; }

private:
    enum class State : std::uint8_t { Punching, Connected, Failed };

    // Per-peer progress in the multi-peer form.
    struct PeerState {
        PeerSpec spec;
        Candidate addr;         // learned from the peer's own ping
        bool addr_known = false;
        bool confirmed = false;  // a pong to OUR nonce came back from addr
        int rtt_ms = 0;
    };

    void send_pings(std::int64_t now_ms);
    void ping_candidates(const std::vector<Candidate>& targets,
                         const std::vector<std::uint8_t>& ping);
    void handle_ping(std::uint32_t nonce, const std::string& ip, std::uint16_t port);
    void handle_pong(std::uint32_t nonce, const std::string& ip, std::uint16_t port,
                     std::int64_t now_ms);
    void finish_if_all_confirmed();

    UdpTransport& transport_;
    std::vector<Candidate> candidates_;  // 2-peer form: the flat target list
    std::uint32_t nonce_;
    int timeout_ms_;
    bool multi_ = false;  // which form this object is in
    std::vector<PeerState> peers_;
    State state_ = State::Punching;
    std::int64_t start_ms_ = -1;      // first step() timestamp
    std::int64_t last_ping_ms_ = -1;  // most recent ping round (for RTT + interval)
    int rtt_ms_ = 0;
    Candidate winner_;
    std::vector<Winner> winners_;
};

}  // namespace bomber::net
