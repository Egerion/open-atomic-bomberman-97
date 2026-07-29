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
// the port used toward the peer — the peers route their datagrams through a
// public-IP forwarder on the matchmaker instead.
//
// This is deliberately just another `Transport`: the RollbackSession stacked on
// top is byte-for-byte identical whether it runs direct or relayed, so the
// fallback is a one-object swap decided by the Rendezvous outcome. The relay
// never decodes the payload (ADR-0011: the server never simulates) — it only
// reads the routing header this class prepends.
//
// FROZEN wire format (services/matchmaker/PROTOCOL.md §6), both directions:
//
//     [16 bytes alloc_id (binary)][1 byte seat][opaque payload …]
//
// Outbound the alloc_id is OURS and the seat is the DESTINATION; inbound the
// alloc_id is ours again and the seat is the SENDER's — so a receiver always
// strips the same fixed 17-byte prefix.
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

    // The seat that sent the datagram most recently returned by poll() — the
    // N-player star reads this to attribute inputs. -1 before any datagram.
    int last_src_seat() const { return last_src_seat_; }

private:
    UdpTransport& socket_;
    std::string relay_host_;
    std::uint16_t relay_port_;
    std::array<std::uint8_t, kRelayAllocIdBytes> alloc_id_;
    int dst_seat_;
    int last_src_seat_ = -1;
    std::vector<std::uint8_t> scratch_;  // reused send buffer (header + payload)
};

}  // namespace bomber::net
