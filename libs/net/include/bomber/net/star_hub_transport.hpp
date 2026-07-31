#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "bomber/net/transport.hpp"
#include "bomber/net/udp_transport.hpp"

// The host-relay STAR for matches with more than two seats (ADR-0011 decision 2
// + decision 4: up to 10 seats). One peer — the lobby host — is the INPUT HUB:
// every guest exchanges datagrams only with the hub, and the hub REFLECTS each
// guest's datagram to the other guests. Packet count and NAT punches are then
// LINEAR in seats (N−1 hub↔guest paths) instead of quadratic (a full mesh would
// need N·(N−1)/2 paths, each with its own punch and possible relay).
//
// This is a TRANSPORT star, NOT a simulation authority: every peer still runs
// the identical deterministic sim and confirms a tick only when all seats are
// known. The hub forwards opaque bytes — it never decodes an input frame, and it
// has no say in the outcome.
//
// Only the HUB uses this class. A guest needs nothing new: its plain
// UdpTransport already points at exactly one peer (the hub), and because the hub
// reflects, the guest still receives every other seat's input frames. The
// RollbackSession above is unchanged either way — it already accepts arbitrary
// local_seats / all_seats masks, so N>2 needs no session change at all.
namespace bomber::net {

class StarHubTransport : public Transport {
public:
    struct Guest {
        std::string host;
        std::uint16_t port = 0;
    };

    // `socket` is the hub's own bound UdpTransport (the one the punches used);
    // `guests` are the punched addresses of every other seat.
    StarHubTransport(UdpTransport& socket, std::vector<Guest> guests);

    // Fan the hub's own datagram out to every guest.
    void send(const std::uint8_t* data, std::size_t size) override;

    // Drain one inbound datagram: reflect it to the OTHER guests (so they learn
    // this seat's inputs without talking to each other), then hand it to the
    // hub's own session. Datagrams from an address that is not a known guest are
    // dropped — untrusted input, and reflecting them would let an outsider
    // inject frames into the match.
    bool poll(std::vector<std::uint8_t>* out) override;

    // This machine is the hub: every guest's traffic crosses THIS uplink twice
    // (in, then reflected out to the others), which is a bottleneck no
    // server-side log can see. Worth knowing before blaming the network.
    NetPath path() const override { return NetPath::StarHub; }

    std::size_t guest_count() const { return guests_.size(); }

private:
    int guest_index(const std::string& ip, std::uint16_t port) const;
    void reflect_to_others(int from, const std::vector<std::uint8_t>& buf);

    UdpTransport& socket_;
    std::vector<Guest> guests_;
};

}  // namespace bomber::net
