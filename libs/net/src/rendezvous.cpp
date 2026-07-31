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

Rendezvous::Rendezvous(UdpTransport& transport, std::vector<PeerSpec> peers,
                       std::uint32_t local_nonce, int timeout_ms)
    : transport_(transport), nonce_(local_nonce), timeout_ms_(timeout_ms), multi_(true) {
    peers_.reserve(peers.size());
    for (PeerSpec& p : peers) peers_.push_back(PeerState{std::move(p), {}, false, false, 0});
}

void Rendezvous::ping_candidates(const std::vector<Candidate>& targets,
                                 const std::vector<std::uint8_t>& ping) {
    for (const Candidate& c : targets) transport_.send_to(c.host, c.port, ping.data(), ping.size());
}

void Rendezvous::send_pings(std::int64_t now_ms) {
    const std::vector<std::uint8_t> ping = encode_punch(nonce_, /*is_pong=*/false);
    last_ping_ms_ = now_ms;
    if (!multi_) {
        ping_candidates(candidates_, ping);
        return;
    }
    for (const PeerState& p : peers_) {
        // Once a peer's real (post-NAT) address is known, aim there too — it is
        // the address that actually works, which a pre-shared candidate list may
        // not contain.
        if (p.addr_known) transport_.send_to(p.addr.host, p.addr.port, ping.data(), ping.size());
        ping_candidates(p.spec.candidates, ping);
    }
}

void Rendezvous::handle_ping(std::uint32_t nonce, const std::string& ip, std::uint16_t port) {
    // Echo every ping straight back to its true source (the reachable path), and
    // in the multi form use the nonce to learn WHICH SEAT is at that address —
    // every peer derives the same per-seat nonces from the shared seed.
    const std::vector<std::uint8_t> pong = encode_punch(nonce, /*is_pong=*/true);
    transport_.send_to(ip, port, pong.data(), pong.size());
    if (!multi_) return;
    for (PeerState& p : peers_) {
        if (p.spec.nonce != nonce) continue;
        p.addr = Candidate{ip, port};
        p.addr_known = true;
        break;
    }
}

void Rendezvous::handle_pong(std::uint32_t nonce, const std::string& ip, std::uint16_t port,
                             std::int64_t now_ms) {
    if (nonce != nonce_ || ip.empty()) return;  // not the echo of OUR ping
    const std::int64_t sent = last_ping_ms_ >= 0 ? last_ping_ms_ : now_ms;
    int rtt = static_cast<int>(now_ms - sent);
    if (rtt < 0) rtt = 0;

    if (!multi_) {
        winner_ = Candidate{ip, port};
        transport_.set_peer(ip, port);
        rtt_ms_ = rtt;
        winners_ = {Winner{-1, winner_, rtt}};
        state_ = State::Connected;
        return;
    }
    // Multi form: a peer is done only when we ALSO saw its own ping from this
    // same address (so both directions are proven, and the seat is known).
    for (PeerState& p : peers_) {
        if (!p.addr_known || p.addr.port != port || p.addr.host != ip) continue;
        if (p.confirmed) break;  // a duplicate pong keeps the first RTT sample
        p.confirmed = true;
        p.rtt_ms = rtt;
        break;
    }
    finish_if_all_confirmed();
}

void Rendezvous::finish_if_all_confirmed() {
    if (peers_.empty()) return;
    for (const PeerState& p : peers_)
        if (!p.confirmed) return;
    winners_.clear();
    winners_.reserve(peers_.size());
    int worst = 0;
    for (const PeerState& p : peers_) {
        winners_.push_back(Winner{p.spec.seat, p.addr, p.rtt_ms});
        if (p.rtt_ms > worst) worst = p.rtt_ms;
    }
    // The star's prediction window must cover the SLOWEST guest.
    rtt_ms_ = worst;
    state_ = State::Connected;
}

void Rendezvous::step(std::int64_t now_ms) {
    if (state_ != State::Punching) return;
    if (start_ms_ < 0) start_ms_ = now_ms;

    // (Re)send the ping fan-out on the first pump and every interval after —
    // punching simultaneously is what opens both NAT bindings.
    if (last_ping_ms_ < 0 || now_ms - last_ping_ms_ >= kPingIntervalMs) {
        send_pings(now_ms);
    }

    std::vector<std::uint8_t> buf;
    std::string src_ip;
    std::uint16_t src_port = 0;
    while (transport_.poll_from(&buf, &src_ip, &src_port)) {
        Message msg;
        if (!decode(buf.data(), buf.size(), &msg) || msg.type != MsgType::Punch) continue;
        if (!msg.punch.is_pong) {
            handle_ping(msg.punch.nonce, src_ip, src_port);
            continue;
        }
        handle_pong(msg.punch.nonce, src_ip, src_port, now_ms);
        if (state_ == State::Connected) return;
    }

    if (now_ms - start_ms_ >= timeout_ms_) state_ = State::Failed;
}

}  // namespace bomber::net
