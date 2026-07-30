#include "bomber/net/migrating_transport.hpp"

namespace bomber::net {

void MigratingTransport::send(const std::uint8_t* data, std::size_t size) {
    if (target_ == nullptr) {
        ++dropped_sends_;  // no path: the old hub is a corpse and the new one is not punched yet
        return;
    }
    target_->send(data, size);
}

bool MigratingTransport::poll(std::vector<std::uint8_t>* out) {
    return target_ != nullptr && target_->poll(out);
}

}  // namespace bomber::net
