#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "bomber/net/transport.hpp"

// A real UDP-socket Transport (raw OS sockets — winsock on Windows, BSD sockets
// on POSIX; NO SDL, so libs/net stays SDL-free and headless-buildable). Binds a
// local UDP port and aims every datagram at ONE fixed peer — the 2-player
// lockstep link. Non-blocking: poll() returns false when nothing has arrived.
// Owns the socket (RAII); winsock is started once per process and cleaned up at
// exit. Untrusted input like everything else: poll() hands the raw bytes to the
// caller, which decodes + bounds-checks them (protocol.hpp).

namespace bomber::net {

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

    // --- Address-aware I/O for the Rendezvous hole-punch (ADR-0011 §3) ---
    // The punch must probe SEVERAL candidate addresses (host / reflexive / relay)
    // and learn WHICH one answered, neither of which the fixed-peer send()/poll()
    // pair supports. send_to() aims one datagram at an explicit address; poll_from
    // reports the source ip:port and — unlike poll() — does NOT auto-learn the
    // peer, so the punch chooses the winner explicitly (then set_peer()s it, after
    // which the match uses the plain send()/poll() path). IPv4 only.
    void send_to(const std::string& host, std::uint16_t port, const std::uint8_t* data,
                 std::size_t size);
    bool poll_from(std::vector<std::uint8_t>* out, std::string* src_ip, std::uint16_t* src_port);

private:
    void close_fd();

    // The socket handle as a signed 64-bit value so one field holds both a POSIX
    // int fd and a Windows SOCKET; -1 means closed (INVALID_SOCKET maps to -1).
    std::int64_t fd_ = -1;
    // Resolved peer address, stored opaquely (sockaddr_storage-sized, aligned) so
    // this header pulls in no OS socket headers.
    alignas(8) unsigned char peer_[128] = {};
    unsigned peer_len_ = 0;
};

}  // namespace bomber::net
