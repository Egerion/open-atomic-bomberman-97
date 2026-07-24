#include "bomber/net/stun_client.hpp"

#include <utility>
#include <vector>

#include "bomber/net/lobby_messages.hpp"

namespace bomber::net {

namespace {
// Resend the probe this often while waiting (ms) — UDP, so a lost probe or
// reply must self-heal.
constexpr std::int64_t kProbeIntervalMs = 250;
}  // namespace

StunClient::StunClient(UdpTransport& transport, std::string server_host,
                       std::uint16_t server_port, std::string nonce, int timeout_ms)
    : transport_(transport),
      server_host_(std::move(server_host)),
      server_port_(server_port),
      nonce_(std::move(nonce)),
      timeout_ms_(timeout_ms) {}

std::string StunClient::reflexive_addr() const {
    if (state_ != State::Done) return {};
    return reflexive_ip_ + ":" + std::to_string(reflexive_port_);
}

void StunClient::step(std::int64_t now_ms) {
    if (state_ != State::Probing) return;
    if (start_ms_ < 0) start_ms_ = now_ms;

    if (last_probe_ms_ < 0 || now_ms - last_probe_ms_ >= kProbeIntervalMs) {
        const std::string probe = encode_stun_probe(nonce_);
        transport_.send_to(server_host_, server_port_,
                           reinterpret_cast<const std::uint8_t*>(probe.data()), probe.size());
        last_probe_ms_ = now_ms;
    }

    std::vector<std::uint8_t> buf;
    std::string src_ip;
    std::uint16_t src_port = 0;
    while (transport_.poll_from(&buf, &src_ip, &src_port)) {
        const std::string text(reinterpret_cast<const char*>(buf.data()), buf.size());
        std::string got_nonce;
        std::string addr;
        if (!parse_stun_reply(text, &got_nonce, &addr)) continue;  // not a reply → ignore
        if (got_nonce != nonce_) continue;                          // a stale attempt's reply
        std::string ip;
        std::uint16_t port = 0;
        if (!split_host_port(addr, &ip, &port)) continue;  // malformed → keep waiting
        reflexive_ip_ = ip;
        reflexive_port_ = port;
        state_ = State::Done;
        return;
    }

    if (now_ms - start_ms_ >= timeout_ms_) state_ = State::Failed;
}

}  // namespace bomber::net
