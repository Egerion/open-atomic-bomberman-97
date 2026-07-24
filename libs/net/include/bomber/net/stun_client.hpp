#pragma once

#include <cstdint>
#include <string>

#include "bomber/net/udp_transport.hpp"

// STUN-style reflexive-address discovery (ADR-0011 §3 / PROTOCOL.md §2): ask the
// matchmaker's UDP port what source address it sees us as, i.e. our public
// ip:port AFTER the NAT rewrite. That address is the "reflexive" candidate the
// far peer punches toward.
//
// CRITICAL: the probe must leave the SAME socket the match will use, so the NAT
// binding the server observes is the one the peer will actually hit — hence the
// borrowed UdpTransport rather than a private socket of our own.
//
// Pump-based and CLOCK-INJECTED like Rendezvous (step(now_ms)), so libs/net stays
// clock-free. Runs BEFORE the punch (gather → exchange → punch), so it and
// Rendezvous never contend for the socket; a stray non-STUN datagram arriving
// during this phase is dropped (nothing else is flowing yet).
namespace bomber::net {

class StunClient {
public:
    // `transport` must already be bound(). `nonce` is an opaque token echoed back
    // to match reply-to-probe (any per-attempt-unique string).
    StunClient(UdpTransport& transport, std::string server_host, std::uint16_t server_port,
               std::string nonce, int timeout_ms = 2000);

    // Pump once: (re)send the probe on an interval, drain inbound, latch a
    // StunReply carrying OUR nonce, and time out. Safe to call every frame.
    void step(std::int64_t now_ms);

    bool done() const { return state_ != State::Probing; }
    bool ok() const { return state_ == State::Done; }
    bool failed() const { return state_ == State::Failed; }

    // Our reflexive (public, post-NAT) address — valid only once ok().
    const std::string& reflexive_ip() const { return reflexive_ip_; }
    std::uint16_t reflexive_port() const { return reflexive_port_; }
    // "ip:port", the wire form of a rendezvous candidate ("" until ok()).
    std::string reflexive_addr() const;

private:
    enum class State { Probing, Done, Failed };

    UdpTransport& transport_;
    std::string server_host_;
    std::uint16_t server_port_;
    std::string nonce_;
    int timeout_ms_;
    State state_ = State::Probing;
    std::int64_t start_ms_ = -1;
    std::int64_t last_probe_ms_ = -1;
    std::string reflexive_ip_;
    std::uint16_t reflexive_port_ = 0;
};

}  // namespace bomber::net
