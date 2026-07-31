#include "bomber/net/udp_transport.hpp"

#include <array>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

// Raw-socket UDP transport. All the OS-specific spelling (SOCKET vs int fd,
// closesocket vs close, ioctlsocket vs fcntl, one-time WSAStartup) is confined
// to this translation unit behind the small shims below; the rest of libs/net
// never sees a socket header.

namespace bomber::net {

namespace {

#ifdef _WIN32
using sock_t = SOCKET;
const sock_t kInvalid = INVALID_SOCKET;
void ensure_started() {
    struct WsaGuard {
        WsaGuard() {
            WSADATA d;
            WSAStartup(MAKEWORD(2, 2), &d);
        }
        ~WsaGuard() { WSACleanup(); }
        WsaGuard(const WsaGuard&) = delete;
        WsaGuard& operator=(const WsaGuard&) = delete;
        WsaGuard(WsaGuard&&) = delete;
        WsaGuard& operator=(WsaGuard&&) = delete;
    };
    static WsaGuard guard;  // one WSAStartup per process; WSACleanup at exit
    (void)guard;
}
void set_nonblocking(sock_t s) {
    u_long mode = 1;
    ioctlsocket(s, static_cast<long>(FIONBIO), &mode);
}
void close_sock(sock_t s) { closesocket(s); }
#else
using sock_t = int;
constexpr sock_t kInvalid = -1;
void ensure_started() {}
void set_nonblocking(sock_t s) {
    const int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
}
void close_sock(sock_t s) { ::close(s); }
#endif

// Every datagram this component exchanges is far below this; the largest is a
// SetupChunk at 1039 bytes (protocol.hpp), deliberately under the ~1200-byte
// safe UDP payload. A longer one is truncated by recvfrom and then rejected by
// decode()'s exact-length framing, which is the right answer for a peer that
// disagrees with us about the protocol.
constexpr std::size_t kRecvBufferBytes = 2048;

// The sender of one received datagram, kept as a pair because recvfrom fills the
// address and its length together and neither is meaningful alone.
struct DatagramSource {
    sockaddr_storage addr{};
    socklen_t len = 0;
};

// ONE non-blocking receive, shared by poll() and poll_from() — they differ only
// in what they do with the SOURCE afterwards (poll auto-learns the peer,
// poll_from reports it and deliberately does not), and that difference is the
// whole reason they are two functions. The recvfrom call, the buffer and the
// "n <= 0 means EWOULDBLOCK or an error" reading were duplicated verbatim.
bool recv_datagram(sock_t s, std::vector<std::uint8_t>* out, DatagramSource* src) {
    // Deliberately NOT value-initialised: recvfrom fills it and only the `n`
    // bytes it reports are ever read, so zeroing 2 KB on every poll of every pump
    // would be pure cost. This is the one hot path in the file.
    std::array<char, kRecvBufferBytes> buf;
    src->len = static_cast<socklen_t>(sizeof(src->addr));
    const int n = static_cast<int>(recvfrom(s, buf.data(), static_cast<int>(buf.size()), 0,
                                            reinterpret_cast<sockaddr*>(&src->addr), &src->len));
    if (n <= 0) return false;  // EWOULDBLOCK (nothing waiting) or an error
    const auto* p = reinterpret_cast<const std::uint8_t*>(buf.data());
    out->assign(p, p + n);
    return true;
}

// Render an IPv4 sockaddr's address as dotted quad, or "" if it cannot be.
std::string ipv4_text(const sockaddr_in& a) {
    std::array<char, INET_ADDRSTRLEN> text{};
    if (inet_ntop(AF_INET, &a.sin_addr, text.data(), text.size()) == nullptr) return {};
    return text.data();
}

// Resolve host:port to an IPv4 sockaddr_in — a dotted literal via inet_pton
// (no DNS), else a hostname via getaddrinfo. Shared by set_peer()/send_to().
bool resolve_ipv4(const std::string& host, std::uint16_t port, sockaddr_in* out) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || res == nullptr) return false;
        a.sin_addr = reinterpret_cast<const sockaddr_in*>(res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }
    *out = a;
    return true;
}

}  // namespace

