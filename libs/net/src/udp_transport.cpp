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

}  // namespace

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
    peer.sin_family = AF_INET;
    peer.sin_port = htons(peer_port);
    if (inet_pton(AF_INET, peer_host.c_str(), &peer.sin_addr) != 1) {
        // Not a dotted-IPv4 literal — resolve it as a hostname.
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(peer_host.c_str(), nullptr, &hints, &res) != 0 || res == nullptr)
            return false;
        peer.sin_addr = reinterpret_cast<const sockaddr_in*>(res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }
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

}  // namespace bomber::net
