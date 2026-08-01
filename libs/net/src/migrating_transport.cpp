#include "bomber/net/migrating_transport.hpp"

namespace bomber::net {

void MigratingTransport::send(const std::uint8_t* data, std::size_t size) {
    // No path: the old hub is a corpse and the new one is not punched yet. The
    // datagram is dropped rather than aimed at an address that cannot answer.
    if (target_ == nullptr) return;
    target_->send(data, size);
}

bool MigratingTransport::poll(std::vector<std::uint8_t>* out) {
    if (target_ == nullptr || !target_->poll(out)) return false;
    ++rx_polled_;
    return true;
}

}  // namespace bomber::net