std::string local_ip_toward(const std::string& host, std::uint16_t port) {
    ensure_started();
    sockaddr_in dst{};
    if (!resolve_ipv4(host, port, &dst)) return {};
    const sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kInvalid) return {};
    std::string result;
    // UDP connect() transmits nothing — it just binds the route, after which
    // getsockname() reports the interface address that would carry the traffic.
    if (connect(s, reinterpret_cast<const sockaddr*>(&dst), sizeof(dst)) == 0) {
        sockaddr_in local{};
        socklen_t len = sizeof(local);
        if (getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0)
            result = ipv4_text(local);
    }
    close_sock(s);
    return result;
}

UdpTransport::~UdpTransport() { close_fd(); }

void UdpTransport::close_fd() {
    if (fd_ >= 0) {
        close_sock(static_cast<sock_t>(fd_));
        fd_ = -1;
    }
}

bool UdpTransport::bind(std::uint16_t local_port) {
    ensure_started();
    close_fd();
    const sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kInvalid) return false;

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(local_port);
    if (::bind(s, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
        close_sock(s);
        return false;
    }
    set_nonblocking(s);
    fd_ = static_cast<std::int64_t>(s);
    return true;
}

bool UdpTransport::set_peer(const std::string& peer_host, std::uint16_t peer_port) {
    sockaddr_in peer{};
    if (!resolve_ipv4(peer_host, peer_port, &peer)) return false;
    static_assert(sizeof(peer) <= 128, "peer_ must hold a resolved sockaddr_in");
    std::memcpy(peer_.data(), &peer, sizeof(peer));
    peer_len_ = sizeof(peer);
    return true;
}

std::uint16_t UdpTransport::local_port() const {
    if (fd_ < 0) return 0;
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (getsockname(static_cast<sock_t>(fd_), reinterpret_cast<sockaddr*>(&addr), &len) != 0)
        return 0;
    return ntohs(addr.sin_port);
}

void UdpTransport::send(const std::uint8_t* data, std::size_t size) {
    if (fd_ < 0 || peer_len_ == 0) return;
    sendto(static_cast<sock_t>(fd_), reinterpret_cast<const char*>(data), static_cast<int>(size), 0,
           reinterpret_cast<const sockaddr*>(peer_.data()), static_cast<socklen_t>(peer_len_));
}

bool UdpTransport::poll(std::vector<std::uint8_t>* out) {
    if (fd_ < 0) return false;
    DatagramSource src;
    if (!recv_datagram(static_cast<sock_t>(fd_), out, &src)) return false;
    // Learn the peer from the first datagram we receive when none was set: a host
    // binds a known port and waits, and the guest (which set_peer'd the host)
    // reveals its own address by connecting — so only the JOINER needs to know an
    // address, the usual host/join UX.
    if (peer_len_ == 0 && src.len > 0 && static_cast<std::size_t>(src.len) <= peer_.size()) {
        std::memcpy(peer_.data(), &src.addr, static_cast<std::size_t>(src.len));
        peer_len_ = static_cast<unsigned>(src.len);
    }
    return true;
}

void UdpTransport::send_to(const std::string& host, std::uint16_t port, const std::uint8_t* data,
                           std::size_t size) {
    if (fd_ < 0) return;
    sockaddr_in dst{};
    if (!resolve_ipv4(host, port, &dst)) return;
    sendto(static_cast<sock_t>(fd_), reinterpret_cast<const char*>(data), static_cast<int>(size), 0,
           reinterpret_cast<const sockaddr*>(&dst), sizeof(dst));
}

bool UdpTransport::poll_from(std::vector<std::uint8_t>* out, std::string* src_ip,
                             std::uint16_t* src_port) {
    if (fd_ < 0) return false;
    DatagramSource src;
    if (!recv_datagram(static_cast<sock_t>(fd_), out, &src)) return false;
    // Report the source (IPv4 only) — but do NOT auto-learn the peer: the punch
    // decides which candidate wins and set_peer()s it explicitly.
    if (src.addr.ss_family != AF_INET) {
        if (src_ip) src_ip->clear();
        if (src_port) *src_port = 0;
        return true;
    }
    const auto* a = reinterpret_cast<const sockaddr_in*>(&src.addr);
    if (src_ip) *src_ip = ipv4_text(*a);
    if (src_port) *src_port = ntohs(a->sin_port);
    return true;
}

}  // namespace bomber::net
