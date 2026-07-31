#include "bomber/net/relayed_transport.hpp"

#include <algorithm>
#include <utility>

namespace bomber::net {

namespace {

// One hex digit → 0..15, or -1 if it is not a hex digit.
constexpr int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

bool parse_alloc_id(const std::string& hex, std::array<std::uint8_t, kRelayAllocIdBytes>* out) {
    if (hex.size() != kRelayAllocIdBytes * 2) return false;
    for (std::size_t i = 0; i < kRelayAllocIdBytes; ++i) {
        const int hi = hex_digit(hex[i * 2]);
        const int lo = hex_digit(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        if (out) (*out)[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return true;
}

RelayedTransport::RelayedTransport(UdpTransport& socket, std::string relay_host,
                                   std::uint16_t relay_port,
                                   std::array<std::uint8_t, kRelayAllocIdBytes> alloc_id,
                                   int dst_seat)
    : socket_(socket),
      relay_host_(std::move(relay_host)),
      relay_port_(relay_port),
      alloc_id_(alloc_id),
      dst_seat_(dst_seat) {}

void RelayedTransport::send(const std::uint8_t* data, std::size_t size) {
    scratch_.clear();
    scratch_.reserve(kRelayHeaderBytes + size);
    scratch_.insert(scratch_.end(), alloc_id_.begin(), alloc_id_.end());
    scratch_.push_back(static_cast<std::uint8_t>(dst_seat_));
    scratch_.insert(scratch_.end(), data, data + size);
    socket_.send_to(relay_host_, relay_port_, scratch_.data(), scratch_.size());
}

bool RelayedTransport::addressed_to_us(const std::vector<std::uint8_t>& buf) const {
    // Untrusted input: anything too short to carry the routing header, or
    // addressed to a different allocation, is dropped rather than decoded.
    if (buf.size() <= kRelayHeaderBytes) return false;
    return std::equal(alloc_id_.begin(), alloc_id_.end(), buf.begin());
}

bool RelayedTransport::poll(std::vector<std::uint8_t>* out) {
    std::vector<std::uint8_t> buf;
    while (socket_.poll_from(&buf, nullptr, nullptr)) {
        if (!addressed_to_us(buf)) continue;
        last_src_seat_ = static_cast<int>(buf[kRelayAllocIdBytes]);
        out->assign(buf.begin() + static_cast<std::ptrdiff_t>(kRelayHeaderBytes), buf.end());
        return true;
    }
    return false;
}

}  // namespace bomber::net
