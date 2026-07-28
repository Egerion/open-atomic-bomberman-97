#include "bomber/net/rematch_session.hpp"

#include <vector>

#include "bomber/net/protocol.hpp"

namespace bomber::net {

RematchSession::RematchSession(Transport& t, bool is_host, int timeout_ms)
    : transport_(&t), timeout_ms_(timeout_ms), is_host_(is_host) {}

void RematchSession::accept() {
    if (!is_host_) return;  // a guest may ask for nothing here; the host drives
    accepted_ = true;
    ready_ = true;         // the host leaves at once and keeps announcing on the way
    last_send_ms_ = -1;    // say so on this pump, not the next one
}

void RematchSession::drain(std::int64_t now_ms) {
    std::vector<std::uint8_t> pkt;
    while (transport_->poll(&pkt)) {
        Message m;
        if (!decode(pkt.data(), pkt.size(), &m)) continue;  // drop malformed (untrusted)
        // ANY well-formed datagram is proof the peer is there. That deliberately
        // includes stragglers from the round that just ended (a redundant input
        // window, a trailing hash): they are the peer, and treating them as
        // silence would be wrong. Draining them here is also what clears the
        // socket before the next SetupSession looks at it.
        last_rx_ms_ = now_ms;
        if (is_host_) continue;  // the host's own exit is its local player's call
        // TWO WAYS TO HEAR THE SAME THING, and the second is the SELF-HEAL. The
        // host's Rematch is the announcement; setup traffic is the host ALREADY
        // BEING in the setup stage, which can only mean the announcement was
        // lost (or went out while we were still on a screen). The implicit form
        // cannot be missed — the host re-broadcasts previews for as long as it
        // is on those screens — so it is the backstop the explicit one needs on
        // UDP. Consuming one preview costs nothing at that rate.
        const bool announced =
            m.type == MsgType::MatchCtl && m.match_ctl.kind == MatchCtlKind::Rematch;
        const bool already_there =
            m.type == MsgType::SetupPreview || m.type == MsgType::SetupChunk;
        if (announced || already_there) ready_ = true;
    }
}

void RematchSession::step(std::int64_t now_ms) {
    if (last_rx_ms_ < 0) last_rx_ms_ = now_ms;  // the clock starts at the first pump
    drain(now_ms);

    if (last_send_ms_ < 0 || now_ms - last_send_ms_ >= kRematchResendMs) {
        // The host announces: liveness while it is still reading the outcome
        // screens, the transition once it has accepted. A GUEST sends the
        // liveness kind too — not as an announcement (it decides nothing) but so
        // the host's own timeout means something, and so a host that reaches the
        // setup screens first is not left guessing.
        const MatchCtlKind kind =
            (is_host_ && accepted_) ? MatchCtlKind::Rematch : MatchCtlKind::RematchWait;
        const std::vector<std::uint8_t> pkt = encode_match_ctl(kind, 0);
        transport_->send(pkt.data(), pkt.size());
        last_send_ms_ = now_ms;
    }

    if (!ready_ && !failed_ && now_ms - last_rx_ms_ > static_cast<std::int64_t>(timeout_ms_))
        failed_ = true;
}

}  // namespace bomber::net
