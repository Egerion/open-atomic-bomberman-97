#include "bomber/net/rendezvous.hpp"

#include <utility>

#include "bomber/net/protocol.hpp"

namespace bomber::net {

namespace {
// Resend the ping fan-out this often while punching (ms). Frequent enough to
// punch quickly through a lossy path, sparse enough not to flood.
constexpr std::int64_t kPingIntervalMs = 200;
}  // namespace

Rendezvous::Rendezvous(UdpTransport& transport, std::vector<Candidate> peer_candidates,
                       std::uint32_t nonce, int timeout_ms)
    : transport_(transport),
      candidates_(std::move(peer_candidates)),
      nonce_(nonce),
      timeout_ms_(timeout_ms) {}

void Rendezvous::send_pings(std::int64_t now_ms) {
    const std::vector<std::uint8_t> ping = encode_punch(nonce_, /*is_pong=*/false);
    for (const Candidate& c : candidates_) {
        transport_.send_to(c.host, c.port, ping.data(), ping.size());
    }
    last_ping_ms_ = now_ms;
}

void Rendezvous::step(std::int64_t now_ms) {
    if (state_ != State::Punching) return;
    if (start_ms_ < 0) start_ms_ = now_ms;

    // (Re)send the ping fan-out on the first pump and every interval after —
    // punching simultaneously is what opens both NAT bindings.
    if (last_ping_ms_ < 0 || now_ms - last_ping_ms_ >= kPingIntervalMs) {
        send_pings(now_ms);
    }

    // Drain everything waiting: echo inbound PINGs as PONGs to their true source
    // (the post-NAT address, which is the real reachable path — not necessarily a
    // pre-shared candidate), and latch a PONG carrying OUR nonce as the winner.
    std::vector<std::uint8_t> buf;
    std::string src_ip;
    std::uint16_t src_port = 0;
    while (transport_.poll_from(&buf, &src_ip, &src_port)) {
        Message msg;
        if (!decode(buf.data(), buf.size(), &msg) || msg.type != MsgType::Punch) continue;
        if (!msg.punch.is_pong) {
            // A peer's PING reached us — echo it straight back to its source.
            const std::vector<std::uint8_t> pong = encode_punch(msg.punch.nonce, /*is_pong=*/true);
            transport_.send_to(src_ip, src_port, pong.data(), pong.size());
        } else if (msg.punch.nonce == nonce_ && !src_ip.empty()) {
            // Our own PING came back as a PONG: this source is reachable — done.
            winner_ = Candidate{src_ip, src_port};
            transport_.set_peer(src_ip, src_port);
            const std::int64_t sent = last_ping_ms_ >= 0 ? last_ping_ms_ : now_ms;
            rtt_ms_ = static_cast<int>(now_ms - sent);
            if (rtt_ms_ < 0) rtt_ms_ = 0;
            state_ = State::Connected;
            return;
        }
    }

    if (now_ms - start_ms_ >= timeout_ms_) state_ = State::Failed;
}

}  // namespace bomber::net
