#include "bomber/net/link_probe.hpp"

#include "bomber/net/protocol.hpp"

namespace bomber::net {

LinkProbe::LinkProbe(Transport& t, std::uint32_t local_nonce, int deadline_ms)
    : transport_(&t), nonce_(local_nonce), deadline_ms_(deadline_ms) {}

void LinkProbe::send_probe(std::int64_t now_ms) {
    const std::vector<std::uint8_t> p = encode_probe(nonce_, peer_seen_);
    transport_->send(p.data(), p.size());
    last_send_ms_ = now_ms;
}

void LinkProbe::on_message(const Message& msg, std::int64_t now_ms) {
    // ANSWER A PUNCH THAT IS STILL RUNNING. We may have reached this path while
    // the peer is still inside its own Rendezvous, and its PINGs now arrive here
    // rather than at a punch object. Echoing them is what stops our success from
    // starving its round trip.
    if (msg.type == MsgType::Punch && !msg.punch.is_pong) {
        const std::vector<std::uint8_t> pong = encode_punch(msg.punch.nonce, /*is_pong=*/true);
        transport_->send(pong.data(), pong.size());
        return;
    }
    if (msg.type != MsgType::Probe) return;  // not ours; the path carries something else
    if (msg.probe.nonce == nonce_) return;   // our own datagram, reflected back

    const bool was_seen = peer_seen_;
    const bool was_two_way = peer_saw_us_;
    peer_seen_ = true;
    if (msg.probe.seen_peer) peer_saw_us_ = true;
    // Answer IMMEDIATELY on a state change, so the exchange completes in ~1.5
    // round trips instead of waiting out two send intervals — but ONLY on a
    // change, or two peers would answer each other's answers forever at line
    // rate. Each side can therefore send at most two of these.
    if (!was_seen || (peer_saw_us_ && !was_two_way)) send_probe(now_ms);
}

void LinkProbe::latch_outcome(std::int64_t now_ms) {
    if (peer_seen_ && peer_saw_us_ && mutual_ms_ < 0) mutual_ms_ = now_ms;
    // MUTUAL PROOF IS TESTED FIRST, and the order is load-bearing: the linger
    // runs past the deadline on a slow path, so testing the deadline first would
    // expire a path that has already been proven to carry.
    if (mutual_ms_ >= 0) {
        if (now_ms - mutual_ms_ >= kProbeLingerMs) verified_ = true;
        return;
    }
    if (now_ms - start_ms_ >= deadline_ms_) expired_ = true;
}

void LinkProbe::step(std::int64_t now_ms) {
    if (verified_ || expired_) return;  // latched: the caller has already decided
    if (start_ms_ < 0) start_ms_ = now_ms;

    while (transport_->poll(&buf_)) {
        Message msg;
        if (decode(buf_.data(), buf_.size(), &msg)) on_message(msg, now_ms);
    }

    if (last_send_ms_ < 0 || now_ms - last_send_ms_ >= kProbeIntervalMs) send_probe(now_ms);
    latch_outcome(now_ms);
}

}  // namespace bomber::net
