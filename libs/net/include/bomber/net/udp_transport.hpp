#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "bomber/net/transport.hpp"

// A real UDP-socket Transport over raw OS sockets — winsock on Windows, BSD on
// POSIX, NO SDL, so libs/net stays headless-buildable. Binds a local port and
// aims every datagram at ONE fixed peer. Non-blocking, owns the socket (RAII),
// and hands poll()'s raw bytes to the caller to decode and bounds-check.

namespace bomber::net {

// The local IPv4 address this machine would use to REACH `host` — the "host"
// (LAN) rendezvous candidate. Found by pointing a throwaway UDP socket at the
// destination and reading back getsockname(): connect() on UDP sends nothing and
// only fixes the route, so this costs no traffic and needs no
// interface-enumeration API. "" on failure, and the caller then offers no host
// candidate.
std::string local_ip_toward(const std::string& host, std::uint16_t port);

class UdpTransport : public Transport {
public:
    UdpTransport() = default;
    ~UdpTransport() override;
    UdpTransport(const UdpTransport&) = delete;
    UdpTransport& operator=(const UdpTransport&) = delete;
    UdpTransport(UdpTransport&&) = delete;
    UdpTransport& operator=(UdpTransport&&) = delete;

    // Bind `local_port` (0 = an OS-chosen ephemeral port, read back via
    // local_port()). No peer yet — call set_peer() before send(). Returns false
    // on socket/bind failure (ok() then stays false).
    bool bind(std::uint16_t local_port);

    // Point all future sends at peer_host:peer_port (a dotted-IPv4 address or a
    // hostname). Returns false if the host cannot be resolved.
    bool set_peer(const std::string& peer_host, std::uint16_t peer_port);

    bool ok() const { return fd_ >= 0; }
    bool has_peer() const { return peer_len_ != 0; }  // set via set_peer() or learned in poll()
    std::uint16_t local_port() const;                 // the actually-bound port (after bind())

    void send(const std::uint8_t* data, std::size_t size) override;
    bool poll(std::vector<std::uint8_t>* out) override;
    // A bare socket aimed at one peer: the punched peer-to-peer path. Nothing
    // the matchmaker does can affect a match running over this.
    NetPath path() const override { return NetPath::Direct; }

    // --- address-aware I/O for the Rendezvous hole-punch (ADR-0011 §3) --------
    //
    // The punch probes SEVERAL candidate addresses and must learn WHICH answered,
    // neither of which the fixed-peer pair above supports. Unlike poll(),
    // poll_from() does NOT auto-learn the peer, so the punch chooses its winner
    // explicitly and set_peer()s it. IPv4 only.
    void send_to(const std::string& host, std::uint16_t port, const std::uint8_t* data,
                 std::size_t size);
    bool poll_from(std::vector<std::uint8_t>* out, std::string* src_ip, std::uint16_t* src_port);

private:
    void close_fd();

    // Signed 64-bit so one field holds both a POSIX int fd and a Windows SOCKET;
    // -1 means closed (INVALID_SOCKET maps to -1).
    std::int64_t fd_ = -1;
    // The resolved peer address, held opaquely so this header pulls in no OS
    // socket headers. The alignment is what makes the .cpp's reinterpret_cast to
    // sockaddr well-defined; the size is a sockaddr_storage upper bound, checked
    // against the real type where it is filled.
    alignas(8) std::array<unsigned char, 128> peer_{};
    unsigned peer_len_ = 0;
};

}  // namespace bomber::net
