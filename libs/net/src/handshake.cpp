#include "bomber/net/handshake.hpp"

#include <cstddef>
#include <vector>

#include "bomber/net/protocol.hpp"

namespace bomber::net {

void SeedHandshake::step() {
    // (Re-)broadcast our side every pump: the host announces the seed, the guest
    // acks. Re-sending unconditionally is what makes the exchange loss-tolerant —
    // a dropped datagram is covered by the next step()'s copy.
    const std::vector<std::uint8_t> out =
        is_host_ ? encode_hello(seed_, /*is_ack=*/false)  // HOST: "the seed is X"
                 : encode_hello(0, /*is_ack=*/true);       // GUEST: "ack — send me the seed"
    transport_->send(out.data(), out.size());

    // Drain every datagram waiting on the socket; a non-Hello packet (a stray
    // late lockstep frame) is simply ignored, like every decode elsewhere.
    std::vector<std::uint8_t> buf;
    while (transport_->poll(&buf)) {
        Message m;
        if (!decode(buf.data(), buf.size(), &m) || m.type != MsgType::Hello) continue;
        if (is_host_) {
            // The guest's ACK proves the link is two-way (poll() has now learned
            // the guest's address, so has_peer() holds) — the host is done.
            if (m.hello.is_ack) done_ = true;
        } else {
            // The host's announcement — adopt its seed and finish.
            if (!m.hello.is_ack) {
                seed_ = m.hello.seed;
                done_ = true;
            }
        }
    }
}

}  // namespace bomber::net
