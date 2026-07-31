#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "bomber/net/transport.hpp"
#include "bomber/net/udp_transport.hpp"

// The TURN-like RELAY fallback (ADR-0011 decision 3, design §4): when the hole
// punch fails — symmetric NAT / CGNAT, where the port the STUN server saw is not
// the port used toward the peer — the peers route through a public-IP forwarder
// on the matchmaker instead.
//
// Deliberately just another `Transport`: the session stacked on top is
// byte-for-byte identical direct or relayed. The relay never decodes the payload
// (the server never simulates), only the FROZEN routing header this class
// prepends — docs/net-wire-format.md, PROTOCOL.md §6.
namespace bomber::net {

inline constexpr std::size_t kRelayAllocIdBytes = 16;
inline constexpr std::size_t kRelayHeaderBytes = kRelayAllocIdBytes + 1;

// Decode the server's 32-hex-char alloc_id into its 16 raw bytes. Returns false
// on a wrong length or a non-hex digit (untrusted input).
bool parse_alloc_id(const std::string& hex, std::array<std::uint8_t, kRelayAllocIdBytes>* out);

class RelayedTransport : public Transport {
public:
    // Borrows the SAME bound UdpTransport the punch used (its NAT binding is
    // already open, and the relay learns our public address from the first
    // datagram). `dst_seat` is the peer we are talking to; for the N>2 star that
    // is the hub seat.
    RelayedTransport(UdpTransport& socket, std::string relay_host, std::uint16_t relay_port,
                     std::array<std::uint8_t, kRelayAllocIdBytes> alloc_id, int dst_seat);

    void send(const std::uint8_t* data, std::size_t size) override;
    bool poll(std::vector<std::uint8_t>* out) override;
    // Every datagram of this match is forwarded by the matchmaker — so, unlike a
    // direct match, the server IS in the path and its health is a live suspect.
    NetPath path() const override { return NetPath::Relayed; }

    // The sender seat off the header of the datagram most recently returned by
    // poll(); -1 before any. DIAGNOSTIC ONLY: a relay is TWO SEATS by
    // construction (this class addresses exactly one destination, so
    // LobbyFlow::can_relay() refuses the fallback for a star), and a 2-seat peer
    // already knows who the sender is. Kept because naming the far seat is worth
    // having when a relayed match is read out of a log.
    int last_src_seat() const { return last_src_seat_; }

private:
    bool addressed_to_us(const std::vector<std::uint8_t>& buf) const;

    UdpTransport& socket_;
    std::string relay_host_;
    std::uint16_t relay_port_;
    std::array<std::uint8_t, kRelayAllocIdBytes> alloc_id_;
    int dst_seat_;
    int last_src_seat_ = -1;
    std::vector<std::uint8_t> scratch_;  // reused send buffer (header + payload)
};

}  // namespace bomber::net
