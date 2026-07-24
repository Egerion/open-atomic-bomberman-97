#include "bomber/net/udp_transport.hpp"

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
        if (getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0) {
            char ip[INET_ADDRSTRLEN] = {};
            if (inet_ntop(AF_INET, &local.sin_addr, ip, sizeof(ip)) != nullptr) result = ip;
        }
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
    std::memcpy(peer_, &peer, sizeof(peer));
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
           reinterpret_cast<const sockaddr*>(peer_), static_cast<socklen_t>(peer_len_));
}

bool UdpTransport::poll(std::vector<std::uint8_t>* out) {
    if (fd_ < 0) return false;
    char buf[2048];
    sockaddr_storage from{};
    socklen_t fromlen = sizeof(from);
    const int n = static_cast<int>(recvfrom(static_cast<sock_t>(fd_), buf,
                                            static_cast<int>(sizeof(buf)), 0,
                                            reinterpret_cast<sockaddr*>(&from), &fromlen));
    if (n <= 0) return false;  // EWOULDBLOCK (nothing waiting) or an error
    // Learn the peer from the first datagram we receive when none was set: a host
    // binds a known port and waits, and the guest (which set_peer'd the host)
    // reveals its own address by connecting — so only the JOINER needs to know an
    // address, the usual host/join UX.
    if (peer_len_ == 0 && fromlen > 0 && static_cast<std::size_t>(fromlen) <= sizeof(peer_)) {
        std::memcpy(peer_, &from, static_cast<std::size_t>(fromlen));
        peer_len_ = static_cast<unsigned>(fromlen);
    }
    const auto* p = reinterpret_cast<const std::uint8_t*>(buf);
    out->assign(p, p + n);
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
    char buf[2048];
    sockaddr_storage from{};
    socklen_t fromlen = sizeof(from);
    const int n = static_cast<int>(recvfrom(static_cast<sock_t>(fd_), buf,
                                            static_cast<int>(sizeof(buf)), 0,
                                            reinterpret_cast<sockaddr*>(&from), &fromlen));
    if (n <= 0) return false;
    // Report the source (IPv4 only) — but do NOT auto-learn the peer: the punch
    // decides which candidate wins and set_peer()s it explicitly.
    if (from.ss_family == AF_INET) {
        const auto* a = reinterpret_cast<const sockaddr_in*>(&from);
        if (src_ip) {
            char ip[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &a->sin_addr, ip, sizeof(ip));
            *src_ip = ip;
        }
        if (src_port) *src_port = ntohs(a->sin_port);
    } else {
        if (src_ip) src_ip->clear();
        if (src_port) *src_port = 0;
    }
    const auto* p = reinterpret_cast<const std::uint8_t*>(buf);
    out->assign(p, p + n);
    return true;
}

}  // namespace bomber::net
