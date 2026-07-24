#pragma once

#include <cstdint>

#include "bomber/net/transport.hpp"

// The pre-match SEED HANDSHAKE (ADR-0010 §3.3 step 5 "lobby/session UI"): a
// tiny pump-based exchange that lets the HOST pick the shared match seed and
// the GUEST adopt it, so a menu-driven host/join flow needs no --seed on either
// command line (the CLI path still carries --seed and skips this). It rides the
// SAME Transport the lockstep session uses and speaks one message — HelloFrame
// (protocol.hpp) — so it works over both the real UdpTransport and the headless
// LoopbackLink.
//
// Convergence (both re-send every step(), so a dropped datagram self-heals):
//   HOST  keeps sending Hello{seed=host_seed, is_ack=false} and finishes the
//         instant it receives an ACK (is_ack=true) — that datagram also teaches
//         UdpTransport the guest's address (poll() learns the sender), so a host
//         that only bind()s a known port needs no peer address up front.
//   GUEST keeps sending Hello{seed=0, is_ack=true} and finishes the instant it
//         receives the host's announcement (is_ack=false), adopting seed=f.seed.
// After done() both peers agree on seed(): the host's own host_seed, the guest's
// adopted copy.

namespace bomber::net {

class SeedHandshake {
public:
    // `t` is BORROWED and must outlive the handshake (the caller owns the socket
    // and hands the SAME transport on to the LockstepSession afterwards).
    // host_seed is meaningful only when is_host — the guest ignores it and adopts
    // the host's announced seed instead.
    SeedHandshake(Transport& t, bool is_host, std::uint32_t host_seed)
        : transport_(&t), is_host_(is_host), seed_(is_host ? host_seed : 0) {}

    // Send our datagram once and drain everything that arrived; latches done_
    // when the peer's reply is seen. Call once per frame/pump until done().
    void step();

    bool done() const { return done_; }
    // The agreed seed: host_seed for the host; the adopted value for the guest
    // (0 until the host's announcement is received, i.e. until done()).
    std::uint32_t seed() const { return seed_; }

private:
    Transport* transport_;
    bool is_host_;
    std::uint32_t seed_;
    bool done_ = false;
};

}  // namespace bomber::net
