#include "bomber/net/star_hub_transport.hpp"

#include <utility>

namespace bomber::net {

StarHubTransport::StarHubTransport(UdpTransport& socket, std::vector<Guest> guests)
    : socket_(socket), guests_(std::move(guests)) {}

int StarHubTransport::guest_index(const std::string& ip, std::uint16_t port) const {
    for (std::size_t i = 0; i < guests_.size(); ++i)
        if (guests_[i].port == port && guests_[i].host == ip) return static_cast<int>(i);
    return -1;
}

void StarHubTransport::send(const std::uint8_t* data, std::size_t size) {
    for (const Guest& g : guests_) socket_.send_to(g.host, g.port, data, size);
}

void StarHubTransport::reflect_to_others(int from, const std::vector<std::uint8_t>& buf) {
    // Verbatim: the guests exchange inputs only through us, and the bytes stay
    // opaque — the hub is a forwarder, not an authority.
    for (std::size_t i = 0; i < guests_.size(); ++i) {
        if (static_cast<int>(i) == from) continue;
        socket_.send_to(guests_[i].host, guests_[i].port, buf.data(), buf.size());
    }
}

bool StarHubTransport::poll(std::vector<std::uint8_t>* out) {
    std::vector<std::uint8_t> buf;
    std::string ip;
    std::uint16_t port = 0;
    while (socket_.poll_from(&buf, &ip, &port)) {
        const int from = guest_index(ip, port);
        if (from < 0) continue;  // not a seat in this match — drop, never reflect
        reflect_to_others(from, buf);
        *out = std::move(buf);
        return true;
    }
    return false;
}

}  // namespace bomber::net
